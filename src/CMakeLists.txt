# Copyright 2014 Carnegie Mellon University
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required (VERSION 3.16)

# basic configuration
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_CURRENT_BINARY_DIR)
	message(FATAL_ERROR "Use out-of-source build only!")
endif(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_CURRENT_BINARY_DIR)

project(MEHCACHED)

add_subdirectory(perf_count)
set(LIBRARY_PATH ${LIBRARY_PATH} perf_count/)

# compiler options
add_definitions(-pthread)
add_definitions(-g -Wall -Wextra -Wsign-conversion -Winline -Wno-unused-function)
add_definitions(-Wconversion)
add_definitions(-O9)

#add_definitions(-msse4.2 -march=corei7)
add_definitions(-std=c++17 -O3 -march=native)

# for buildroot 
# find_package(gflags REQUIRED)
# include_directories(${CMAKE_CURRENT_SOURCE_DIR})
# include_directories(${CMAKE_CURRENT_SOURCE_DIR}/gem5-include)
# link_directories(${CMAKE_CURRENT_SOURCE_DIR}/gem5-lib)
# link_libraries(m5 gflags)
# link_libraries(m5)
add_definitions("-D_GEM5_")

# preprocessor
SET(NDEBUG "$ENV{NDEBUG}" CACHE STRING "define NDEBUG macro")

message(STATUS "NDEBUG (disable all additional checkings; no*, yes) = ${NDEBUG}")
if("${NDEBUG}" STREQUAL "yes")
	add_definitions(-DNDEBUG)
endif()

SET(NLOCAL "$ENV{NLOCAL}" CACHE STRING "no local programs")
SET(NCLIENT "$ENV{NCLIENT}" CACHE STRING "no clients")
SET(NSERVER "$ENV{NSERVER}" CACHE STRING "no servers")

message(STATUS "NLOCAL (do not compile local programs; no*, yes) = ${NLOCAL}")
message(STATUS "NCLIENT (do not compile clients; no*, yes) = ${NCLIENT}")
message(STATUS "NSERVER (do not compile server; no*, yes) = ${NSERVER}")

# common source files
set(SOURCES ${SOURCES} hash.c)
set(SOURCES ${SOURCES} city.c)
set(SOURCES ${SOURCES} stopwatch.c)
set(SOURCES ${SOURCES} shm.c)

# common libraries
set(LIBRARIES ${LIBRARIES} rt crypto perf_count m pthread)

# tcmalloc (optional)
#add_definitions(-fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free)
#set(LIBRARIES ${LIBRARIES} tcmalloc_minimal)

# DPDK
# set(RTE_SDK /home/ubuntu/app_server/gem5-dpdk-setup/dpdk-pktgen-build/dpdk)
# set(RTE_TARGET x86_64-default-linuxapp-gcc)
set(RTE_TARGET arm64-armv8-linuxapp-gcc)
add_definitions(-DUSE_DPDK)
add_definitions(-isystem ${RTE_SDK}/build/include -include rte_config.h)
# link_directories(${LINK_DIRECTORIES} ${RTE_SDK}/build/lib)
# set(LIBRARIES ${LIBRARIES} -Wl,--whole-archive)
# set(LIBRARIES ${LIBRARIES} rte_kvargs rte_mbuf rte_ethdev rte_mempool rte_ring rte_eal)
# set(LIBRARIES ${LIBRARIES} -Wl,--no-whole-archive)
# set(LIBRARIES ${LIBRARIES} rt dl)

include_directories($ENV{RTE_SDK}/config)
include_directories($ENV{RTE_SDK}/build)
include_directories($ENV{RTE_SDK}/lib/librte_eal/include)
include_directories($ENV{RTE_SDK}/lib/librte_eal/linux/include)
include_directories($ENV{RTE_SDK}/lib/librte_ethdev)
include_directories($ENV{RTE_SDK}/lib/librte_net)
include_directories($ENV{RTE_SDK}/lib/librte_eal/arm/include)
include_directories($ENV{RTE_SDK}/lib/librte_mbuf)
include_directories($ENV{RTE_SDK}/lib/librte_mempool)
include_directories($ENV{RTE_SDK}/lib/librte_ring)
include_directories($ENV{RTE_SDK}/lib/librte_pdump)
include_directories($ENV{RTE_SDK}/lib/librte_kvargs)
#include_directories($ENV{GIT_ROOT}/gem5/include)
link_libraries(
        -L$ENV{RTE_SDK}/build/drivers
        -L$ENV{RTE_SDK}/build/lib
        -fno-inline-atomics
        -Wl,--whole-archive
        -l:librte_flow_classify.a
        -l:librte_pipeline.a
        -l:librte_pdump.a
        -l:librte_distributor.a
        -l:librte_ip_frag.a
        -l:librte_meter.a
        -l:librte_lpm.a
        -l:librte_acl.a
        -l:librte_jobstats.a
        -l:librte_metrics.a
        -l:librte_bitratestats.a
        -l:librte_latencystats.a
        -l:librte_efd.a
        -l:librte_bpf.a
        -l:librte_cfgfile.a
        -l:librte_gro.a
        -l:librte_gso.a
        -l:librte_hash.a
        -l:librte_member.a
        -l:librte_vhost.a
        -l:librte_kvargs.a
        -l:librte_mbuf.a
        -l:librte_net.a
        -l:librte_ethdev.a
        -l:librte_bbdev.a
        -l:librte_cryptodev.a
        -l:librte_security.a
        -l:librte_compressdev.a
        -l:librte_eventdev.a
        -l:librte_rawdev.a
        -l:librte_timer.a
        -l:librte_mempool_ring.a
        -l:librte_mempool.a
        -l:librte_ring.a
        -l:librte_pci.a
        -l:librte_eal.a
        -l:librte_cmdline.a
        -l:librte_reorder.a
        -l:librte_sched.a
        -l:librte_common_cpt.a
        -l:librte_common_octeontx.a
        -l:librte_common_dpaax.a
        -l:librte_bus_pci.a
        -l:librte_bus_vdev.a
        -l:librte_net_e1000.a
        -l:librte_net_tap.a
        -l:librte_net_virtio.a
        -l:librte_net_igc.a
        -l:librte_common_iavf.a
        -l:librte_kni.a
        -l:librte_telemetry.a
        -l:librte_rcu.a
        -Wl,--no-whole-archive
        -l:librte_table.a
        -l:librte_port.a
        -ldl
        -lm
        -latomic
        -lpthread)



#if(NOT "${NLOCAL}" STREQUAL "yes")
#	# test
#	add_executable(test test.c ${SOURCES})
#	target_link_libraries(test ${LIBRARIES})

	# load
#	add_executable(load load.c ${SOURCES})
#	target_link_libraries(load ${LIBRARIES})

	# microbench
#	add_executable(microbench microbench.c ${SOURCES})
#	target_link_libraries(microbench ${LIBRARIES})

	# microbench_store
#	add_executable(microbench_store microbench.c ${SOURCES})
#	set_target_properties(microbench_store PROPERTIES COMPILE_FLAGS "-DMEHCACHED_NO_EVICTION")
#	target_link_libraries(microbench_store ${LIBRARIES})

	# netbench_analysis
#	add_executable(netbench_analysis netbench_config.c netbench_analysis.c ${SOURCES})
#	target_link_libraries(netbench_analysis ${LIBRARIES})
#endif()


if(NOT "${NSERVER}" STREQUAL "yes")
	# netbench_server
	add_executable(netbench_server net_common.c netbench_config.c netbench_server.c ${SOURCES})
	target_link_libraries(netbench_server ${LIBRARIES})
	

	# netbench_server_latency
	# add_executable(netbench_server_latency net_common.c netbench_config.c netbench_server.c ${SOURCES})
	# set_target_properties(netbench_server_latency PROPERTIES COMPILE_FLAGS "-DMEHCACHED_MEASURE_LATENCY")
	# target_link_libraries(netbench_server_latency ${LIBRARIES})

	# netbench_server_soft_fdir
	# add_executable(netbench_server_soft_fdir net_common.c netbench_config.c netbench_server.c ${SOURCES})
	# set_target_properties(netbench_server_soft_fdir PROPERTIES COMPILE_FLAGS "-DMEHCACHED_USE_SOFT_FDIR -O0 -g")
	# target_link_libraries(netbench_server_soft_fdir ${LIBRARIES})

	# netbench_server_store
	# add_executable(netbench_server_store net_common.c netbench_config.c netbench_server.c ${SOURCES})
	# set_target_properties(netbench_server_store PROPERTIES COMPILE_FLAGS "-DMEHCACHED_NO_EVICTION")
	# target_link_libraries(netbench_server_store ${LIBRARIES})
endif()
