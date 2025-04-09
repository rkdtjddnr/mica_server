// Copyright 2014 Carnegie Mellon University
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "common.h"
#include "net_common.h"

// #define NUM_PORT (4)
// #define MEHCACHED_MAX_THREADS (1)
// #define MEHCACHED_MAX_PARTITIONS (2)
#define MEHCACHED_MAX_WORKLOAD_THREADS (1)
#define MEHCACHED_MAX_HOT_ITEMS (64)

// SAME AS CLIENT CONFIG 
#define PER_TH_PORT 1 // number of port for each thread
#define NUM_PORT 1 // number of port
#define NUM_QUEUE 2 // number of queue


#define NUM_THREAD 2 // number of thread
#define NUM_PART 2 // number of partition
#define MAC_SZ 6 // mac addr array size
#define IP_SZ 4 // ip addr array size

#define NUM_CORE 2
#define KEY_LEN 8 
#define VAL_LEN 8
#define NUM_ITEMS 1048576
#define NUM_HOT_ITEMS 64

#define NUM_PREPOP NUM_ITEMS // prepopulation dataset size
// For workload configuration 
#define PARTITION_MODE -1 // partition mode -1, 0 -1 : NO NUMA, 정수는 특정 NUMA에 binding
#define ZIPF 0.0
#define GET_RATIO 0.95
#define PUT_RATIO (1 - GET_RATIO)
#define INC_RATIO 0.0
#define BATCH_SZ 2
#define NUM_OP 16384 //operation size
#define DURATION 0.0


// common
#pragma pack(push,1)
struct mehcached_port_conf
{
	uint8_t mac_addr[6];
	uint8_t ip_addr[4];
};
#pragma pack(pop)

// server
#pragma pack(push,1)
struct mehcached_server_thread_conf
{
	uint8_t num_ports;
	uint8_t port_ids[NUM_PORT];
};
#pragma pack(pop)


struct mehcached_server_partition_conf
{
	uint64_t num_items;
	uint64_t alloc_size;
	uint8_t concurrent_table_read;
	uint8_t concurrent_table_write;
	uint8_t concurrent_alloc_write;
	uint8_t thread_id;
	double mth_threshold;
};


struct mehcached_server_hot_item_conf
{
	uint64_t key_hash;
	uint8_t thread_id;
};

#pragma pack(push, 1)
struct mehcached_server_conf
{
	uint8_t num_ports;
	struct mehcached_port_conf ports[NUM_PORT];
	uint8_t num_threads;
	struct mehcached_server_thread_conf threads[NUM_THREAD];
	uint16_t num_partitions;
	struct mehcached_server_partition_conf partitions[NUM_PART];
	uint8_t num_hot_items;
	struct mehcached_server_hot_item_conf hot_items[MEHCACHED_MAX_HOT_ITEMS];
};
#pragma pack(pop)

#define MEHCACHED_CONCURRENT_TABLE_READ(server_conf, partition_id) ((server_conf)->partitions[partition_id].concurrent_table_read)
#define MEHCACHED_CONCURRENT_TABLE_WRITE(server_conf, partition_id) ((server_conf)->partitions[partition_id].concurrent_table_write)
#define MEHCACHED_CONCURRENT_ALLOC_WRITE(server_conf, partition_id) ((server_conf)->partitions[partition_id].concurrent_alloc_write)


// client
struct mehcached_client_conf
{
	uint8_t num_ports;
	struct mehcached_port_conf ports[NUM_PORT];
	uint8_t num_threads;
};


// prepopulation
struct mehcached_prepopulation_conf
{
	// TODO: support multiple datasets
	uint64_t num_items;
	size_t key_length;
	size_t value_length;
};


// workload
struct mehcached_workload_thread_conf
{
	uint8_t num_ports;
	uint8_t port_ids[NUM_PORT];
	char server_name[64];
	int8_t partition_mode;
	uint64_t num_items;
	size_t key_length;
	size_t value_length;
	double zipf_theta;
	uint8_t batch_size;
	double get_ratio;
	double put_ratio;
	double increment_ratio;
	uint64_t num_operations;
	double duration;
};

struct mehcached_workload_conf
{
	uint8_t num_threads;
	struct mehcached_workload_thread_conf threads[MEHCACHED_MAX_WORKLOAD_THREADS];
};

/*
// functions
struct mehcached_server_conf *
mehcached_get_server_conf(const char *filename, const char *server_name);

struct mehcached_client_conf *
mehcached_get_client_conf(const char *filename, const char *client_name);

struct mehcached_prepopulation_conf *
mehcached_get_prepopulation_conf(const char *filename, const char *server_name);

struct mehcached_workload_conf *
mehcached_get_workload_conf(const char *filename, const char *client_name);
*/

enum ConcurrencyType{
    EREW,
    CREW,
    CRCW,
    CRCWS,
    CREW0
};

void fill_partitions(struct mehcached_server_conf *conf,
                     uint16_t num_partitions,
                     uint64_t total_items,
                     uint64_t total_alloc_size,
                     double mth_threshold,
                     enum ConcurrencyType concurrency);

void make_server_config(struct mehcached_server_conf* s_conf);

void make_client_config(struct mehcached_client_conf* c_conf);

void make_prepopulation_conf(struct mehcached_prepopulation_conf* p_conf);

void make_workload_conf(struct mehcached_workload_conf* w_conf);

