/*************************************************************************
 * Copyright (c) 2015-2020, NVIDIA CORPORATION. All rights reserved.
 * Modifications Copyright (c) 2019-2021 Advanced Micro Devices, Inc. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#include "nccl.h"
#include "channel.h"
#include "nvmlwrap.h"
#include "bootstrap.h"
#include "transport.h"
#include "group.h"
#include "net.h"
#include "coll_net.h"
#include "enqueue.h"
#include "graph.h"
#include "argcheck.h"
#include <fcntl.h>
#include <unistd.h>
#include <hip/hip_runtime.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "graph/topo.h"

// [RCCL]
#include "clique/CliqueManager.h"
// [/RCCL]

#define STR2(v) #v
#define STR(v) STR2(v)

#ifdef ENABLE_TRACE
std::chrono::high_resolution_clock::time_point ncclEpoch;
#endif

#if CUDART_VERSION >= 9020 || defined(__HIP_PLATFORM_HCC__) || defined(__HCC__) || defined(__HIPCC__)
#define NCCL_GROUP_CUDA_STREAM 0 // CGMD: CUDA 9.2,10.X Don't need to use an internal CUDA stream
#else
#define NCCL_GROUP_CUDA_STREAM 1 // CGMD: CUDA 9.0,9.1 Need to use an internal CUDA stream
#endif

const char* ncclFuncStr[NCCL_NUM_FUNCTIONS+1] = { "Broadcast", "Reduce", "AllGather", "ReduceScatter", "AllReduce", "SendRecv" };
const char* ncclAlgoStr[NCCL_NUM_ALGORITHMS] = { "Tree", "Ring", "CollNet" };
const char* ncclProtoStr[NCCL_NUM_PROTOCOLS] = { "LL", "LL128", "Simple" };
const char* ncclRedOpStr[ncclNumOps] = { "Sum", "Prod", "Max", "Min" };
const char *ncclTypeStr[ncclNumTypes] = {"_i8", "_u8", "_i32", "_u32", "_i64", "_u64", "_f16", "_f32", "_f64", "_b16"};

NCCL_PARAM(GroupCudaStream, "GROUP_CUDA_STREAM", NCCL_GROUP_CUDA_STREAM);

NCCL_PARAM(CheckPointers, "CHECK_POINTERS", 0);

ncclNet_t* ncclNet = NULL;
ncclCollNet_t* ncclCollNet = NULL;

struct allocationTracker allocTracker[MAX_ALLOC_TRACK_NGPU] = {};

// Returns ncclInternalError if anything fails, causing that network to be ignored.
ncclResult_t initNet(ncclNet_t* net) {
  int ndev;
  if (net->init(ncclDebugLog) != ncclSuccess) return ncclInternalError;
  if (net->devices(&ndev) != ncclSuccess) return ncclInternalError;
  if (ndev <= 0) return ncclSystemError;
  return ncclSuccess;
}

ncclResult_t initCollNet(ncclCollNet_t* collnet) {
  int ndev;
  if (collnet->init(ncclDebugLog) != ncclSuccess) return ncclInternalError;
  if (collnet->devices(&ndev) != ncclSuccess) return ncclInternalError;
  if (ndev <= 0) return ncclSystemError;
  return ncclSuccess;
}

ncclResult_t initNetPlugin(ncclNet_t** net, ncclCollNet_t** collnet) {
  void* netPluginLib = dlopen("librccl-net.so", RTLD_NOW | RTLD_LOCAL);
  if (netPluginLib == NULL) {
    // dlopen does not guarantee to set errno, but dlerror only gives us a
    // string, so checking errno doesn't hurt to try to provide a better
    // error message
    if (errno == ENOENT) {
      INFO(NCCL_INIT|NCCL_NET, "NET/Plugin : No plugin found (librccl-net.so), using internal implementation");
    } else {
      INFO(NCCL_INIT|NCCL_NET, "NET/Plugin : Plugin load returned %d : %s.", errno, dlerror());
    }
    return ncclSuccess;
  }
  ncclNet_t* extNet = (ncclNet_t*) dlsym(netPluginLib, STR(NCCL_PLUGIN_SYMBOL));
  if (extNet == NULL) {
    INFO(NCCL_INIT|NCCL_NET, "NET/Plugin: Failed to find " STR(NCCL_PLUGIN_SYMBOL) " symbol.");
  } else if (initNet(extNet) == ncclSuccess) {
    *net = extNet;
    // Check for CollNet
    ncclCollNet_t* extCollNet = (ncclCollNet_t*) dlsym(netPluginLib, STR(NCCL_COLLNET_PLUGIN_SYMBOL));
    if (extCollNet == NULL) {
      INFO(NCCL_INIT|NCCL_NET, "NET/Plugin: Failed to find " STR(NCCL_COLLNET_PLUGIN_SYMBOL) " symbol.");
    } else if (initCollNet(extCollNet) == ncclSuccess) {
      *collnet = extCollNet;
    }
    return ncclSuccess;
  }
  if (netPluginLib != NULL) dlclose(netPluginLib);
  return ncclSuccess;
}

ncclResult_t initNet() {
  // Always initialize bootstrap network
  NCCLCHECK(bootstrapNetInit());

  NCCLCHECK(initNetPlugin(&ncclNet, &ncclCollNet));
  if (ncclNet != NULL) return ncclSuccess;
  if (initNet(&ncclNetIb) == ncclSuccess) {
    ncclNet = &ncclNetIb;
  } else {
    NCCLCHECK(initNet(&ncclNetSocket));
    ncclNet = &ncclNetSocket;
  }
  return ncclSuccess;
}

NCCL_PARAM(CollNetEnable, "COLLNET_ENABLE", 0);

pthread_mutex_t initLock = PTHREAD_MUTEX_INITIALIZER;
static bool initialized = false;
static ncclResult_t ncclInit() {
  if (initialized) return ncclSuccess;
  pthread_mutex_lock(&initLock);
  if (!initialized) {
    initEnv();
    NCCLCHECK(initNet());
    INFO(NCCL_INIT, "Using network %s", ncclNetName());
    initialized = true;
  }
  pthread_mutex_unlock(&initLock);
  return ncclSuccess;
}

NCCL_API(ncclResult_t, ncclGetVersion, int* version);
ncclResult_t ncclGetVersion(int* version) {
  if (version == NULL) return ncclInvalidArgument;
  *version = NCCL_VERSION_CODE;
  return ncclSuccess;
}

NCCL_API(ncclResult_t, ncclGetUniqueId, ncclUniqueId* out);
ncclResult_t ncclGetUniqueId(ncclUniqueId* out) {
  NCCLCHECK(ncclInit());
  NCCLCHECK(PtrCheck(out, "GetUniqueId", "out"));
  return bootstrapGetUniqueId(out);
}

// Prevent compiler from optimizing out these operations
#ifdef __clang__
#define NCCL_NO_OPTIMIZE __attribute__((optnone))
#else
#define NCCL_NO_OPTIMIZE __attribute__((optimize("O0")))
#endif

void NCCL_NO_OPTIMIZE commPoison(ncclComm_t comm) {
  comm->rank = comm->cudaDev = comm->busId = comm->nRanks = -1;
}

#ifdef ENABLE_COLLTRACE
void *ncclCommThreadMain(void *arg) {
  ncclComm_t comm = (ncclComm_t)arg;
  int head = comm->hostDevComm.collTraceHead;
  #define MAX_NAME_LENGTH 32
  char* func_names = (char *)malloc(MAX_NAME_LENGTH*(FUNC_INDEX_P2P+1));
  for (int func = 0; func < NCCL_NUM_FUNCTIONS; func++) {
    for (int al = 0; al < NCCL_NUM_ALGORITHMS; al++) {
      for (int type = 0; type < ncclNumTypes; type++) {
        for (int pr = 0; pr < NCCL_NUM_PROTOCOLS; pr++) {
          for (int redop = 0; redop < ncclNumOps; redop++) {
            char* line = func_names+MAX_NAME_LENGTH*FUNC_INDEX(func, redop, type, al, pr);
            sprintf(line, "%s%s%s%s%s", ncclFuncStr[func], ncclAlgoStr[al], ncclProtoStr[pr],
              ncclRedOpStr[redop], ncclTypeStr[type]);
          }
        }
      }
    }
  }
  char* line = func_names+MAX_NAME_LENGTH*FUNC_INDEX_P2P;
  sprintf(line, "%s", ncclFuncStr[NCCL_NUM_FUNCTIONS]);
  do {
    int tail = LOAD(comm->hostDevComm.collTraceTail)%COLLTRACE_NUM_ITEMS;
    int count;
    if (head <= tail)
      count = tail - head;
    else
      count = COLLTRACE_NUM_ITEMS + head - tail;
    if (!count) {
      if(LOAD(&comm->hostDevComm.collTraceExit))
        break;
      else {
        usleep(1000); //sleep 1ms
        continue;
      }
    }
    for (int i = 0; i < count; i++) {
      struct ncclCollTrace *td = comm->hostDevComm.collTrace+head;
      uint8_t type = LOAD(&(td->type));
      if (type == ncclCollTraceNotReady)
        break;
      char line[1024];
      int offset = 0;
      uint16_t fIdx = td->funcIndex;
      #define VEGA_GPU_RTC_FREQUENCY 2.5E7
      if (type == ncclCollTraceDataType) {
        sprintf(line, "## [%12.6f] [%02d:%02d] L:%04d DT %08x %016lx %016lx",
          (double)(td->timeStamp)/VEGA_GPU_RTC_FREQUENCY, comm->rank, td->bid,
          fIdx, td->data_0, td->opCount, td->data_1);
      } else {
        sprintf(line, "## [%12.6f] [%02d:%02d] %06lx",
          (double)(td->timeStamp)/VEGA_GPU_RTC_FREQUENCY, comm->rank, td->bid, td->opCount);
        offset = strlen(line);
        switch (type) {
          case ncclCollTraceKernelLaunchType:
            sprintf(line+offset, " KL HWID %8x %s ",
              td->data_0, func_names+MAX_NAME_LENGTH*fIdx);
            offset = strlen(line);
            if (fIdx > FUNC_INDEX_P2P)
              sprintf(line+offset, "ERROR bad function index %d", fIdx);
            else if (fIdx == FUNC_INDEX_P2P)
              sprintf(line+offset, "nt %d dt %d", td->p2p.nThreads, td->p2p.delta);
            else
              sprintf(line+offset, "nt %d bi %d nc %d", td->coll.nThreads, td->coll.bid, td->coll.nChannels);
            break;
          case ncclCollTraceCollEndType:
            if (fIdx != 0xffff) {
              sprintf(line+offset, " CE %s ", func_names+MAX_NAME_LENGTH*fIdx);
              offset = strlen(line);
              if (fIdx > FUNC_INDEX_P2P)
                sprintf(line+offset, "ERROR bad function index %d", fIdx);
              else if (fIdx == FUNC_INDEX_P2P)
                sprintf(line+offset, "nt %d dt %d", td->p2p.nThreads, td->p2p.delta);
              else
                sprintf(line+offset, "nt %d bi %d nc %d", td->coll.nThreads, td->coll.bid, td->coll.nChannels);
            }
            else
              sprintf(line+offset, " KE");
            break;
          case ncclCollTraceAbortType:
            sprintf(line+offset, " Abort");
            break;
          default:
            sprintf(line+offset, " unknown collective trace data type");
            break;
        }
      }
      INFO(NCCL_COLL, "%s", line);
      STORE(&(td->type), ncclCollTraceNotReady);
      head ++;
      head %= COLLTRACE_NUM_ITEMS;
    }
  } while(1);
  free(func_names);
  comm->hostDevComm.collTraceHead = head;
  pthread_exit(NULL);
}
#endif

#undef NCCL_NO_OPTIMIZE

static ncclResult_t commFree(ncclComm_t comm) {
  if (comm == NULL)
    return ncclSuccess;
  free(comm->connectSend);
  free(comm->connectRecv);
  free(comm->p2pSends);
  free(comm->p2pRecvs);
  free(comm->asyncOps);

#ifdef ENABLE_PROFILING
  struct ncclProf* prof = (struct ncclProf*)malloc(sizeof(struct ncclProf));
  CUDACHECK(hipMemcpy(prof, comm->hostDevComm.devProf, sizeof(struct ncclProf), hipMemcpyDeviceToHost));
  uint64_t wait_cycle = 0, wait_recv_cycle = 0;
  for (int chan=0; chan<comm->nChannels; chan++) {
    wait_cycle += prof->wait_cycle[chan];
    wait_recv_cycle += prof->wait_recv_cycle[chan];
  }
  #define VEGA_GPU_RTC_FREQUENCY 2.5E7
  if (comm->rank == 0) {
    INFO(NCCL_INIT, "# %4s %6s %6s %6s %6s %6s %7s %6s %6s %6s %6s %6s", "Rank", "total", "  wait", "w_recv", "send", "rcRdS", "dRcRdCS", "dRcCS", "dRc", "cS", "rc", "rcCS");
    INFO(NCCL_INIT, "# %4s %6s %6s %6s %6s %6s %7s %6s %6s %6s %6s %6s", "", "(s)", "(s)", "(s)", "(GB/s)", "(GB/s)", "(GB/s)", "(GB/s)", "(GB/s)", "(GB/s)", "(GB/s)", "(GB/s)");
  }
  INFO(NCCL_INIT, "# %4d %6.4f %6.4f %6.4f %6.2f %6.2f %7.2f %6.2f %6.2f %6.2f %6.2f %6.2f",
    comm->rank, (double)prof->total_cycle/VEGA_GPU_RTC_FREQUENCY/comm->nChannels,
    (double)wait_cycle/VEGA_GPU_RTC_FREQUENCY/comm->nChannels,
    (double)wait_recv_cycle/VEGA_GPU_RTC_FREQUENCY/comm->nChannels,
    (prof->send_cycle) ? (double)prof->send_byte*comm->nChannels/((double)prof->send_cycle/VEGA_GPU_RTC_FREQUENCY*1.0E9) : 0,
    (prof->recvReduceSend_cycle) ? (double)prof->recvReduceSend_byte*comm->nChannels/((double)prof->recvReduceSend_cycle/VEGA_GPU_RTC_FREQUENCY*1.0E9) : 0,
    (prof->directRecvReduceCopySend_cycle) ? (double)prof->directRecvReduceCopySend_byte*comm->nChannels/((double)prof->directRecvReduceCopySend_cycle/VEGA_GPU_RTC_FREQUENCY*1.0E9) : 0,
    (prof->directRecvCopySend_cycle) ? (double)prof->directRecvCopySend_byte*comm->nChannels/((double)prof->directRecvCopySend_cycle/VEGA_GPU_RTC_FREQUENCY*1.0E9) : 0,
    (prof->directRecv_cycle) ? (double)prof->directRecv_byte*comm->nChannels/((double)prof->directRecv_cycle/VEGA_GPU_RTC_FREQUENCY*1.0E9) : 0,
    (prof->copySend_cycle) ? (double)prof->copySend_byte*comm->nChannels/((double)prof->copySend_cycle/VEGA_GPU_RTC_FREQUENCY*1.0E9) : 0,
    (prof->recv_cycle) ? (double)prof->recv_byte*comm->nChannels/((double)prof->recv_cycle/VEGA_GPU_RTC_FREQUENCY*1.0E9) : 0,
    (prof->recvCopySend_cycle) ? (double)prof->recvCopySend_byte*comm->nChannels/((double)prof->recvCopySend_cycle/VEGA_GPU_RTC_FREQUENCY*1.0E9) : 0);
  free(prof);
  CUDACHECK(hipFree(comm->hostDevComm.devProf));

  for (int channel=0; channel<std::max(comm->nChannels, comm->p2pnChannels); channel++) {
    if (comm->channels[channel].send_byte) INFO(NCCL_INIT, "# [%03d:%02d] Proxy Send %6.2f GB/s (%ld bytes %d measurements)",
      comm->rank, channel, (comm->channels[channel].bw_count) ?
      (float)comm->channels[channel].bw_cumulative/comm->channels[channel].bw_count : 0,
      comm->channels[channel].send_byte, comm->channels[channel].bw_count);
    if (comm->channels[channel].recv_byte) INFO(NCCL_INIT, "# [%03d:%02d] Proxy Recv %6.2f GB/s (%ld bytes %d measurements)",
      comm->rank, channel, (comm->channels[channel].bw_count) ?
      (float)comm->channels[channel].bw_cumulative/comm->channels[channel].bw_count : 0,
      comm->channels[channel].recv_byte, comm->channels[channel].bw_count);
  }
#endif

#ifdef ENABLE_COLLTRACE
  STORE(&comm->hostDevComm.collTraceExit, 1);
  if (comm->hostDevComm.collTraceThread) pthread_join(comm->hostDevComm.collTraceThread, NULL);
  NCCLCHECK(ncclCudaHostFree((void *)comm->hostDevComm.collTrace));
  NCCLCHECK(ncclCudaHostFree((void *)comm->hostDevComm.collTraceTail));
#endif

  free(comm->peerInfo);
  ncclTopoFree(comm->topo);

  if (comm->bootstrap)
    NCCLCHECK(bootstrapClose(comm->bootstrap));

  CUDACHECK(hipFree(comm->hostDevComm.channels));
  CUDACHECK(hipFree(comm->devComm));

  for (int channel=0; channel<MAXCHANNELS; channel++)
    NCCLCHECK(freeChannel(comm->channels+channel, comm->nRanks));

  if (comm->doneEvent != NULL)
    CUDACHECK(hipEventDestroy(comm->doneEvent));

  if (comm->launchMode == ncclComm::GROUP) {
    CUDACHECK(hipStreamDestroy(comm->groupStream));
  }

  // Last rank frees shared resources between threads
  int isLast;
  NCCLCHECK(ncclCpuBarrierIn(comm, &isLast));
  if (isLast) {
    free(comm->intraBarrier);
    free(comm->intraParams);
    free(comm->intraCudaDevs);
    free(comm->intraCGMode);
    free(comm->intraCC);
  }
  NCCLCHECK(ncclCudaHostFree((void *)comm->abortFlag));
  NCCLCHECK(ncclCudaHostFree((void *)comm->p2pNet));

  // Poison comm to try and catch a double free
  commPoison(comm);

  free(comm);
  return ncclSuccess;
}

RCCL_PARAM(ForceEnableClique, "FORCE_ENABLE_CLIQUE", 0);
RCCL_PARAM(P2pNetDisable, "P2P_NET_DISABLE", 0);

static ncclResult_t commAlloc(ncclComm_t* comret, int ndev, int rank) {
  if (ndev < 1) {
    WARN("invalid device count (%d) requested", ndev);
    return ncclInvalidArgument;
  }
  if (rank >= ndev || rank < 0) {
    WARN("rank %d exceeds ndev=%d", rank, ndev);
    return ncclInvalidArgument;
  }

  // Try to create a CUDA object right away. If there is something wrong with
  // the device we're on (failure cause #1) , better know it early.
  hipEvent_t doneEvent;
  CUDACHECK(hipEventCreateWithFlags(&doneEvent, hipEventDisableTiming));

  struct ncclComm* comm;
  NCCLCHECK(ncclCalloc(&comm, 1));

  comm->rank = comm->hostDevComm.rank = rank;
  comm->nRanks = comm->hostDevComm.nRanks = ndev;
  hipGetDevice(&comm->cudaDev);
  NCCLCHECK(getBusId(comm->cudaDev, &comm->busId));
  TRACE(NCCL_INIT,"comm %p rank %d nranks %d cudaDev %d busId %lx", comm, rank, ndev, comm->cudaDev, comm->busId);

  comm->doneEvent = doneEvent;
  comm->checkPointers = ncclParamCheckPointers() == 1 ? true : false;
#if CUDART_VERSION >= 9020 || defined(__HIP_PLATFORM_HCC__) || defined(__HCC__) || defined(__HIPCC__)
  comm->groupCudaStream = ncclParamGroupCudaStream();
#else
  // Don't allow the user to overload the default setting in older CUDA builds
  comm->groupCudaStream = NCCL_GROUP_CUDA_STREAM;
#endif
  comm->fatalError = ncclSuccess;

  NCCLCHECK(ncclCudaHostCalloc((uint32_t**)&comm->abortFlag, 1));
  comm->hostDevComm.abortFlag = comm->abortFlag;
  STORE(comm->abortFlag, 0);

  NCCLCHECK(ncclCudaHostCalloc((uint32_t**)&comm->p2pNet, 1));
  comm->hostDevComm.p2pNet = comm->p2pNet;
  STORE(comm->p2pNet, 0);

  comm->argsptr = &comm->args;
#ifdef ENABLE_PROFILING
  NCCLCHECK(ncclCudaCalloc(&comm->hostDevComm.devProf, 1));
#endif

#ifdef ENABLE_COLLTRACE
  NCCLCHECK(ncclCudaHostCalloc(&comm->hostDevComm.collTraceTail, 1));
  NCCLCHECK(ncclCudaHostCalloc(&comm->hostDevComm.collTrace, COLLTRACE_NUM_ITEMS));
  memset(comm->hostDevComm.collTrace, 0, sizeof(struct ncclCollTrace) * COLLTRACE_NUM_ITEMS);
  comm->hostDevComm.collTraceExit = comm->hostDevComm.collTraceHead = *comm->hostDevComm.collTraceTail = 0;
  if ((ncclDebugLevel >= NCCL_LOG_INFO) && (ncclDebugMask & NCCL_COLL))
    pthread_create(&comm->hostDevComm.collTraceThread, NULL, ncclCommThreadMain, (void *)comm);
  else
    comm->hostDevComm.collTraceThread = 0;
#endif
  comm->collNetSupport = 0;

  NCCLCHECK(ncclCalloc(&comm->asyncOps, NCCL_MAX_OPS));
  comm->asyncOpCount = 0;
  comm->asyncTotalSize = 0;

  static_assert(MAXCHANNELS <= sizeof(*comm->connectSend)*8, "comm->connectSend must have enough bits for all channels");
  static_assert(MAXCHANNELS <= sizeof(*comm->connectRecv)*8, "comm->connectRecv must have enough bits for all channels");
  NCCLCHECK(ncclCalloc(&comm->connectSend, comm->nRanks));
  NCCLCHECK(ncclCalloc(&comm->connectRecv, comm->nRanks));

  comm->p2pSendCount = comm->p2pRecvCount = 0;
  NCCLCHECK(ncclCalloc(&comm->p2pSends, comm->nRanks));
  NCCLCHECK(ncclCalloc(&comm->p2pRecvs, comm->nRanks));

  // Mark channels as non initialized.
  for (int c=0; c<MAXCHANNELS; c++) comm->channels[c].id = -1;

  *comret = comm;
  return ncclSuccess;
}

static ncclResult_t devCommSetup(ncclComm_t comm) {
  // Duplicate the channels on the device
  NCCLCHECK(ncclCudaCalloc(&comm->hostDevComm.channels, std::max(comm->nChannels, comm->p2pnChannels)));
  NCCLCHECK(ncclCudaMemcpy(comm->hostDevComm.channels, comm->channels, std::max(comm->nChannels, comm->p2pnChannels)));

  // Copy userRanks and peers
  for (int r=0; r<std::max(comm->nChannels, comm->p2pnChannels); r++) {
    NCCLCHECK(ncclCudaMemcpy(comm->channels[r].ring.devUserRanks, comm->channels[r].ring.userRanks, comm->nRanks));
  }

  // Duplicate the dev comm on the device
  NCCLCHECK(ncclCudaCalloc(&comm->devComm, 1));
  NCCLCHECK(ncclCudaMemcpy(comm->devComm, &comm->hostDevComm, 1));
  return ncclSuccess;
}

// Pre-process the string so that running "strings" on the lib can quickly reveal the version.
#if defined(__HIP_PLATFORM_HCC__) || defined(__HCC__) || defined(__HIPCC__)
#define VERSION_STRING "RCCL version " STR(NCCL_MAJOR) "." STR(NCCL_MINOR) "." STR(NCCL_PATCH) NCCL_SUFFIX "+hip" STR(HIP_VERSION_MAJOR) "." STR(HIP_VERSION_MINOR)
#else
#define VERSION_STRING "NCCL version " STR(NCCL_MAJOR) "." STR(NCCL_MINOR) "." STR(NCCL_PATCH) NCCL_SUFFIX "+cuda" STR(CUDA_MAJOR) "." STR(CUDA_MINOR)
#endif
static void showVersion() {
  static int shown = 0;
  if (shown == 0 && ncclDebugLevel >= NCCL_LOG_VERSION) {
    printf("%s\n", VERSION_STRING);
    fflush(stdout);
    if (ncclDebugFile != stdout)
      INFO(NCCL_ALL,"%s", VERSION_STRING); // Also log NCCL version in one of the files
    shown = 1;
  }
}

static ncclResult_t fillInfo(struct ncclComm* comm, struct ncclPeerInfo* info, uint64_t commHash) {
  info->rank = comm->rank;
  CUDACHECK(hipGetDevice(&info->cudaDev));
  info->hostHash=getHostHash()+commHash;
  info->pidHash=getPidHash()+commHash;

  // Get the device MAJOR:MINOR of /dev/shm so we can use that
  // information to decide whether we can use SHM for inter-process
  // communication in a container environment
  struct stat statbuf;
  SYSCHECK(stat("/dev/shm", &statbuf), "stat");
  info->shmDev = statbuf.st_dev;

  info->busId = comm->busId;

  NCCLCHECK(ncclGpuGdrSupport(&info->gdrSupport));
  return ncclSuccess;
}

static ncclResult_t setupChannel(struct ncclComm* comm, int channelId, int rank, int nranks, int* ringRanks) {
  TRACE(NCCL_INIT, "rank %d nranks %d", rank, nranks);
  NCCLCHECK(initChannel(comm, channelId));

  struct ncclRing* ring = &comm->channels[channelId].ring;
  // Reorganize ranks to start with rank.
  int shift;
  for (shift = 0; shift<nranks; shift++) {
    if (ringRanks[shift] == rank) {
      break;
    }
  }
  for (int i=0; i<nranks; i++) {
    ring->userRanks[i] = ringRanks[(i+shift)%nranks];
  }
  return ncclSuccess;
}

void* waitForNonNullPtr(void* p) {
  volatile void** ptr = (volatile void**) p;
  while (LOAD(ptr) == NULL) sched_yield();
  return (void*)(LOAD(ptr));
}

ncclResult_t initParams(struct ncclComm* comm) {
  hipLaunchParams* params = comm->myParams = comm->intraParams+comm->intraRank;
  params->args = (void **)&comm->argsptr;
  params->stream = NULL;
  params->sharedMem = 0;
  params->blockDim.x = 0; params->blockDim.y = params->blockDim.z = 1;
  params->gridDim.x = 0; params->gridDim.y = params->gridDim.z = 1;
  return ncclSuccess;
}

// Allocate/Set Intra Process Structures and set CG options
ncclResult_t ncclCommSetIntra(struct ncclComm* comm, int rank, int ranks, struct ncclComm* comm0) {
  comm->intraRank = rank;
  comm->intraRanks = ranks;
  comm->intraPhase = 0;

  // Alloc shared structures
  if (rank == 0) {
    assert(comm == comm0);
    int* bar;
    NCCLCHECK(ncclCalloc(&bar, 2));
    bar[0] = bar[1] = 0;
    comm->intraBarrier = bar;
    NCCLCHECK(ncclCalloc(&comm->intraParams, comm->intraRanks));
    NCCLCHECK(ncclCalloc(&comm->intraCudaDevs, comm->intraRanks));
    int* CGMode;
    NCCLCHECK(ncclCalloc(&CGMode, 1));
    *CGMode = 0x11;
    comm->intraCGMode = CGMode;
    int* CC;
    NCCLCHECK(ncclCalloc(&CC, 1));
    *CC = ncclCudaCompCap();
    comm->intraCC = CC;
  } else {
    comm->intraBarrier = (int*)waitForNonNullPtr(&comm0->intraBarrier);
    comm->intraParams = (hipLaunchParams*)waitForNonNullPtr(&comm0->intraParams);
    comm->intraCudaDevs = (int*)waitForNonNullPtr(&comm0->intraCudaDevs);
    comm->intraCGMode = (int*)waitForNonNullPtr(&comm0->intraCGMode);
    comm->intraCC = (int*)waitForNonNullPtr(&comm0->intraCC);
  }
  comm->intraCudaDevs[comm->intraRank] = comm->cudaDev;
  NCCLCHECK(initParams(comm));

  int cgMdLaunch = 1;

  // Set CG Mode
  comm->launchMode = ncclComm::GROUP;
  char* str = getenv("NCCL_LAUNCH_MODE");
  if (str) INFO(NCCL_ENV, "NCCL_LAUNCH_MODE set by environment to %s", str);
  if (comm->intraRanks == 1 || (str && strcmp(str, "PARALLEL") == 0)) {
    comm->launchMode = ncclComm::PARALLEL;
  }
  if (comm->launchMode == ncclComm::GROUP) {
    CUDACHECK(hipStreamCreateWithFlags(&comm->groupStream, hipStreamNonBlocking));
#if CUDART_VERSION >= 9000
    if (*comm->intraCC && (ncclCudaCompCap() == *comm->intraCC)) {
      // Check whether the GPU supports Cooperative Group Multi Device Launch
      (void) hipDeviceGetAttribute(&cgMdLaunch, cudaDevAttrCooperativeMultiDeviceLaunch, comm->cudaDev);
    }
#endif
  }

  // Disable cgMdLaunch if any rank does not support it
  if (cgMdLaunch == 0) {
    *comm->intraCGMode = 0x10;
  }
  return ncclSuccess;
}

#define DEFAULT_LL_BUFFSIZE (NCCL_LL_LINES_PER_THREAD*NCCL_LL_MAX_NTHREADS*NCCL_STEPS*sizeof(union ncclLLFifoLine))
#define DEFAULT_LL128_BUFFSIZE (NCCL_LL128_ELEMS_PER_THREAD*NCCL_LL128_MAX_NTHREADS*NCCL_STEPS*sizeof(uint64_t))
#define DEFAULT_BUFFSIZE (1 << 22) /* 4MiB */
#define DEFAULT_BUFFSIZE_ARM (1 << 20) /* 1MiB */
NCCL_PARAM(BuffSize, "BUFFSIZE", -2);
NCCL_PARAM(LlBuffSize, "LL_BUFFSIZE", -2);
NCCL_PARAM(Ll128BuffSize, "LL128_BUFFSIZE", -2);

static ncclResult_t computeBuffSizes(struct ncclComm* comm) {
  int cpuArch, cpuVendor, cpuModel;
  NCCLCHECK(ncclTopoCpuType(comm->topo, &cpuArch, &cpuVendor, &cpuModel));

  int64_t envs[NCCL_NUM_PROTOCOLS] = { ncclParamLlBuffSize(), ncclParamLl128BuffSize(), ncclParamBuffSize() };
  int defaults[NCCL_NUM_PROTOCOLS] = { DEFAULT_LL_BUFFSIZE, DEFAULT_LL128_BUFFSIZE, DEFAULT_BUFFSIZE };

  if (cpuArch == NCCL_TOPO_CPU_ARCH_ARM) defaults[NCCL_PROTO_SIMPLE] = DEFAULT_BUFFSIZE_ARM;

  for (int p=0; p<NCCL_NUM_PROTOCOLS; p++) {
    comm->buffSizes[p] = comm->hostDevComm.buffSizes[p] = envs[p] != -2 ? envs[p] : defaults[p];
  }
  return ncclSuccess;
}

extern struct ncclTransport collNetTransport;

// All ranks must participate in collNetSetup call
// type: 0 for send, 1 for recv
// return: 0 - unsupported, 1 - supported
// We do not NCCLCHECK this call because we would fall back to P2P network in case CollNet setup fails
static int collNetSetup(struct ncclComm* comm, struct ncclTopoGraph* collNetGraph, struct ncclChannel* channel, int rank, int nranks,  int masterRank, int masterPeer, int nMasters, int type) {
  int rankInCollNet = -1;
  int supported = 0;
  int isMaster = (rank == masterRank) ? 1 : 0;
  struct {
    int collNetRank;
    ncclConnect connect;
  } sendrecvExchange;

  // check if we can connect to collnet, whose root is the nranks-th rank
  struct ncclPeerInfo *myInfo = comm->peerInfo+rank, *peerInfo = comm->peerInfo+nranks;
  peerInfo->rank = nranks;
  int ret = 1;
  if (isMaster) {
    NCCLCHECK(collNetTransport.canConnect(&ret, comm->topo, collNetGraph, myInfo, peerInfo));
  }

  // send master receives connect info from peer recv master
  if (isMaster && type == 0) {
    NCCLCHECK(bootstrapRecv(comm->bootstrap, masterPeer, &sendrecvExchange, sizeof(sendrecvExchange)));
    rankInCollNet = sendrecvExchange.collNetRank;
    INFO(NCCL_INIT, "CollNet [send] : rank %d collNetRank %d collNetNranks %d received connect from rank %d", rank, rankInCollNet, nMasters, masterPeer);
  }

  // select
  struct ncclPeer* root = channel->peers+nranks;
  struct ncclConnector* conn = (type == 1) ? &root->recv : &root->send;
  struct ncclTransportComm* transportComm = (type == 1) ? &(collNetTransport.recv) : &(collNetTransport.send);
  conn->transportComm = transportComm;
  // setup
  struct ncclConnect myConnect;
  if (isMaster && ret > 0) {
    NCCLCHECK(transportComm->setup(comm, collNetGraph, myInfo, peerInfo, &myConnect, conn, channel->id));
  }
  // prepare connect handles
  ncclResult_t res;
  struct {
    int isMaster;
    ncclConnect connect;
  } *allConnects = NULL;
  ncclConnect *masterConnects = NULL;
  NCCLCHECK(ncclCalloc(&masterConnects, nMasters));
  if (type == 1) {  // recv side: AllGather
    // all ranks must participate
    NCCLCHECK(ncclCalloc(&allConnects, nranks));
    allConnects[rank].isMaster = isMaster;
    memcpy(&(allConnects[rank].connect), &myConnect, sizeof(struct ncclConnect));
    NCCLCHECKGOTO(bootstrapAllGather(comm->bootstrap, allConnects, sizeof(*allConnects)), res, cleanup);
    // consolidate
    int c = 0;
    for (int r = 0; r < nranks; r++) {
      if (allConnects[r].isMaster) {
        memcpy(masterConnects+c, &(allConnects[r].connect), sizeof(struct ncclConnect));
        if (r == rank) rankInCollNet = c;
        c++;
      }
    }
  } else { // send side : copy in connect info received from peer recv master
    if (isMaster) memcpy(masterConnects+rankInCollNet, &(sendrecvExchange.connect), sizeof(struct ncclConnect));
  }
  // connect
  if (isMaster && ret > 0) {
    NCCLCHECKGOTO(transportComm->connect(comm, masterConnects, nMasters, rankInCollNet, conn), res, cleanup);
    struct ncclPeer* devRoot = channel->devPeers+nranks;
    struct ncclConnector* devConn = (type == 1) ? &devRoot->recv : &devRoot->send;
    CUDACHECKGOTO(hipMemcpy(devConn, conn, sizeof(struct ncclConnector), hipMemcpyHostToDevice), res, cleanup);
  }
  // recv side sends connect info to send side
  if (isMaster && type == 1) {
    sendrecvExchange.collNetRank = rankInCollNet;
    memcpy(&sendrecvExchange.connect, masterConnects+rankInCollNet, sizeof(struct ncclConnect));
    NCCLCHECKGOTO(bootstrapSend(comm->bootstrap, masterPeer, &sendrecvExchange, sizeof(sendrecvExchange)), res, cleanup);
    INFO(NCCL_INIT, "CollNet [recv] : rank %d collNetRank %d collNetNranks %d sent connect to rank %d", rank, rankInCollNet, nMasters, masterPeer);
  }
  if (ret > 0) {
    supported = 1;
  }
cleanup:
  if (allConnects != NULL) free(allConnects);
  if (masterConnects != NULL) free(masterConnects);
  return supported;
}

static ncclResult_t checkCollNetSetup(struct ncclComm* comm, int rank, int collNetSetupFail) {
  int nranks = comm->nRanks;
  // AllGather collNet setup results
  int* allGatherFailures;
  NCCLCHECK(ncclCalloc(&allGatherFailures, nranks));
  allGatherFailures[rank] = collNetSetupFail;
  NCCLCHECK(bootstrapAllGather(comm->bootstrap, allGatherFailures, sizeof(int)));
  for (int i=0; i<nranks; i++) {
    if (allGatherFailures[i] != 0) {
      collNetSetupFail = 1;
      break;
    }
  }
  free(allGatherFailures);
  if (collNetSetupFail) {
    if (rank == 0) WARN("Cannot initialize CollNet, using %s instead", ncclNetName());
    // Free collNet resources
    for (int r=0; r<comm->nChannels; r++) {
      struct ncclChannel* channel = comm->channels+r;
      struct ncclPeer* peer = channel->peers+nranks;
      if (peer->send.transportResources && peer->send.transportComm) NCCLCHECK(peer->send.transportComm->free(peer->send.transportResources));
      if (peer->recv.transportResources && peer->recv.transportComm) NCCLCHECK(peer->recv.transportComm->free(peer->recv.transportResources));
      peer->send.transportResources = NULL; // avoid double free
      peer->recv.transportResources = NULL; // avoid double free
    }
    // Set support to 0
    comm->collNetSupport = 0;
  } else {
    comm->collNetSupport = 1;
  }
  return ncclSuccess;
}

NCCL_PARAM(CrossNic, "CROSS_NIC", 2);
NCCL_PARAM(GraphDumpFileRank, "GRAPH_DUMP_FILE_RANK", 0);

static ncclResult_t initTransportsRank(struct ncclComm* comm, ncclUniqueId* commId) {
  // We use 2 AllGathers
  // 1. { peerInfo, comm, compCap}
  // 2. { nChannels, graphInfo, topoRanks }

  int rank = comm->rank;
  int nranks = comm->nRanks;
  uint64_t commHash = getHash(commId->internal, NCCL_UNIQUE_ID_BYTES);
  TRACE(NCCL_INIT, "comm %p, commHash %lx, rank %d nranks %d - BEGIN", comm, commHash, rank, nranks);
  // [RCCL] Collect the PID of the root
  int rootPid;
  NCCLCHECK(bootstrapInit(commId, rank, nranks, &comm->bootstrap, &rootPid));
  // [/RCCL]

  // AllGather1 - begin
  struct {
    struct ncclPeerInfo peerInfo;
    struct ncclComm* comm;
    int cudaCompCap;
  } *allGather1Data;

  NCCLCHECK(ncclCalloc(&allGather1Data, nranks));
  allGather1Data[rank].comm = comm;
  allGather1Data[rank].cudaCompCap = ncclCudaCompCap();
  struct ncclPeerInfo* myInfo = &allGather1Data[rank].peerInfo;
  NCCLCHECK(fillInfo(comm, myInfo, commHash));
  NCCLCHECK(bootstrapAllGather(comm->bootstrap, allGather1Data, sizeof(*allGather1Data)));

  NCCLCHECK(ncclCalloc(&comm->peerInfo, nranks+1)); // Extra rank to represent CollNet root
  for (int i = 0; i < nranks; i++) {
    memcpy(comm->peerInfo+i, &allGather1Data[i].peerInfo, sizeof(struct ncclPeerInfo));
    if ((i != rank) && (comm->peerInfo[i].hostHash == myInfo->hostHash) && (comm->peerInfo[i].busId == myInfo->busId)) {
      WARN("Duplicate GPU detected : rank %d and rank %d both on CUDA device %lx", rank, i, myInfo->busId);
      return ncclInvalidUsage;
    }
  }

  // Compute intra ranks and minimum CUDA Compute capabilities of intra-node GPUs and all GPUs
  int intraRank0 = -1, intraRank = -1, intraRanks = 0;
  int myCompCap = allGather1Data[rank].cudaCompCap;
  int minCompCap = myCompCap, maxCompCap = myCompCap;
  uint64_t otherHostHash;
  int tmpNnodes = 1;
  for (int i = 0; i < nranks; i++) {
    if (allGather1Data[i].peerInfo.hostHash == allGather1Data[rank].peerInfo.hostHash) {
      if (allGather1Data[i].peerInfo.pidHash == allGather1Data[rank].peerInfo.pidHash) {
        if (intraRanks == 0) intraRank0 = i;
        if (i == rank) intraRank = intraRanks;
        intraRanks++;
      }
    } else {  // Determine whether number of nodes is 2 (for use in tree pattern determination)
      if (tmpNnodes == 1) {
        otherHostHash = allGather1Data[i].peerInfo.hostHash;
        tmpNnodes = 2;
      } else if (tmpNnodes == 2 && otherHostHash != allGather1Data[i].peerInfo.hostHash) {
        tmpNnodes = 3;
      }
    }
    minCompCap = std::min(allGather1Data[i].cudaCompCap, minCompCap);
    maxCompCap = std::max(allGather1Data[i].cudaCompCap, maxCompCap);
  }
  TRACE(NCCL_INIT,"hostHash[%d] %lx intraRank %d intraRanks %d intraRank0 %d",
        rank, allGather1Data[rank].peerInfo.hostHash, intraRank, intraRanks, intraRank0);
  if (intraRank == -1 || intraRank0 == -1 || allGather1Data[intraRank0].comm == NULL) {
    WARN("Failed to determine intra ranks hostHash[%d] %lx intraRank %d intraRanks %d intraRank0 %d",
         rank, allGather1Data[rank].peerInfo.hostHash, intraRank, intraRanks, intraRank0);
    return ncclInternalError;
  }
  struct ncclComm* intraRank0Comm = allGather1Data[intraRank0].comm;

  // AllGather1 - end

  // Topo detection / System graph creation
  NCCLCHECK(ncclTopoGetSystem(comm, &comm->topo));
  // save nRanks to ncclTopoSystem as indicator of multi-node
  comm->topo->nRanks = comm->nRanks;
  // Compute paths between GPUs and NICs
  NCCLCHECK(ncclTopoComputePaths(comm->topo, comm->peerInfo));
  // Remove inaccessible GPUs and unused NICs
  NCCLCHECK(ncclTopoTrimSystem(comm->topo, comm));
  // Recompute paths after trimming
  NCCLCHECK(ncclTopoComputePaths(comm->topo, comm->peerInfo));
  // Init search
  NCCLCHECK(ncclTopoSearchInit(comm->topo));
  // Print final topology
  NCCLCHECK(ncclTopoPrint(comm->topo));

  { // [RCCL] Check if clique-based kernels can be enabled and initialize CliqueManager
    CliqueManager::cliqueMode_t cliqueMode = CliqueManager::CLIQUE_DISABLED;
    if (intraRanks == nranks)
    {
      // Check that all the GPUs have peer access to one another
      bool hasPeerAccess = true;
      for (int i = 0; i < nranks && hasPeerAccess; i++)
      {
        int cudaDev1 = allGather1Data[i].peerInfo.cudaDev;
        for (int j = 0; j < nranks; j++)
        {
          if (i == j) continue;
          int cudaDev2 = allGather1Data[j].peerInfo.cudaDev;
          int p2p;
          if (hipDeviceCanAccessPeer(&p2p, cudaDev1, cudaDev2) != hipSuccess || !p2p)
          {
            hasPeerAccess = false;
            break;
          }
        }
      }
      if (hasPeerAccess)
      {
        if (intraRanks == nranks)
          cliqueMode = CliqueManager::CLIQUE_SINGLE_PROCESS;
        else
          cliqueMode = CliqueManager::CLIQUE_SINGLE_NODE;
      }

      // For now, only enable clique-based kernels on CR8_G topologies, unless explicitly asked
      if (!rcclParamForceEnableClique())
      {
        // Disable clique-kernel support if not on CR8 topology
        if (!(comm->topo->nodes[GPU].count == comm->topo->nRanks && (comm->topo->type & RCCL_TOPO_CR8G)))
        {
          INFO(NCCL_INIT, "Disabling clique-based kernels due to topology (force enable with RCCL_FORCE_ENABLE_CLIQUE)");
          cliqueMode = CliqueManager::CLIQUE_DISABLED;
        }
      }
    }
    comm->cliqueManager = new CliqueManager(rank, nranks, cliqueMode);
    NCCLCHECK(comm->cliqueManager->Init(commId, rootPid));
  } // [/RCCL]

  // Get rings and trees
  struct ncclTopoGraph ringGraph;
  ringGraph.id = 0;
  ringGraph.pattern = NCCL_TOPO_PATTERN_RING;
  ringGraph.crossNic = ncclParamCrossNic();
  ringGraph.collNet = 0;
  ringGraph.minChannels = 1;
  ringGraph.maxChannels = MAXCHANNELS/2;
  NCCLCHECK(ncclTopoCompute(comm->topo, &ringGraph));
  NCCLCHECK(ncclTopoPrintGraph(comm->topo, &ringGraph));

  struct ncclTopoGraph treeGraph;
  treeGraph.id = 1;
  treeGraph.pattern = tmpNnodes <= 2 ? NCCL_TOPO_PATTERN_TREE : NCCL_TOPO_PATTERN_BALANCED_TREE;
  treeGraph.crossNic = ncclParamCrossNic();
  treeGraph.collNet = 0;
  treeGraph.minChannels = comm->topo->nodes[NET].count != 0 ? 1 : ringGraph.nChannels;
  treeGraph.maxChannels = ringGraph.nChannels;
  NCCLCHECK(ncclTopoCompute(comm->topo, &treeGraph));
  NCCLCHECK(ncclTopoPrintGraph(comm->topo, &treeGraph));

  struct ncclTopoGraph collNetGraph;
  collNetGraph.id = 2;
  collNetGraph.pattern = NCCL_TOPO_PATTERN_TREE;
  collNetGraph.collNet = 1;
  collNetGraph.crossNic = ncclParamCrossNic();
  collNetGraph.minChannels = collNetGraph.maxChannels = ringGraph.nChannels;
  NCCLCHECK(ncclTopoCompute(comm->topo, &collNetGraph));
  NCCLCHECK(ncclTopoPrintGraph(comm->topo, &collNetGraph));

  if (comm->rank == ncclParamGraphDumpFileRank()) {
    struct ncclTopoGraph* graphs[3] = { &ringGraph, &treeGraph, &collNetGraph };
    NCCLCHECK(ncclTopoDumpGraphs(comm->topo, 3, graphs));
  }

  if ((comm->topo->type & RCCL_TOPO_4P2H_ROME) && (comm->topo->type & RCCL_TOPO_GDR_ALL)) {
    if (rcclParamP2pNetDisable() == 0) {
      STORE(comm->p2pNet, 1);
      INFO(NCCL_INIT, "RCCL enabled same node P2P over network");
    }
    else
      INFO(NCCL_INIT, "RCCL force disabled same node P2P over network");
  }
  // AllGather3 - begin
  struct ncclGraphInfo {
    int pattern;
    int sameChannels;
    float speedIntra;
    float speedInter;
    int typeIntra;
    int typeInter;
  };

  struct {
    int cudaCompCap;
    int fullCudaCompCap;
    int nChannels;
    int gcn;
    struct ncclGraphInfo tree;
    struct ncclGraphInfo ring;
    struct ncclGraphInfo collNet;
    struct ncclTopoRanks topoRanks;
  } *allGather3Data;

  NCCLCHECK(ncclCalloc(&allGather3Data, nranks));
  int idx;
  NCCLCHECK(ncclTopoIdToIndex(comm->topo, GPU, myInfo->busId, &idx));
  allGather3Data[rank].cudaCompCap = comm->topo->nodes[GPU].nodes[idx].gpu.cudaCompCap;
  allGather3Data[rank].gcn = comm->topo->nodes[GPU].nodes[idx].gpu.gcn;

  allGather3Data[rank].nChannels = comm->nChannels = treeGraph.nChannels = ringGraph.nChannels =
    std::min(treeGraph.nChannels, ringGraph.nChannels);
  allGather3Data[rank].tree.pattern = treeGraph.pattern;
  allGather3Data[rank].tree.sameChannels = treeGraph.sameChannels;
  allGather3Data[rank].tree.speedIntra = treeGraph.speedIntra;
  allGather3Data[rank].tree.speedInter = treeGraph.speedInter;
  allGather3Data[rank].tree.typeIntra = treeGraph.typeIntra;
  allGather3Data[rank].tree.typeInter = treeGraph.typeInter;
  allGather3Data[rank].ring.pattern = ringGraph.pattern;
  allGather3Data[rank].ring.sameChannels = ringGraph.sameChannels;
  allGather3Data[rank].ring.speedIntra = ringGraph.speedIntra;
  allGather3Data[rank].ring.speedInter = ringGraph.speedInter;
  allGather3Data[rank].ring.typeIntra = ringGraph.typeIntra;
  allGather3Data[rank].ring.typeInter = ringGraph.typeInter;
  allGather3Data[rank].collNet.pattern = collNetGraph.pattern;
  allGather3Data[rank].collNet.sameChannels = collNetGraph.sameChannels;
  allGather3Data[rank].collNet.speedIntra = collNetGraph.speedIntra;
  allGather3Data[rank].collNet.speedInter = collNetGraph.speedInter;
  allGather3Data[rank].collNet.typeIntra = collNetGraph.typeIntra;
  allGather3Data[rank].collNet.typeInter = collNetGraph.typeInter;

  NCCLCHECK(ncclTopoPreset(comm, &treeGraph, &ringGraph, &collNetGraph, &allGather3Data[rank].topoRanks));

  NCCLCHECK(bootstrapAllGather(comm->bootstrap, allGather3Data, sizeof(*allGather3Data)));

  // Determine nNodes, firstRanks, ...
  int *nodesFirstRank, *nodesTreePatterns;
  NCCLCHECK(ncclCalloc(&nodesFirstRank, nranks));
  NCCLCHECK(ncclCalloc(&nodesTreePatterns, nranks));
  for (int i=0; i<nranks; i++) {
    int node = -1;
    int firstRank = allGather3Data[i].topoRanks.ringRecv[0];
    for (int n=0; n<comm->nNodes; n++) {
      if (nodesFirstRank[n] == firstRank) node = n;
    }
    if (node == -1) {
      node = comm->nNodes++;
      nodesFirstRank[node] = firstRank;
      // Record tree pattern of each node as they can be different depending on sm arch
      nodesTreePatterns[node] = allGather3Data[i].tree.pattern;
    }
    if (i == comm->rank) comm->node = node;
  }

  int nChannelsOrig = comm->nChannels;
  struct ncclTopoRanks** allTopoRanks;
  NCCLCHECK(ncclCalloc(&allTopoRanks, comm->nRanks));
  int gcn = allGather3Data[0].gcn;
  for (int i=0; i<nranks; i++) {
    allTopoRanks[i] = &allGather3Data[i].topoRanks;
    gcn = std::min(allGather3Data[i].gcn, gcn);
    // Make sure we align all ranks so that the tuning is consistent across ranks
    treeGraph.nChannels = ringGraph.nChannels = comm->nChannels = std::min(allGather3Data[i].nChannels, comm->nChannels);
    treeGraph.sameChannels = std::min(allGather3Data[i].tree.sameChannels, treeGraph.sameChannels);
    treeGraph.speedIntra = std::min(allGather3Data[i].tree.speedIntra, treeGraph.speedIntra);
    treeGraph.speedInter = std::min(allGather3Data[i].tree.speedInter, treeGraph.speedInter);
    treeGraph.typeIntra = std::min(allGather3Data[i].tree.typeIntra, treeGraph.typeIntra);
    treeGraph.typeInter = std::min(allGather3Data[i].tree.typeInter, treeGraph.typeInter);
    ringGraph.sameChannels = std::min(allGather3Data[i].ring.sameChannels, ringGraph.sameChannels);
    ringGraph.speedIntra = std::min(allGather3Data[i].ring.speedIntra, ringGraph.speedIntra);
    ringGraph.speedInter = std::min(allGather3Data[i].ring.speedInter, ringGraph.speedInter);
    ringGraph.typeIntra = std::min(allGather3Data[i].ring.typeIntra, ringGraph.typeIntra);
    ringGraph.typeInter = std::min(allGather3Data[i].ring.typeInter, ringGraph.typeInter);
    collNetGraph.sameChannels = std::min(allGather3Data[i].collNet.sameChannels, collNetGraph.sameChannels);
    collNetGraph.speedIntra = std::min(allGather3Data[i].collNet.speedIntra, collNetGraph.speedIntra);
    collNetGraph.speedInter = std::min(allGather3Data[i].collNet.speedInter, collNetGraph.speedInter);
    collNetGraph.typeIntra = std::min(allGather3Data[i].collNet.typeIntra, collNetGraph.typeIntra);
    collNetGraph.typeInter = std::min(allGather3Data[i].collNet.typeInter, collNetGraph.typeInter);
  }

  // count NETs used by ring
  int nNets = 0;
  int nets[MAXCHANNELS*2];
  // do not count NETs in case of single node, i.e comm->topo->nodes[GPU].count == comm->topo->nRanks
  for (int i = 0; comm->topo->nodes[GPU].count != comm->topo->nRanks && i < ringGraph.nChannels; i++) {
    for (int j = 0; j < 2; j++) {
      int k;
      for (k = 0; k < nNets; k++)
        if (nets[k] == ringGraph.inter[i*2+j]) break;
      if (k >= nNets) {
        nets[nNets] = ringGraph.inter[i*2+j];
        nNets++;
      }
    }
  }

  if (comm->nChannels < nChannelsOrig) {
    // We started duplicating channels during Preset(), so we need to move the
    // duplicated channels since we have removed some.
    for (int i=0; i<comm->nChannels; i++) memcpy(comm->channels+comm->nChannels+i, comm->channels+nChannelsOrig+i, sizeof(struct ncclChannel));
  }

  int *rings;
  NCCLCHECK(ncclCalloc(&rings, nranks*MAXCHANNELS));

  NCCLCHECK(ncclTopoPostset(comm, nodesFirstRank, nodesTreePatterns, allTopoRanks, rings, gcn, nNets));
  if (comm->nNodes > 1 &&
      ncclParamCollNetEnable() == 1 &&
      collNetSupport() && collNetGraph.nChannels) {
    NCCLCHECK(ncclTopoConnectCollNet(comm, &collNetGraph, rank));
  }

  free(allTopoRanks);
  free(nodesTreePatterns);
  free(nodesFirstRank);
  free(allGather1Data);
  free(allGather3Data);

  // AllGather3 - end

  TRACE(NCCL_INIT, "rank %d nranks %d - BUILT %d TREES/RINGS", rank, nranks, comm->nChannels);

  char line[1024];
  line[0]='\0';
  for (int c=0; c<comm->nChannels; c++) {
    struct ncclTree* tree = &comm->channels[c].tree;
    snprintf(line+strlen(line), 1023-strlen(line), " [%d] %d/%d/%d->%d->%d",
        c, tree->down[0], tree->down[1], tree->down[2], rank, tree->up);
    INFO(NCCL_GRAPH, "Ring %d : %d -> %d -> %d", c, comm->channels[c].ring.prev, comm->rank, comm->channels[c].ring.next);
  }
  line[1023] = '\0';
  INFO(NCCL_INIT, "Trees%s", line);

  // Set Affinity to a CPU local the our GPU, so that all memory we allocate
  // on the host is local.
  cpu_set_t affinitySave;
  sched_getaffinity(0, sizeof(cpu_set_t), &affinitySave);
  NCCLCHECK(ncclTopoSetAffinity(comm->topo, comm->rank));
  ncclResult_t ret;

  NCCLCHECK(computeBuffSizes(comm));

  // Connect with prev/next for each ring
  for (int c=0; c<comm->nChannels; c++) {
    struct ncclChannel* channel = comm->channels+c;
    NCCLCHECKGOTO(setupChannel(comm, c, rank, nranks, rings+c*nranks), ret, affinity_restore);
    if (comm->nRanks == 1) continue;
    NCCLCHECKGOTO(ncclTransportP2pConnect(comm, channel, 1, &channel->ring.prev, 1, &channel->ring.next), ret, affinity_restore);
  }
  NCCLCHECKGOTO(ncclTransportP2pSetup(comm, &ringGraph), ret, affinity_restore);
  INFO(NCCL_INIT, "Connected all rings");

  // Connect Trees
  for (int c=0; c<comm->nChannels; c++) {
    struct ncclChannel* channel = comm->channels+c;
    if (comm->nRanks == 1) continue;
    NCCLCHECKGOTO(ncclTransportP2pConnect(comm, channel, NCCL_MAX_TREE_ARITY, channel->tree.down, 1, &channel->tree.up), ret, affinity_restore);
    NCCLCHECKGOTO(ncclTransportP2pConnect(comm, channel, 1, &channel->tree.up, NCCL_MAX_TREE_ARITY, channel->tree.down), ret, affinity_restore);
  }
  NCCLCHECKGOTO(ncclTransportP2pSetup(comm, &treeGraph), ret, affinity_restore);
  INFO(NCCL_INIT, "Connected all trees");

  // Check if we can setup CollNet
  if (comm->nNodes > 1 &&
      ncclParamCollNetEnable() == 1 &&
      collNetSupport() && collNetGraph.nChannels) {
    int logicChannels = comm->nChannels/2;
    int collNetSetupFail = 0;
    const int recvIndex = 0;  // recv GPU index is always 0
    const int sendIndex = collNetGraph.pattern == NCCL_TOPO_PATTERN_TREE ? 0 : 1;  // send GPU index depends on topo pattern
    for (int c=0; c<logicChannels; c++) {
      struct ncclChannel* channelRecv = comm->channels+logicChannels+c;
      struct ncclChannel* channelSend = comm->channels+c;
      NCCLCHECK(ncclTransportP2pConnect(comm, channelRecv, 1, &channelRecv->collTree.up, 1, channelRecv->collTree.down));
      NCCLCHECK(ncclTransportP2pConnect(comm, channelSend, 1, channelSend->collTree.down, 1, &channelSend->collTree.up));
      const int recvMaster = collNetGraph.intra[c*comm->localRanks+recvIndex];
      const int sendMaster = collNetGraph.intra[c*comm->localRanks+sendIndex];
      if (collNetSetup(comm, &collNetGraph, channelRecv, rank, nranks, recvMaster, sendMaster, comm->nNodes, 1) != 1)
        collNetSetupFail = 1;
      else if (collNetSetup(comm, &collNetGraph, channelSend, rank, nranks, sendMaster, recvMaster, comm->nNodes, 0) != 1)
        collNetSetupFail = 1;
    }
    NCCLCHECK(ncclTransportP2pSetup(comm, &collNetGraph));
    // Verify CollNet setup across ranks
    NCCLCHECK(checkCollNetSetup(comm, rank, collNetSetupFail));
  }
  TRACE(NCCL_INIT, "rank %d nranks %d - CONNECTED %d RINGS AND TREES", rank, nranks, comm->nChannels);
  free(rings);

  // Compute time models for algorithm and protocol combinations
  NCCLCHECK(ncclTopoTuneModel(comm, minCompCap, maxCompCap, &treeGraph, &ringGraph, &collNetGraph));

  // Compute nChannels per peer for p2p
  NCCLCHECK(ncclTopoComputeP2pChannels(comm));

  NCCLCHECK(ncclCommSetIntra(comm, intraRank, intraRanks, intraRank0Comm));

  if (comm->nNodes) NCCLCHECK(ncclProxyCreate(comm));

  // We should have allocated all buffers, collective fifos, ... we can
  // restore the affinity.
affinity_restore:
  sched_setaffinity(0, sizeof(cpu_set_t), &affinitySave);
  if (ret != ncclSuccess) return ret;

  TRACE(NCCL_INIT, "rank %d nranks %d - DONE", rank, nranks);
  return ncclSuccess;
}

ncclResult_t ncclCommInitRankSync(ncclComm_t* newcomm, int nranks, ncclUniqueId commId, int myrank, int cudaDev) {
  ncclResult_t res;

  CUDACHECK(hipSetDevice(cudaDev));
  NCCLCHECKGOTO(commAlloc(newcomm, nranks, myrank), res, cleanup);
  NCCLCHECKGOTO(initTransportsRank(*newcomm, &commId), res, cleanup);
  NCCLCHECKGOTO(devCommSetup(*newcomm), res, cleanup);

  INFO(NCCL_INIT,"comm %p rank %d nranks %d cudaDev %d busId %lx used %ld bytes - Init COMPLETE", *newcomm, myrank, nranks, (*newcomm)->cudaDev, (*newcomm)->busId, allocTracker[(*newcomm)->cudaDev].totalAllocSize);

  return ncclSuccess;
cleanup:
  if ((*newcomm) && (*newcomm)->bootstrap) bootstrapAbort((*newcomm)->bootstrap);
  *newcomm = NULL;
  return res;
}

static ncclResult_t ncclCommInitRankDev(ncclComm_t* newcomm, int nranks, ncclUniqueId commId, int myrank, int cudaDev) {
  ncclResult_t res;
  char* env = getenv("NCCL_COMM_ID");
  if (env && myrank == 0) {
    INFO(NCCL_ENV, "NCCL_COMM_ID set by environment to %s", env);
    NCCLCHECKGOTO(bootstrapCreateRoot(&commId, true), res, end);
  }

  NCCLCHECKGOTO(ncclInit(), res, end);
  if (myrank == 0) showVersion();

  memset(allocTracker+cudaDev, 0, sizeof(struct allocationTracker));
  // Make sure the CUDA runtime is initialized.
  CUDACHECKGOTO(hipFree(NULL), res, end);

  NCCLCHECKGOTO(PtrCheck(newcomm, "CommInitRank", "newcomm"), res, end);
  if (nranks < 1 || myrank < 0 || myrank >= nranks) {
    WARN("Invalid rank requested : %d/%d", myrank, nranks);
    res = ncclInvalidArgument;
    goto end;
  }

  if (ncclAsyncMode()) {
    NCCLCHECKGOTO(ncclAsyncInit(ncclCommInitRankSync, newcomm, nranks, commId, myrank, cudaDev), res, end);
  } else {
    NCCLCHECKGOTO(ncclCommInitRankSync(newcomm, nranks, commId, myrank, cudaDev), res, end);
  }
end:
  if (ncclAsyncMode()) return ncclAsyncErrCheck(res);
  else return res;
}

NCCL_API(ncclResult_t, ncclCommInitRank, ncclComm_t* newcomm, int nranks, ncclUniqueId commId, int myrank);
ncclResult_t ncclCommInitRank(ncclComm_t* newcomm, int nranks, ncclUniqueId commId, int myrank) {
  NVTX3_FUNC_RANGE_IN(nccl_domain);
  int cudaDev;
  CUDACHECK(hipGetDevice(&cudaDev));
  NCCLCHECK(ncclCommInitRankDev(newcomm, nranks, commId, myrank, cudaDev));
  return ncclSuccess;
}

NCCL_API(ncclResult_t, ncclCommInitAll, ncclComm_t* comms, int ndev, const int* devlist);
ncclResult_t ncclCommInitAll(ncclComm_t* comms, int ndev, const int* devlist) {
  NVTX3_FUNC_RANGE_IN(nccl_domain);
  NCCLCHECK(PtrCheck(comms, "CommInitAll", "comms"));
  if (ndev < 0) {
    WARN("Invalid device count requested : %d", ndev);
    return ncclInvalidArgument;
  }

  ncclUniqueId uniqueId;
  NCCLCHECK(ncclGetUniqueId(&uniqueId));
  NCCLCHECK(ncclGroupStart());
  for (int i=0; i<ndev; i++) {
    // Ignore return codes .. we need to call ncclGroupEnd to clean up anyway
    ncclCommInitRankDev(comms+i, ndev, uniqueId, i, devlist ? devlist[i] : i);
  }
  NCCLCHECK(ncclGroupEnd());
  return ncclSuccess;
}

static ncclResult_t commDestroy(ncclComm_t comm) {
  int savedDevice;
#ifdef ENABLE_TRACE
  int rank = comm->rank;
#endif
  CUDACHECK(hipGetDevice(&savedDevice));
  int commDevice = comm->cudaDev;

  if (savedDevice != commDevice) {
    CUDACHECK(hipSetDevice(commDevice));
  }

  TRACE(NCCL_INIT, "Destroying comm %p rank %d abortFlag %d fatalError %d", comm, comm->rank, LOAD(comm->abortFlag), comm->fatalError);

  CUDACHECK(hipStreamSynchronize(comm->groupStream));
  NCCLCHECK(ncclProxyDestroy(comm));
  NCCLCHECK(commFree(comm));

  if (savedDevice != commDevice)
    CUDACHECK(hipSetDevice(savedDevice));

  TRACE(NCCL_INIT, "Destroyed comm %p rank %d", comm, rank);

  return ncclSuccess;
}

NCCL_API(ncclResult_t, ncclCommDestroy, ncclComm_t comm);
ncclResult_t ncclCommDestroy(ncclComm_t comm) {
  NVTX3_FUNC_RANGE_IN(nccl_domain);
  if (comm == NULL)
    return ncclSuccess;

  TRACE(NCCL_INIT, "comm %p rank %d nRanks %d cudaDev %d busId %lx", comm, comm->rank, comm->nRanks, comm->cudaDev, comm->busId);

  // Try and prevent a double free of the comm struct (user error)
  if (comm->rank == -1 || comm->nRanks <= 0 || comm->cudaDev == -1 || comm->busId == -1) {
    WARN("comm %p has already been destroyed", comm);
    return ncclInvalidArgument;
  }

  // [RCCL] Delete CliqueManager if it exists
  if (comm->cliqueManager) delete comm->cliqueManager;
  // [/RCCL]

  return commDestroy(comm);
}

NCCL_API(ncclResult_t, ncclCommAbort, ncclComm_t comm);
ncclResult_t ncclCommAbort(ncclComm_t comm) {
  NVTX3_FUNC_RANGE_IN(nccl_domain);
  if (comm == NULL)
    return ncclSuccess;

  // Ask anything that might still be running on the device to quit
  STORE(comm->abortFlag, 1);

  // do not destroy comm because kernel maybe still running
  // return commDestroy(comm);
  return ncclSuccess;
}

NCCL_API(const char*, ncclGetErrorString, ncclResult_t code);
const char* ncclGetErrorString(ncclResult_t code) {
  switch (code) {
    case ncclSuccess                : return "no error";
    case ncclUnhandledCudaError     : return "unhandled cuda error";
    case ncclSystemError            : return "unhandled system error";
    case ncclInternalError          : return "internal error";
    case ncclInvalidArgument        : return "invalid argument";
    case ncclInvalidUsage           : return "invalid usage";
    default                         : return "unknown result code";
  }
}

NCCL_API(ncclResult_t, ncclCommGetAsyncError, ncclComm_t comm, ncclResult_t *asyncError);
ncclResult_t ncclCommGetAsyncError(ncclComm_t comm, ncclResult_t *asyncError) {
  NCCLCHECK(PtrCheck(comm, "ncclGetAsyncError", "comm"));
  NCCLCHECK(PtrCheck(asyncError, "ncclGetAsyncError", "asyncError"));
  *asyncError = comm->fatalError;
  return ncclSuccess;
}

NCCL_API(ncclResult_t, ncclCommCount, const ncclComm_t comm, int* count);
ncclResult_t ncclCommCount(const ncclComm_t comm, int* count) {
  NVTX3_FUNC_RANGE_IN(nccl_domain);
  NCCLCHECK(PtrCheck(comm, "CommCount", "comm"));
  NCCLCHECK(PtrCheck(count, "CommCount", "count"));
  *count = comm->nRanks;
  return ncclSuccess;
}

NCCL_API(ncclResult_t, ncclCommCuDevice, const ncclComm_t comm, int* devid);
ncclResult_t ncclCommCuDevice(const ncclComm_t comm, int* devid) {
  NVTX3_FUNC_RANGE_IN(nccl_domain);
  NCCLCHECK(PtrCheck(comm, "CommCuDevice", "comm"));
  NCCLCHECK(PtrCheck(devid, "CommCuDevice", "devid"));
  *devid = comm->cudaDev;
  return ncclSuccess;
}

NCCL_API(ncclResult_t, ncclCommUserRank, const ncclComm_t comm, int* rank);
ncclResult_t ncclCommUserRank(const ncclComm_t comm, int* rank) {
  NVTX3_FUNC_RANGE_IN(nccl_domain);
  NCCLCHECK(PtrCheck(comm, "CommUserRank", "comm"));
  NCCLCHECK(PtrCheck(rank, "CommUserRank", "rank"));
  *rank = comm->rank;
  return ncclSuccess;
}
