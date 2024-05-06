# MIT License
#
# Copyright (c) 2020 Advanced Micro Devices, Inc. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

# Dependencies

# HIP dependency is handled earlier in the project cmake file
# when VerifyCompiler.cmake is included.

# GIT

# Test dependencies

# For downloading, building, and installing required dependencies
include(cmake/DownloadProject.cmake)

if(ENABLE_MSCCLPP)
    set(MSCCLPP_ROOT ${CMAKE_CURRENT_BINARY_DIR}/mscclpp CACHE PATH "")
    find_package(mscclpp)

    if(NOT mscclpp_FOUND)
        message(STATUS "MSCCLPP not found. Downloading and building MSCCLPP.")
        # Download, build and install mscclpp

        download_project(PROJ                mscclpp
                         GIT_REPOSITORY      https://github.com/nileshnegi/mscclpp.git
                         GIT_TAG             feature/rccl_mscclpp
                         INSTALL_DIR         ${MSCCLPP_ROOT}
                         CMAKE_ARGS          -DGPU_TARGETS=${AMDGPU_TARGETS} -DBYPASS_GPU_CHECK=ON -DUSE_ROCM=ON -DCMAKE_BUILD_TYPE=Release -DBUILD_APPS_NCCL=ON -DBUILD_PYTHON_BINDINGS=OFF -DBUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
                         LOG_DOWNLOAD        TRUE
                         LOG_CONFIGURE       TRUE
                         LOG_BUILD           TRUE
                         LOG_INSTALL         TRUE
                         UPDATE_DISCONNECTED TRUE
        )

        if(EXISTS ${MSCCLPP_ROOT}/lib)
        else()
            message(FATAL_ERROR "Cannot find mscclpp library installation path.")
            find_package(mscclpp REQUIRED CONFIG PATHS ${MSCCLPP_ROOT})
        endif()
    endif()

    set(MSCCLPP_INCLUDE_DIRS ${MSCCLPP_ROOT}/include CACHE PATH "")
    set(MSCCLPP_LIBRARIES ${MSCCLPP_ROOT}/lib/libmscclpp_static.a;${MSCCLPP_ROOT}/lib/libmscclpp_nccl_static.a CACHE PATH "")
endif()
