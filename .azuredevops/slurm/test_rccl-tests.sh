#!/bin/bash
#SBATCH --job-name=rccl-tests
#SBATCH --output=%x-%j.out
#SBATCH --error=%x-%j.out
#SBATCH --time=$TESTS_TIMEOUT
#SBATCH --nodes=1
#SBATCH --exclusive
#SBATCH --partition=gt

short_id=$(hostname | cut -d'.' -f1 | cut -d'-' -f3-)
echo "Node identifier: $short_id"

source /etc/profile.d/lmod.sh
module load rocm/6.2.0

if [ "$ENABLE_COVERAGE" = "true" ]; then
    cd $(Pipeline.Workspace)/s/rccl-test-infra || exit
    CODE_COV=1 ./run.sh -c config/"$INFRA_TEST_CONFIG".json -B -C -O --use-slurm --slurm-time="$TESTS_TIMEOUT" --slurm-partition=gt --slurm-nodes="$NUM_NODES" --work_dir="$BINARIES_DIR"
    cd slurm_runs_"$INFRA_TEST_CONFIG_"* || exit

    FILE="rawprofiles.list"

    echo "Waiting for ${FILE}"
    while [ ! -e "${FILE}" ]; do
      sleep 1
    done
    echo "File ${FILE} found, now building coverage report"
    /opt/rocm/lib/llvm/bin/llvm-profdata merge --sparse --input-files=$FILE --output=merged.profdata
    /opt/rocm/lib/llvm/bin/llvm-cov show --instr-profile="merged.profdata" --format=html --output-dir=report --project-title=RCCL_Lib_Coverage_Report --ignore-filename-regex="ext-src\/*" "${BINARIES_DIR}"/rccl/build/release/librccl.so
    exit 0
fi

cd "$BINARIES_DIR/bin"

export PATH="$BINARIES_DIR/bin:$PATH"
export LD_LIBRARY_PATH="$BINARIES_DIR/lib:$LD_LIBRARY_PATH"

for coll in all_reduce all_gather reduce_scatter alltoall alltoallv broadcast gather reduce scatter sendrecv
do
	cmd="${MPI_HOME}/bin/mpirun -np 8 -mca oob_tcp_if_exclude docker,lo -mca btl_tcp_if_exclude docker,lo -mca pml ob1 -mca btl ^openib -x PATH -x LD_LIBRARY_PATH -x NCCL_DEBUG=VERSION -x NCCL_IGNORE_CPU_AFFINITY=1 -x HSA_NO_SCRATCH_RECLAIM=1 ${BINARIES_DIR}/bin/${coll}_perf -b 1K -e 1G -f 2 -g 1 -d float -n 100 -w 50 -Z json -x rccl-tests_${coll}_nodes1_gpus8_float.json"

	echo "Running ${coll}"
	echo "Run cmd: ${cmd}"
	eval ${cmd}

	sleep 2
done

## To add
### Summarize results
### Convert to junit
