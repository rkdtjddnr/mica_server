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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include <signal.h>

#include <fcntl.h>

#include "mehcached.h"
#include "hash.h"
#include "net_common.h"
#include "proto.h"
#include "stopwatch.h"
#include "netbench_config.h"
#include "netbench_hot_item_hash.h"
#include "table.h"

#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_debug.h>
//#include <gem5/m5ops.h>



#if !defined(NETBENCH_SERVER_MEMCACHED) && !defined(NETBENCH_SERVER_MASSTREE) && !defined(NETBENCH_SERVER_RAMCLOUD)
#define NETBENCH_SERVER_MEHCACHED
#endif

#ifdef NETBENCH_SERVER_MEMCACHED
#include <memcached_export.h>
#endif
#ifdef NETBENCH_SERVER_MASSTREE
#include <masstree_export.h>
#endif
#ifdef NETBENCH_SERVER_RAMCLOUD
#include <ramcloud_export.h>
#endif


#ifdef MICA_PCAP
// For pcap file
#include "../../pcap_file/src/mica_pcap.h"
#endif

// for logging
#define RXMBUF_ADDR 0
#define REQ_DEBUG 0

#define TIME_DIFF_POLL 0

//#define USE_HOT_ITEMS

struct server_state
{
    struct mehcached_server_conf *server_conf;
    struct mehcached_prepopulation_conf *prepopulation_conf;
#ifdef NETBENCH_SERVER_MEHCACHED
    struct mehcached_table **partitions;
#endif
#ifdef NETBENCH_SERVER_MEMCACHED
    // memcached uses a singleton
#endif
#ifdef NETBENCH_SERVER_MASSTREE
    masstree_t masstree;
#endif
#ifdef NETBENCH_SERVER_RAMCLOUD
    ramcloud_t ramcloud;
#endif

#ifdef USE_HOT_ITEMS
    struct mehcached_hot_item_hash hot_item_hash;
#endif
    uint32_t target_request_rate;   // target request rate (in ops) for each client thread

    int cpu_mode;
    int port_mode;

    // runtime state
    uint64_t num_operations_done;
    uint64_t num_key0_operations_done;
    uint64_t num_operations_succeeded;
    uint64_t num_rx_burst;
    uint64_t num_rx_received;
    uint64_t num_tx_sent;
    uint64_t num_tx_dropped;
    uint64_t bytes_rx;
    uint64_t bytes_tx;
    uint64_t num_per_partition_ops[NUM_PART];
    uint64_t last_num_operations_done;
    uint64_t last_num_key0_operations_done;
    uint64_t last_num_operations_succeeded;
    uint64_t last_num_rx_burst;
    uint64_t last_num_rx_received;
    uint64_t last_num_tx_sent;
    uint64_t last_num_tx_dropped;
    uint64_t last_bytes_rx;
    uint64_t last_bytes_tx;
    uint64_t last_num_per_partition_ops[NUM_PART];
    uint16_t packet_size;
    

#ifdef MEHCACHED_USE_SOFT_FDIR
    // struct rte_ring *soft_fdir_mailbox[NUM_THREAD] __rte_cache_aligned;
    struct rte_ring *soft_fdir_mailbox[MEHCACHED_MAX_NUMA_NODES] __rte_cache_aligned;
    uint64_t num_soft_fdir_dropped[NUM_PORT];
#endif
} __rte_cache_aligned;

#ifdef MEHCACHED_MEASURE_LATENCY
static uint32_t target_request_rate_from_user;
#endif

static volatile bool exiting = false;

static
void
signal_handler(int signum)
{
    if (signum == SIGINT)
        fprintf(stderr, "caught SIGINT\n");
    else if (signum == SIGTERM)
        fprintf(stderr, "caught SIGTERM\n");
    else
        fprintf(stderr, "caught unknown signal\n");
    exiting = true;
}

static
uint16_t
mehcached_get_partition_id(struct server_state *state, uint64_t key_hash)
{
#ifdef USE_HOT_ITEMS
    uint8_t hot_item_id = mehcached_get_hot_item_id(state->server_conf, &state->hot_item_hash, key_hash);
    if (hot_item_id != (uint8_t)-1)
        return (uint16_t)(state->server_conf->num_partitions + state->server_conf->hot_items[hot_item_id].thread_id);
    else
#endif
        return (uint16_t)(key_hash >> 48) & (uint16_t)(state->server_conf->num_partitions - 1);
}

static
void
mehcached_remote_send_response(struct server_state *state, struct rte_mbuf *mbuf, uint8_t port_id)
{
    struct mehcached_batch_packet *packet = rte_pktmbuf_mtod(mbuf, struct mehcached_batch_packet *);

    // update stats
    uint8_t request_index;
    const uint8_t *next_key = packet->data + sizeof(struct mehcached_request) * (size_t)packet->num_requests;
    for (request_index = 0; request_index < packet->num_requests; request_index++)
    {
        struct mehcached_request *req = (struct mehcached_request *)packet->data + request_index;

        state->num_operations_done++;

        if (*(const uint64_t *)next_key == 0)
            state->num_key0_operations_done++;

        if (req->result == MEHCACHED_OK)
            state->num_operations_succeeded++;

        next_key += MEHCACHED_ROUNDUP8(MEHCACHED_KEY_LENGTH(req->kv_length_vec)) + MEHCACHED_ROUNDUP8(MEHCACHED_VALUE_LENGTH(req->kv_length_vec));
    }

    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)rte_pktmbuf_mtod(mbuf, unsigned char *);
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)((unsigned char *)eth + sizeof(struct rte_ether_hdr));
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)((unsigned char *)ip + sizeof(struct rte_ipv4_hdr));

    uint16_t packet_length = (uint16_t)(next_key - (uint8_t *)packet);

#ifdef MEHCACHED_ENABLE_THROTTLING
    // server load feedback
    // abuse the opaque field because it is going to be used for flow control anyway if clients can do full RX
    packet->opaque = state->target_request_rate;
#endif

    // TODO: update IP checksum

    // swap source and destination
    {
        struct rte_ether_addr t = eth->s_addr;
        eth->s_addr = eth->d_addr;
        eth->d_addr = t;
    }
    {
        uint32_t t = ip->src_addr;
        ip->src_addr = ip->dst_addr;
        ip->dst_addr = t;
    }
    {
        uint16_t t = udp->src_port;
        udp->src_port = udp->dst_port;
        udp->dst_port = t;
    }

    // reset TTL
    ip->time_to_live = 64;

    ip->total_length = rte_cpu_to_be_16((uint16_t)(packet_length - sizeof(struct rte_ether_hdr)));
    udp->dgram_len = rte_cpu_to_be_16((uint16_t)(packet_length - sizeof(struct rte_ether_hdr) - sizeof(struct rte_ipv4_hdr)));

    // printf("IP Total Length:      %u bytes (network to host converted)\n", rte_be_to_cpu_16(ip->total_length));

    mbuf->data_len = packet_length;
    mbuf->pkt_len = (uint32_t)packet_length;
    mbuf->next = NULL;
    mbuf->nb_segs = 1;
    mbuf->ol_flags = 0;

#ifndef NDEBUG
    rte_mbuf_sanity_check(mbuf, RTE_MBUF_PKT, 1);
    if (rte_pktmbuf_headroom(mbuf) + mbuf->data_len > mbuf->buf_len)
    {
        printf("data_len = %hd\n", mbuf->data_len);
        uint8_t request_index;
        const uint8_t *next_key = packet->data + sizeof(struct mehcached_request) * (size_t)packet->num_requests;
        for (request_index = 0; request_index < packet->num_requests; request_index++)
        {
            struct mehcached_request *req = (struct mehcached_request *)packet->data + request_index;

            printf("%hhu: %hhu %hhu %u %u\n", request_index, req->operation, req->result, MEHCACHED_KEY_LENGTH(req->kv_length_vec), MEHCACHED_VALUE_LENGTH(req->kv_length_vec));
            next_key += MEHCACHED_ROUNDUP8(MEHCACHED_KEY_LENGTH(req->kv_length_vec)) + MEHCACHED_ROUNDUP8(MEHCACHED_VALUE_LENGTH(req->kv_length_vec));
        }
    }
    assert(rte_pktmbuf_headroom(mbuf) + mbuf->data_len <= mbuf->buf_len);
#endif

    mehcached_send_packet(port_id, mbuf);

    state->bytes_tx += (uint64_t)(packet_length + 24);  // 24 for PHY overheads
}

static
int
mehcached_benchmark_consume_packets_proc(void *arg)
{
    uint8_t num_ports = (uint8_t)(size_t)arg;

    // discard some initial packets that may have been misclassified (there is a small window before setting perfect filters)
    // 4096 is the maximum number of descriptors in a ring
    int i;
    uint8_t port_id;
    for (port_id = 0; port_id < num_ports; port_id++)
    {
        for (i = 0; i < 4096; i++)
        {
            struct rte_mbuf *mbuf = mehcached_receive_packet(port_id);
            if (mbuf != NULL)
                mehcached_packet_free(mbuf);
            else
                break;
        }
    }
    return 0;
}

#ifdef USE_ENSO
struct RXTXState 
{
    //EndpointId eid;
    //enso::TxPipe* tx_pipe;

    struct PendingTX {

    uint8_t* start_tx_buffer;
    uint8_t* current_tx_buffer;

    uint16_t count;
    uint64_t oldest_time;
    } pending_tx;
};
// for maintain current enso pipe state used by host
typedef struct MicaProcessingUnit
{
    uint32_t avail_bytes;
    uint8_t* rx_buf; // Enso Pipe Start addr for packet processing
    uint8_t* end_of_buffer;

    // maintain state
    int32_t missing_messages; // = burstSize; will be decline
    uint32_t remaining_bytes; // = availByte;, will be decline
    uint32_t received; // count for received packet
    uint32_t end; // flag for current buffer is consumed all

    uint8_t* addr;
    uint8_t* next_addr;

}MicaProcessingUnit_t;

void
setProcessingUnit(RxEnsoPipe_t* rx_pipe, MicaProcessingUnit_t* mica_unit, uint32_t new_rx_bytes, uint8_t* new_rx_buf, int32_t burst_size)
{
    mica_unit->rx_buf = new_rx_buf;
    mica_unit->avail_bytes = new_rx_bytes;
    mica_unit->end_of_buffer = (uint8_t*)rx_pipe->buf + ENSO_BUF_SIZE;

    mica_unit->missing_messages = burst_size;
    mica_unit->remaining_bytes = new_rx_bytes;
    mica_unit->received = 0;
    mica_unit->end = burst_size;

    mica_unit->addr = new_rx_buf;
    mica_unit->next_addr = new_rx_buf;//getNextPkt(mica_unit->addr);

}

void dump_hex(const char *label, const uint8_t *data, uint32_t len) {
    printf("    %s (len=%u): ", label, len);
    for (uint32_t i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
    printf("\n");
}

void dumpRxPktInfo(const struct mehcached_batch_packet *packet)
{
    printf("=== Dumping mehcached_batch_packet ===\n");
    printf("Num Requests: %u\n", packet->num_requests);

    const struct mehcached_request *req = (const struct mehcached_request *)packet->data;
    const uint8_t *key_ptr = packet->data + sizeof(struct mehcached_request) * packet->num_requests;

    for (uint8_t i = 0; i < packet->num_requests; i++) {
        const struct mehcached_request* rx_req = req + i;

        uint32_t key_len_raw = MEHCACHED_KEY_LENGTH(rx_req->kv_length_vec);
        uint32_t val_len_raw = MEHCACHED_VALUE_LENGTH(rx_req->kv_length_vec);

        uint32_t key_len_rounded = MEHCACHED_ROUNDUP8(key_len_raw);
        uint32_t val_len_rounded = MEHCACHED_ROUNDUP8(val_len_raw);

        printf("[Request %u]\n", i);
        printf("Key Hash:     0x%lx\n", rx_req->key_hash);
        printf("Expire Time:  0x%x\n", rx_req->expire_time);

        if (rx_req->operation == MEHCACHED_GET) {
            printf("Operation:    GET\n");
            dump_hex("Key", key_ptr, key_len_raw);
        } else if (rx_req->operation == MEHCACHED_SET) {
            printf("Operation:    SET\n");
            dump_hex("Key", key_ptr, key_len_raw);
            dump_hex("Value", key_ptr + key_len_rounded, val_len_raw);
        } else {
            printf("Operation:    UNKNOWN (%u)\n", rx_req->operation);
        }

        key_ptr += key_len_rounded + val_len_rounded;
    }
}

uint16_t be_to_le_16(const uint16_t le) {
  return ((le & (uint16_t)0x00ff) << 8) | ((le & (uint16_t)0xff00) >> 8);
}

uint16_t get_pkt_len(const uint8_t* addr) {
    const struct rte_ether_hdr* l2_hdr = (struct rte_ether_hdr*)addr;
    const struct rte_ipv4_hdr* l3_hdr = (struct rte_ipv4_hdr*)(l2_hdr + 1);
    const uint16_t total_len = be_to_le_16(l3_hdr->total_length) + sizeof(struct rte_ether_hdr);
    printf("[DEBUG] host get_pkt_len func total_len %u \n", total_len);
    
    return total_len;
}

uint8_t* getNextPkt(uint8_t* pkt)
{
    uint32_t pkt_len = get_pkt_len(pkt);
    //uint32_t pkt_len = getMemcPktLen(pkt); // for memcached
    uint32_t nb_flits = (pkt_len - 1) / 64 + 1;
    //printf("[DEBUG] pkt_len: %u, nb_flits: %u\n", pkt_len, nb_flits);

    return pkt + nb_flits * 64;
}

struct mehcached_batch_packet* 
getPacketFromRxPipe(MicaProcessingUnit_t* mica_unit)
{
    if((mica_unit->missing_messages > 0) && (mica_unit->remaining_bytes > 0))
    {
        mica_unit->addr = mica_unit->next_addr;
        if(mica_unit->addr >= mica_unit->end_of_buffer)
        {
            //printf("[WARN] addr reached end_of_buffer, received=%u\n",mica_unit->received);
            mica_unit->end = mica_unit->received;
            return NULL;
        }
        mica_unit->next_addr = getNextPkt(mica_unit->next_addr);
        uint32_t consumed = mica_unit->next_addr - mica_unit->addr;

        mica_unit->remaining_bytes -= consumed;
        --mica_unit->missing_messages;
        ++mica_unit->received;

        return (struct mehcached_batch_packet* )mica_unit->addr;
    }
    else
    {
        //printf("[WARN] complete processing %u pkts\n", mica_unit->received);
        mica_unit->end = mica_unit->received;
        return NULL;
    }
}

// Cpy Ether, IP, UDP, Request Header of RX to TX
void cpyHeaderRxToTx(struct RXTXState* rx_tx_state, struct mehcached_batch_packet* rx_packet)
{
    assert(rx_tx_state);
    assert(rx_packet);
    struct mehcached_batch_packet* tx_res = (struct mehcached_batch_packet *)rx_tx_state->pending_tx.current_tx_buffer;

    tx_res->num_requests = rx_packet->num_requests;

    // Set ethernet, ip, udp header...
    // RX packet
    struct rte_ether_hdr* rx_eth = (struct rte_ether_hdr *)rx_packet;
    struct rte_ipv4_hdr* rx_ip = (struct rte_ipv4_hdr*)(rx_eth + 1);
    struct rte_udp_hdr* rx_udp = (struct rte_udp_hdr*)(rx_ip + 1);
    // TX packet
    struct rte_ether_hdr* tx_eth = (struct rte_ether_hdr *)rx_tx_state->pending_tx.current_tx_buffer;
    struct rte_ipv4_hdr* tx_ip = (struct rte_ipv4_hdr*)(tx_eth + 1);
    struct rte_udp_hdr* tx_udp = (struct rte_udp_hdr*)(tx_ip + 1);

    // swap source and destination
    tx_eth->s_addr = rx_eth->d_addr;
    tx_eth->d_addr = rx_eth->s_addr;

    tx_ip->dst_addr = rx_ip->src_addr;
    tx_ip->src_addr = rx_ip->dst_addr;

    tx_udp->dst_port = rx_udp->src_port;
    tx_udp->src_port = rx_udp->dst_port;
    

    // for multiple request
    for (int request_idx = 0; request_idx < rx_packet->num_requests; request_idx++)
    {
        struct mehcached_request* rx_req = (struct mehcached_request *)rx_packet->data + request_idx;
        struct mehcached_request* tx_req = (struct mehcached_request *)tx_res->data + request_idx;
        // Operation result
        tx_req->operation = rx_req->operation;
        tx_req->result = rx_req->result;
        tx_req->key_hash = rx_req->key_hash;
        tx_req->kv_length_vec = rx_req->kv_length_vec;
        tx_req->expire_time = rx_req->expire_time;

        // Else...
        tx_req->reserved0 = 0;
        tx_req->reserved1 = rx_req->reserved1;
        
    }
    
}

void
setPacketToTxPipe(struct server_state *state, struct RXTXState* rx_tx_state)
{
    struct mehcached_batch_packet *packet = (struct mehcached_batch_packet *)rx_tx_state->pending_tx.current_tx_buffer;
    
    // update stats
    uint8_t request_index;
    const uint8_t *next_key = packet->data + sizeof(struct mehcached_request) * (size_t)packet->num_requests;
    for (request_index = 0; request_index < packet->num_requests; request_index++)
    {
        struct mehcached_request *req = (struct mehcached_request *)packet->data + request_index;

        state->num_operations_done++;

        if (*(const uint64_t *)next_key == 0)
            state->num_key0_operations_done++;

        if (req->result == MEHCACHED_OK)
            state->num_operations_succeeded++;

        next_key += MEHCACHED_ROUNDUP8(MEHCACHED_KEY_LENGTH(req->kv_length_vec)) + MEHCACHED_ROUNDUP8(MEHCACHED_VALUE_LENGTH(req->kv_length_vec));
    }

    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)rx_tx_state->pending_tx.current_tx_buffer;
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)((unsigned char *)eth + sizeof(struct rte_ether_hdr));
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)((unsigned char *)ip + sizeof(struct rte_ipv4_hdr));

    uint16_t packet_length = (uint16_t)(next_key - (uint8_t *)packet);

#ifdef MEHCACHED_ENABLE_THROTTLING
    // server load feedback
    // abuse the opaque field because it is going to be used for flow control anyway if clients can do full RX
    packet->opaque = state->target_request_rate;
#endif

    // TODO: update IP checksum

    // reset TTL
    ip->time_to_live = 64;

    ip->total_length = rte_cpu_to_be_16((uint16_t)(packet_length - sizeof(struct rte_ether_hdr)));
    udp->dgram_len = rte_cpu_to_be_16((uint16_t)(packet_length - sizeof(struct rte_ether_hdr) - sizeof(struct rte_ipv4_hdr)));

    // printf("IP Total Length:      %u bytes (network to host converted)\n", rte_be_to_cpu_16(ip->total_length));

}
#endif

static
int
mehcached_benchmark_server_proc(void *arg)
{
    struct server_state **states = (struct server_state **)arg;

    uint8_t thread_id = (uint8_t)rte_lcore_id();
    //printf("[SERVER] thread id = %u\n", thread_id);
    struct server_state *state = states[thread_id];
    if (state->server_conf == NULL) {
	printf("[ERROR] server_conf is NULL in state[%u]\n", thread_id);
	abort();
    }

    struct mehcached_server_conf *server_conf = state->server_conf;
    if (server_conf->threads == NULL) {
	printf("[ERROR] server_conf->threads is NULL\n");
	abort();
    }

    struct mehcached_server_thread_conf *thread_conf = &server_conf->threads[thread_id];
    //printf("[DEBUG] thread_conf accessed successfully for thread_id %u\n", thread_id);

#ifdef USE_HOT_ITEMS
    mehcached_calc_hot_item_hash(state->server_conf, &state->hot_item_hash);
#endif

    // for single core performance test
    // if (thread_id != 0 && thread_id != 1)
    //     return 0;

    if (thread_id != thread_id % (server_conf->num_threads >> state->cpu_mode))
        return 0;

    uint64_t t_start;
    uint64_t t_end;
    double diff;
    double prev_report = 0.;
    double prev_rate_update = 0.;
    uint64_t t_last_rx[NUM_PORT];
    uint64_t t_last_tx_flush[NUM_PORT];

    t_start = mehcached_stopwatch_now();

    {
        uint8_t port_id;
        for (port_id = 0; port_id < server_conf->num_ports; port_id++)
        {
            t_last_rx[port_id] = t_start;
            t_last_tx_flush[port_id] = t_start;
        }
    }

    uint64_t i = 0;

    uint64_t last_ipackets[NUM_PORT];
    uint64_t last_ierrors[NUM_PORT];
    uint64_t last_opackets[NUM_PORT];
    uint64_t last_oerrors[NUM_PORT];
    /*
    if (thread_id == 0)
    {
        fprintf(stderr, "[DEBUG] server thread 0 starting stats init...\n");
        uint8_t port_id;
        for (port_id = 0; port_id < server_conf->num_ports; port_id++)
        {
            fprintf(stderr, "[DEBUG] port_id: %u\n", port_id);
            struct rte_eth_stats stats;
            int ret = rte_eth_stats_get(port_id, &stats);
            if (ret < 0)
                fprintf(stderr, "[DEBUG] rte_eth_stats_get failed for port %u\n", port_id);
            else
                fprintf(stderr, "[DEBUG] ipackets = %lu\n", stats.ipackets);

            last_ipackets[port_id] = stats.ipackets;
            last_ierrors[port_id] = stats.ierrors;

            uint64_t opackets = 0;
            uint64_t oerrors = 0;
            uint8_t i;
            for (i = 0; i < server_conf->num_threads; i++)
            {
                fprintf(stderr, "[DEBUG] calling mehcached_get_stats_lcore(%u, %u)\n", port_id, i);
                uint64_t num_tx_sent;
                uint64_t num_tx_dropped;
                mehcached_get_stats_lcore(port_id, i, NULL, NULL, NULL, &num_tx_sent, &num_tx_dropped);
                opackets += num_tx_sent;
                oerrors += num_tx_dropped;
            }
            last_opackets[port_id] = opackets;
            last_oerrors[port_id] = oerrors;
        }
    }
    */    

#ifdef MICA_PCAP
    FILE* my_pcap_file;
    const char* filename;
    // write global header to pcap file
    if(thread_id == 0)
    {
        filename = get_filename(NUM_CORE, KEY_LEN, VAL_LEN, NUM_OP, BATCH_SZ, GET_RATIO, MICA);
	if(filename == NULL){
		fprintf(stderr, "[ERROR] Wrong file name\n");
		return 0;
	}
        my_pcap_file = fopen(filename, "wb");
        if (!my_pcap_file) {
                    fprintf(stderr, "Error: Failed to open PCAP file for writing!\n");
                        exit(EXIT_FAILURE);
        }
        write_pcap_header(my_pcap_file);
        fclose(my_pcap_file);
    }
#endif

#ifndef USE_ENSO
    const size_t pipeline_size = MEHCACHED_MAX_PKT_BURST;
    const size_t max_pending_packets = MEHCACHED_MAX_PKT_BURST;
#else
    const size_t enso_pipeline_size = 1024; // same as enso burst size
    const size_t max_pending_packets = 1024;
#endif
#define USE_STAGE_GAP

#ifndef USE_STAGE_GAP
    const uint64_t min_delay_stage0 = 100;
    const uint64_t min_delay_stage1 = 100;
    const uint64_t min_delay_stage2 = 100;
#else
    const size_t stage_gap = 2;
#endif

    size_t next_port_index = 0;

    #ifdef USE_ENSO
    // test thread_id only 0
    // ENSO Initializing
    // assume only 1 port
    /*=============== Enso Initializing ===============*/
    EnsoDevice_t* ensoDevice = rte_eth_enso_device_init(thread_id, 0);
    
    uint32_t target_size = 1536*enso_pipeline_size; // allocate size for TX buffer, need to change??
    
    int notif_ret = rte_eth_notif_init(ensoDevice);
    int rx_enso_ret = rte_eth_rx_enso_init(ensoDevice);
    int tx_enso_ret = rte_eth_tx_enso_init(ensoDevice);

    struct RXTXState rxTxState;
    rxTxState.pending_tx.count = 0;
    rxTxState.pending_tx.current_tx_buffer = NULL;
    rxTxState.pending_tx.start_tx_buffer = NULL;
    MicaProcessingUnit_t mica_unit;

    if(notif_ret < 0 || rx_enso_ret < 0 || tx_enso_ret < 0)
    {
        printf("failed to initialize ENSO\n");
        return 0;
    }
    else
        printf("======finish initializing ENSO buffer======\n");


    /*=============== Enso Initializing finished ===============*/

    #endif

#ifdef _GEM5_
    // system("cat /proc/meminfo | grep -i huge"); // check rte_zmalloc using hugepage
    fprintf(stderr, "Taking post-initialization checkpoint.\n");
    system("m5 checkpoint");
    //m5_checkpoint(0,0);
#endif

    printf("DPDK-version of mica is ready to accept requests!\n");
    printf("DPDK-version of mica burst size is %d \n", MEHCACHED_MAX_PKT_BURST);

    #if REQ_DEBUG == 1
    uint32_t counter_d = 0;
    uint32_t get_s = 0;
    uint32_t get_f = 0;
    uint32_t set_s = 0;
    uint32_t set_f = 0;
    uint32_t acc_p = 0;
    #endif

    uint32_t counter_d = 0;
    uint32_t get_s = 0;
    uint32_t get_f = 0;
    uint32_t acc_p = 0;

#ifndef USE_ENSO
    while (!exiting)
    {
#ifndef USE_STAGE_GAP
        uint64_t prefetch_time[pipeline_size];
#endif

        // invariant: 0 <= stage3_index <= stage2_index <= stage1_index <= stage0_index <= packet_count <= pipeline_size
        size_t packet_count = 0;
        size_t stage0_index = 0;
        size_t stage1_index = 0;
        size_t stage2_index = 0;
        size_t stage3_index = 0;

        // RX
        struct rte_mbuf *packet_mbufs[pipeline_size];
        // stage0
        struct mehcached_batch_packet *packets[pipeline_size];
        // stage1
#ifdef NETBENCH_SERVER_MEHCACHED
#ifdef USE_HOT_ITEMS
        uint16_t partition_ids[pipeline_size];
#endif
#endif
        // stage1 & stage2
#ifdef NETBENCH_SERVER_MEHCACHED
        struct mehcached_prefetch_state prefetch_state[pipeline_size][MEHCACHED_MAX_BATCH_SIZE];
#endif
        uint8_t port_id = thread_conf->port_ids[next_port_index];

        // while (true)
        // {
        //     struct rte_mbuf *mbuf = mehcached_receive_packet(port_id);
        //     if (mbuf == NULL)
        //         break;

        //     state->bytes_rx += (uint64_t)(mbuf->data_len + 24);   // 24 for PHY overheads

        //     packet_mbufs[packet_count] = mbuf;
        //     packet_count++;
        //     if (packet_count == pipeline_size)
        //         break;
        // }

        t_end = mehcached_stopwatch_now();
        // receive packets
        // the minimum retrieval interval of 1 us avoids excessive PCIe use, which causes slowdowns in skewed workloads
        // (most cores cause small batches, which reduces available bandwidth for the loaded cores)
        

        #if TIME_DIFF_POLL == 1
        if (t_end - t_last_rx[next_port_index] >= 1 * mehcached_stopwatch_1_usec)
        {
            packet_count = pipeline_size;
            mehcached_receive_packets(port_id, packet_mbufs, &packet_count);
            t_last_rx[next_port_index] = t_end;
        }
        #else
        packet_count = pipeline_size;
        mehcached_receive_packets(port_id, packet_mbufs, &packet_count);
        if(packet_count == 0) continue;
        //printf("DPDK-Version of MICA Server received %d Packets in a single Bursts!\n", (int)packet_count);
        #endif

        #if REQ_DEBUG == 1
        acc_p += packet_count;
        #endif
        

#ifdef MICA_PCAP
        // packet capture for RX packet
	if(thread_id == 0)
	{
        	for(uint16_t i = 0; i < packet_count; i++)
        	{
            		save_packet_to_pcap(packet_mbufs[i], filename, my_pcap_file);
        	}
	}
#endif



	
#ifdef MEHCACHED_USE_SOFT_FDIR
        {
            // enqueue to soft_fdir_mailbox
            size_t packet_index;
            for (packet_index = 0; packet_index < packet_count; packet_index++)
            {
                struct rte_mbuf *mbuf = packet_mbufs[packet_index];
                struct mehcached_batch_packet *packet = rte_pktmbuf_mtod(mbuf, struct mehcached_batch_packet *);
                struct mehcached_request *req = (struct mehcached_request *)packet->data + 0;
                uint64_t key_hash = req->key_hash;
                uint16_t partition_id = mehcached_get_partition_id(state, key_hash);
                uint8_t owner_thread_id = server_conf->partitions[partition_id].thread_id;
                uint8_t target_thread_id;
                // XXX: this does not support hot items
                if (req->operation == MEHCACHED_NOOP_READ || req->operation == MEHCACHED_GET)
                {
                    if (MEHCACHED_CONCURRENT_TABLE_READ(server_conf, partition_id))
                        target_thread_id = thread_id;
                    else
                        target_thread_id = owner_thread_id;
                }
                else
                {
                    if (MEHCACHED_CONCURRENT_TABLE_WRITE(server_conf, partition_id))
                        target_thread_id = thread_id;
                    else
                        target_thread_id = owner_thread_id;
                }

                // uint8_t mailbox_index = thread_id;
                uint8_t mailbox_index = (uint8_t)rte_lcore_to_socket_id(thread_id);
                // if (rte_ring_sp_enqueue(states[target_thread_id]->soft_fdir_mailbox[mailbox_index], mbuf) == -ENOBUFS)
                if (rte_ring_mp_enqueue(states[target_thread_id]->soft_fdir_mailbox[mailbox_index], mbuf) == -ENOBUFS)
                {
                    state->num_soft_fdir_dropped[port_id]++;
                    mehcached_packet_free(mbuf);
                }
            }
            // dequeue from soft_fdir_mailbox
            packet_count = 0;
            size_t mailbox_index;
            // for (mailbox_index = 0; mailbox_index < NUM_THREAD; mailbox_index++)
            for (mailbox_index = 0; mailbox_index < MEHCACHED_MAX_NUMA_NODES; mailbox_index++)
                packet_count += (size_t)rte_ring_sc_dequeue_burst(state->soft_fdir_mailbox[mailbox_index], (void **)(packet_mbufs + packet_count), (unsigned int)(pipeline_size - packet_count), NULL);
        }
#endif

        // update RX byte statistics
        {
            size_t packet_index;
            for (packet_index = 0; packet_index < packet_count; packet_index++)
                state->bytes_rx += (uint64_t)(packet_mbufs[packet_index]->data_len + 24);   // 24 for PHY overheads
        }


#ifndef USE_STAGE_GAP
        uint64_t t = mehcached_stopwatch_now();
#endif

        while (stage3_index < packet_count)
        {
#ifndef USE_STAGE_GAP
            if (stage0_index < packet_count && stage0_index - stage3_index < max_pending_packets)
#else
            if (stage0_index < packet_count && stage0_index - stage3_index < max_pending_packets && stage0_index - stage1_index < stage_gap)
#endif
            {
                struct rte_mbuf *mbuf = packet_mbufs[stage0_index];
                #if RXMBUF_ADDR == 1
                printf("[MICA] ------ %d ------ \n", stage3_index);
                printf("[MICA] rx mbuf physical addr 0x%lx \n", rte_mbuf_data_iova_default(mbuf));
                #endif
                struct mehcached_batch_packet *packet = packets[stage0_index] = rte_pktmbuf_mtod(mbuf, struct mehcached_batch_packet *);
                __builtin_prefetch(packet, 0, 0);
                __builtin_prefetch(&packet->data, 0, 0);
#ifndef USE_STAGE_GAP
                prefetch_time[stage0_index] = t;
#endif
                stage0_index++;
            }
#ifndef USE_STAGE_GAP
            else if (stage1_index < stage0_index && t - prefetch_time[stage1_index] >= min_delay_stage0)
#else
            else if (stage1_index < stage0_index && stage1_index - stage2_index < stage_gap)
#endif
            {
#ifdef NETBENCH_SERVER_MEHCACHED
                struct mehcached_batch_packet *packet = packets[stage1_index];
                assert(packet->num_requests <= MEHCACHED_MAX_BATCH_SIZE);
                uint8_t request_index;
                for (request_index = 0; request_index < packet->num_requests; request_index++)
                {
                    struct mehcached_request *req = (struct mehcached_request *)packet->data + request_index; // -> packet->data + 16B x request_index offset
                    if (req->operation != MEHCACHED_NOOP_READ && req->operation != MEHCACHED_NOOP_WRITE)
                    {
                        uint64_t key_hash = req->key_hash;
                        uint16_t partition_id;
#ifdef USE_HOT_ITEMS
                        if (request_index == 0)
                            partition_id = partition_ids[stage1_index] = mehcached_get_partition_id(state, key_hash);
                        else
                        {
                            partition_id = partition_ids[stage1_index];
                            assert(partition_id == mehcached_get_partition_id(state, key_hash));
                        }
#else
                        partition_id = mehcached_get_partition_id(state, key_hash);
#endif
                        struct mehcached_table *partition = state->partitions[partition_id];
                        mehcached_prefetch_table(partition, key_hash, &prefetch_state[stage1_index][request_index]);
                    }
#ifdef USE_HOT_ITEMS
                    else
                        partition_ids[stage1_index] = 0;  // any partition can handle no-op requests
#endif
                }
#endif
#ifndef USE_STAGE_GAP
                prefetch_time[stage1_index] = t;
#endif
                stage1_index++;
            }
#ifndef USE_STAGE_GAP
            else if (stage2_index < stage1_index && t - prefetch_time[stage2_index] >= min_delay_stage1)
#else
            else if (stage2_index < stage1_index && stage2_index - stage3_index < stage_gap)
#endif
            {
#ifdef NETBENCH_SERVER_MEHCACHED
                struct mehcached_batch_packet *packet = packets[stage2_index];
                uint8_t request_index;
                for (request_index = 0; request_index < packet->num_requests; request_index++)
                {
                    struct mehcached_request *req = (struct mehcached_request *)packet->data + request_index;
                    if (req->operation != MEHCACHED_NOOP_READ && req->operation != MEHCACHED_NOOP_WRITE)
                        mehcached_prefetch_alloc(&prefetch_state[stage2_index][request_index]);
                }
#endif
#ifndef USE_STAGE_GAP
                prefetch_time[stage2_index] = t;
#endif
                stage2_index++;
            }
#ifndef USE_STAGE_GAP
            else if (stage3_index < stage2_index && t - prefetch_time[stage3_index] >= min_delay_stage2)
#else
            else if (stage3_index < stage2_index)
#endif
            {
                struct rte_mbuf *mbuf = packet_mbufs[stage3_index];
                #if RXMBUF_ADDR == 1
                // log for first mbuf in every loop
                if (1)// (stage3_index == 0)
                {
                    printf("[MICA] ------ %d ------ \n", stage3_index);
                    printf("[MICA] rx mbuf physical addr 0x%lx \n", rte_mbuf_data_iova_default(mbuf));
                }
                
                #endif
                struct mehcached_batch_packet *packet = packets[stage3_index];
#ifdef NETBENCH_SERVER_MEHCACHED
#ifdef USE_HOT_ITEMS
                uint16_t partition_id = partition_ids[stage3_index];
#else
                struct mehcached_request *req = (struct mehcached_request *)packet->data + 0;
                uint16_t partition_id = mehcached_get_partition_id(state, req->key_hash);
#endif
#endif

                uint8_t new_key_values[RTE_ETHER_MAX_LEN - RTE_ETHER_CRC_LEN - sizeof(struct mehcached_batch_packet)];
                size_t new_key_value_length = RTE_ETHER_MAX_LEN - RTE_ETHER_CRC_LEN - sizeof(struct mehcached_batch_packet) - sizeof(struct mehcached_request) * (size_t)packet->num_requests;

#ifdef MEHCACHED_MEASURE_LATENCY
                uint32_t org_expire_time;
                {
                    struct mehcached_request *req = (struct mehcached_request *)packet->data + 0;
                    org_expire_time = req->expire_time;
                }
#endif

#ifdef NETBENCH_SERVER_MEHCACHED
                struct mehcached_table *partition = state->partitions[partition_id];
                //struct mehcached_request *requests = (struct mehcached_request *)packet->data;

                uint8_t alloc_id;
                if (partition_id < state->server_conf->num_partitions)
                    // alloc_id = (uint8_t)((MEHCACHED_CONCURRENT_TABLE_WRITE(state->server_conf, partition_id) && !MEHCACHED_CONCURRENT_ALLOC_WRITE(state->server_conf, partition_id)) ? (rte_lcore_id() >> 1) : 0);
                    alloc_id = (uint8_t)((MEHCACHED_CONCURRENT_TABLE_WRITE(state->server_conf, partition_id) && !MEHCACHED_CONCURRENT_ALLOC_WRITE(state->server_conf, partition_id)) ? rte_lcore_id() : 0);
                else
                    alloc_id = 0;

                bool readonly;
                if (thread_id == state->server_conf->partitions[partition_id].thread_id)
                    readonly = false;
                else if (MEHCACHED_CONCURRENT_TABLE_WRITE(state->server_conf, partition_id))
                    readonly = false;
                else
                    readonly = true;

#ifdef MEHCACHED_COLLECT_PER_PARTITION_LOAD
                state->num_per_partition_ops[partition_id] += packet->num_requests;
#endif

                //mehcached_process_batch(alloc_id, partition, requests, packet->num_requests, packet->data + sizeof(struct mehcached_request) * (size_t)packet->num_requests, new_key_values, &new_key_value_length, readonly);
#endif
//#if defined(NETBENCH_SERVER_MEMCACHED) || defined(NETBENCH_SERVER_MASSTREE)
                uint8_t *out_data_p = new_key_values;
                const uint8_t *out_data_end = out_data_p + new_key_value_length;

                uint8_t request_index;
                const uint8_t *in_data_p = packet->data + sizeof(struct mehcached_request) * (size_t)packet->num_requests;
                for (request_index = 0; request_index < packet->num_requests; request_index++)
                {
                    struct mehcached_request *req = (struct mehcached_request *)packet->data + request_index;
                    size_t key_length = MEHCACHED_KEY_LENGTH(req->kv_length_vec);
                    size_t value_length = MEHCACHED_VALUE_LENGTH(req->kv_length_vec);
                    const uint8_t *key = in_data_p;
                    const uint8_t *value = in_data_p + MEHCACHED_ROUNDUP8(key_length);
                    //const uint8_t *key = (const uint8_t *)&req->key_hash;
                    //key_length = sizeof(req->key_hash);
                    in_data_p += MEHCACHED_ROUNDUP8(key_length) + MEHCACHED_ROUNDUP8(value_length);

                    switch (req->operation)
                    {
                        case MEHCACHED_NOOP_READ:
                        case MEHCACHED_NOOP_WRITE:
                            {
                                req->result = MEHCACHED_OK;
                                req->kv_length_vec = 0;
                            }
                            break;
                        case MEHCACHED_ADD:
                        case MEHCACHED_SET:
                            {
                                if (0)
                                {
                                    // for debug: concurrent read/write validation
                                    uint64_t v = *(uint64_t *)value;
                                    if ((v & 0xffffffff) != ((~v >> 32) & 0xffffffff))
                                    {
                                        static unsigned int p = 0;
#ifndef NETBENCH_SERVER_MEHCACHED
                                        uint16_t partition_id = 0;
#endif
                                        if ((p++ & 63) == 0)
                                            fprintf(stderr, "thread %hhu partition %hu unexpected value being written: %lu %lu\n", thread_id, partition_id, (v & 0xffffffff), ((~v >> 32) & 0xffffffff));
                                    }
                                }
#ifdef NETBENCH_SERVER_MEHCACHED
                                if (mehcached_set(alloc_id, partition, req->key_hash, key, key_length, value, value_length, req->expire_time, req->operation == MEHCACHED_SET))
#endif
#ifdef NETBENCH_SERVER_MEMCACHED
                                if (process_update((char *)key, key_length, (char *)value, value_length, false, true, false))
#endif
#ifdef NETBENCH_SERVER_MASSTREE
                                if (masstree_put(state->masstree, thread_id, (const char *)key, key_length, (const char *)value, value_length))
#endif
#ifdef NETBENCH_SERVER_RAMCLOUD
                                if (ramcloud_put(state->ramcloud, thread_id, (const char *)key, key_length, (const char *)value, value_length))
#endif
                                {
                                    req->result = MEHCACHED_OK;
                                    #if REQ_DEBUG == 1
                                    set_s++;
                                    #endif
                                }
                                else
                                {
                                    req->result = MEHCACHED_ERROR;
                                    #if REQ_DEBUG == 1
                                    set_f++;
                                    #endif
                                }
                                req->kv_length_vec = 0;
                            }
                            break;
                        case MEHCACHED_GET:
                            {
                                size_t out_value_length = (size_t)(out_data_end - out_data_p);
                                uint8_t *out_value = out_data_p;
#ifdef NETBENCH_SERVER_MEHCACHED
                                if (mehcached_get(alloc_id, partition, req->key_hash, key, key_length, out_value, &out_value_length, &req->expire_time, readonly))
#endif
#ifdef NETBENCH_SERVER_MEMCACHED
                                if (process_get((char *)key, key_length, (char *)out_value, &out_value_length))
#endif
#ifdef NETBENCH_SERVER_MASSTREE
                                if (masstree_get(state->masstree, thread_id, (const char *)key, key_length, (char *)out_value, &out_value_length))
#endif
#ifdef NETBENCH_SERVER_RAMCLOUD
                                if (ramcloud_get(state->ramcloud, thread_id, (const char *)key, key_length, (char *)out_value, &out_value_length))
#endif
                                {
                                    req->result = MEHCACHED_OK;
                                    #if REQ_DEBUG == 1
                                    uint64_t v = *(uint64_t *)out_value;
                                    printf("[GET] Req success\n");
                                    printf("value 0x%lx\n", v);
                                    get_s++;
                                    #endif

                                    if (0)
                                    {
                                        // for debug: concurrent read/write validation
                                        uint64_t v = *(uint64_t *)out_value;
                                        if ((v & 0xffffffff) != ((~v >> 32) & 0xffffffff))
                                        {
                                            static unsigned int p = 0;
    #ifndef NETBENCH_SERVER_MEHCACHED
                                            uint16_t partition_id = 0;
    #endif
                                            if ((p++ & 63) == 0)
                                                fprintf(stderr, "thread %hhu partition %hu unexpected value being read: %lu %lu\n", thread_id, partition_id, (v & 0xffffffff), ((~v >> 32) & 0xffffffff));
                                        }
                                    }
                                }
                                else
                                {
                                    req->result = MEHCACHED_ERROR;  // TODO: return a correct failure code
                                    out_value_length = 0;
                                    #if REQ_DEBUG == 1
                                    get_f++;
                                    #endif
                                }
                                req->kv_length_vec = MEHCACHED_KV_LENGTH_VEC(0, out_value_length);
#ifndef NETBENCH_SERVER_MEHCACHED
                                req->expire_time = 0;
#endif
                                out_data_p += MEHCACHED_ROUNDUP8(out_value_length);
                            }
                            break;
                        case MEHCACHED_INCREMENT:
                            {
                                // TODO: check if the output space is large enough
                                // TODO: use expire_time
                                uint64_t increment;
                                assert(value_length == sizeof(uint64_t));
                                mehcached_memcpy8((uint8_t *)&increment, value, sizeof(uint64_t));
                                size_t out_value_length = sizeof(uint64_t);
                                uint8_t *out_value = out_data_p;
#ifdef NETBENCH_SERVER_MEHCACHED
                                if (mehcached_increment(alloc_id, partition, req->key_hash, key, key_length, increment, (uint64_t *)out_value, req->expire_time))
#endif
#ifdef NETBENCH_SERVER_MEMCACHED
                                if (process_add_delta((char *)key, key_length, increment, (uint64_t *)out_value))
#endif
#ifdef NETBENCH_SERVER_MASSTREE
                                (void)out_value;
                                if (0)  // masstree does not have native support for atomic increment operations
#endif
#ifdef NETBENCH_SERVER_RAMCLOUD
                                (void)out_value;
                                if (0)  // ramcloud does not have native support for atomic increment operations
#endif
                                    req->result = MEHCACHED_OK;
                                else
                                {
                                    req->result = MEHCACHED_ERROR;  // TODO: return a correct failure code
                                    out_value_length = 0;
                                }
                                req->kv_length_vec = MEHCACHED_KV_LENGTH_VEC(0, out_value_length);
                                out_data_p += MEHCACHED_ROUNDUP8(out_value_length);
                            }
                            break;
                        default:
                            fprintf(stderr, "invalid operation or not implemented\n");
                            break;
                    }
                }
                new_key_value_length = (size_t)(out_data_p - new_key_values);
//#endif

                rte_memcpy(packet->data + sizeof(struct mehcached_request) * (size_t)packet->num_requests, new_key_values, new_key_value_length);
                // new packet length will be calculated in mehcached_remote_send_response()

#ifdef MEHCACHED_MEASURE_LATENCY
                {
                    struct mehcached_request *req = (struct mehcached_request *)packet->data + 0;
                    req->expire_time = org_expire_time;
                }
#endif

		mehcached_remote_send_response(state, mbuf, port_id);
#ifdef MICA_PCAP
                // packet capture 시점 Response
                if(thread_id == 0)
			save_packet_to_pcap(mbuf, filename, my_pcap_file);
#endif


                stage3_index++;
                #if REQ_DEBUG == 1
                counter_d++;
                #endif
            }
#ifndef USE_STAGE_GAP
            else
                t = (uint32_t)mehcached_stopwatch_now();
#endif
        

        }
        #if REQ_DEBUG == 1
        if((counter_d > 0) && (counter_d % 512 == 0))
        {
            printf("------acc statistics------\n");
            printf("Acc packet %u\n", acc_p);
            printf("GET success %u\n", get_s);
            printf("GET failed %u\n", get_f);
            printf("SET success %u\n", set_s);
            printf("SET failed %u\n", set_f);
            fflush(stdout);
        }
        
        #endif

        t_end = mehcached_stopwatch_now();

        //if (packet_count < pipeline_size)
        {
            // send out packets (improves latency under low throughput)
            // the minimum flush interval of 10 us avoids excessive PCIe use, which causes slowdowns in skewed workloads
            // (most cores cause small batches, which reduces available bandwidth for the loaded cores)
            // in other words, no TX packets will stay in the application-level queue much longer than 10 us
            if (t_end - t_last_tx_flush[next_port_index] >= 10 * mehcached_stopwatch_1_usec)
            {
                t_last_tx_flush[next_port_index] = t_end;
                mehcached_send_packet_flush(port_id);
            }

            next_port_index += (uint8_t)(1 << state->port_mode);
            if (next_port_index >= thread_conf->num_ports)
                next_port_index = 0;
        }

        /*
        i++;

        if ((i & 0xff) == 0)
        {
            uint64_t total_num_rx_burst = 0;
            uint64_t total_num_rx_received = 0;
            uint64_t total_num_tx_sent = 0;
            uint64_t total_num_tx_dropped = 0;
            size_t port_index;
            for (port_index = 0; port_index < thread_conf->num_ports; port_index++)
            {
                uint8_t port_id = thread_conf->port_ids[port_index];
                uint64_t num_rx_burst;
                uint64_t num_rx_received;
                uint64_t num_tx_sent;
                uint64_t num_tx_dropped;
                mehcached_get_stats(port_id, &num_rx_burst, &num_rx_received, NULL, &num_tx_sent, &num_tx_dropped);
                total_num_rx_burst += num_rx_burst;
                total_num_rx_received += num_rx_received;
                total_num_tx_sent += num_tx_sent;
                total_num_tx_dropped += num_tx_dropped;
            }
            state->num_rx_burst = total_num_rx_burst;
            state->num_rx_received = total_num_rx_received;
            state->num_tx_sent = total_num_tx_sent;
            state->num_tx_dropped = total_num_tx_dropped;

            //t_end = mehcached_stopwatch_now();
            diff = mehcached_stopwatch_diff_in_s(t_end, t_start);

            if (diff - prev_report >= 1.)
            {
                if (thread_id == 0)
                {
                    uint64_t total_new_num_tx_sent = 0;
                    uint64_t total_new_num_tx_dropped = 0;
                    uint64_t total_new_bytes_rx = 0;
                    uint64_t total_new_bytes_tx = 0;
                    uint64_t total_new_num_operations_done = 0;
                    uint64_t total_new_num_key0_operations_done = 0;
                    uint64_t total_new_num_operations_succeeded = 0;
                    size_t num_active_threads = 0;

                    size_t thread_id;
                    for (thread_id = 0; thread_id < server_conf->num_threads; thread_id++)
                    {
                        uint64_t num_tx_sent = states[thread_id]->num_tx_sent;
                        uint64_t new_num_tx_sent = num_tx_sent - states[thread_id]->last_num_tx_sent;
                        states[thread_id]->last_num_tx_sent = num_tx_sent;

                        uint64_t num_tx_dropped = states[thread_id]->num_tx_dropped;
                        uint64_t new_num_tx_dropped = num_tx_dropped - states[thread_id]->last_num_tx_dropped;
                        states[thread_id]->last_num_tx_dropped = num_tx_dropped;

                        uint64_t bytes_rx = states[thread_id]->bytes_rx;
                        uint64_t new_bytes_rx = bytes_rx - states[thread_id]->last_bytes_rx;
                        states[thread_id]->last_bytes_rx = bytes_rx;

                        uint64_t bytes_tx = states[thread_id]->bytes_tx;
                        uint64_t new_bytes_tx = bytes_tx - states[thread_id]->last_bytes_tx;
                        states[thread_id]->last_bytes_tx = bytes_tx;

                        uint64_t num_operations_done = states[thread_id]->num_operations_done;
                        uint64_t new_num_operations_done = num_operations_done - states[thread_id]->last_num_operations_done;
                        states[thread_id]->last_num_operations_done = num_operations_done;

                        uint64_t num_key0_operations_done = states[thread_id]->num_key0_operations_done;
                        uint64_t new_num_key0_operations_done = num_key0_operations_done - states[thread_id]->last_num_key0_operations_done;
                        states[thread_id]->last_num_key0_operations_done = num_key0_operations_done;

                        uint64_t num_operations_succeeded = states[thread_id]->num_operations_succeeded;
                        uint64_t new_num_operations_succeeded = num_operations_succeeded - states[thread_id]->last_num_operations_succeeded;
                        states[thread_id]->last_num_operations_succeeded = num_operations_succeeded;

                        total_new_num_tx_sent += new_num_tx_sent;
                        total_new_num_tx_dropped += new_num_tx_dropped;
                        total_new_bytes_rx += new_bytes_rx;
                        total_new_bytes_tx += new_bytes_tx;
                        total_new_num_operations_done += new_num_operations_done;
                        total_new_num_key0_operations_done += new_num_key0_operations_done;
                        total_new_num_operations_succeeded += new_num_operations_succeeded;

                        if (new_bytes_tx != 0)
                            num_active_threads++;
                    }

                    double effective_tx_ratio = 0.;
                    if (total_new_num_tx_sent + total_new_num_tx_dropped != 0)
                        effective_tx_ratio = (double)total_new_num_tx_sent / (double)(total_new_num_tx_sent + total_new_num_tx_dropped);

                    double success_rate = 0.;
                    if (total_new_num_operations_done != 0 && effective_tx_ratio > 0.)
                    {
                        success_rate = (double)total_new_num_operations_succeeded / ((double)total_new_num_operations_done * effective_tx_ratio);
                        if (success_rate > 1.)
                            success_rate = 1.;  // total_new_num_operations_succeeded & total_new_num_operations_done are measured for a different set of requests
                    }

                    double total_mops = (double)total_new_num_operations_done * effective_tx_ratio;
                    double key0_mops;
                    if (total_new_num_operations_done != 0)
                        key0_mops = total_mops * ((double)total_new_num_key0_operations_done / (double)total_new_num_operations_done);
                    else
                        key0_mops = 0.;
                    total_mops *= 1. / (diff - prev_report) * 0.000001;
                    key0_mops *= 1. / (diff - prev_report) * 0.000001;
                    double gbps_rx = (double)total_new_bytes_rx / (diff - prev_report) * 8 * 0.000000001;
                    double gbps_tx = (double)total_new_bytes_tx / (diff - prev_report) * 8 * 0.000000001 * effective_tx_ratio;

                    printf("%.1f current_ops: %10.2lf Mops (%10.2lf Mops for key0); success_rate: %3.2f%%", diff, total_mops, key0_mops, success_rate * 100.);

                    printf("; bw: %.2lf Gbps (rx), %.2lf Gbps (tx); threads: %zu", gbps_rx, gbps_tx, num_active_threads);

                    printf("; target_request_rate: %.3f Mops (s)", (float)state->target_request_rate * 0.000001f);

                    printf("; avg_rx_burst");
                    {
                        uint64_t total_num_rx_burst = 0;
                        uint64_t total_num_rx_received = 0;
                        for (thread_id = 0; thread_id < server_conf->num_threads; thread_id++)
                        {
                            uint64_t num_rx_burst = states[thread_id]->num_rx_burst;
                            uint64_t new_num_rx_burst = num_rx_burst - states[thread_id]->last_num_rx_burst;
                            states[thread_id]->last_num_rx_burst = num_rx_burst;

                            uint64_t num_rx_received = states[thread_id]->num_rx_received;
                            uint64_t new_num_rx_received = num_rx_received - states[thread_id]->last_num_rx_received;
                            states[thread_id]->last_num_rx_received = num_rx_received;

                            double avg_rx_burst_size = 0.;
                            if (new_num_rx_burst != 0)
                                avg_rx_burst_size = (double)new_num_rx_received / (double)new_num_rx_burst;

                            total_num_rx_burst += new_num_rx_burst;
                            total_num_rx_received += new_num_rx_received;

                            printf(" %zu[%.3f]", thread_id, avg_rx_burst_size);
                        }
                        {
                            double avg_rx_burst_size = 0.;
                            if (total_num_rx_burst != 0)
                                avg_rx_burst_size = (double)total_num_rx_received / (double)total_num_rx_burst;
                            printf(" avg[%.3f]", avg_rx_burst_size);
                        }
                    }

#ifdef MEHCACHED_COLLECT_PER_PARTITION_LOAD
                    // calculate per-partition load
                    printf("; load");
                    uint64_t total_num_per_partition_ops[server_conf->num_partitions];
                    memset(total_num_per_partition_ops, 0, sizeof(total_num_per_partition_ops));
                    uint64_t max_ops = 1;

                    size_t partition_id;
                    for (thread_id = 0; thread_id < server_conf->num_threads; thread_id++)
                    {
                        for (partition_id = 0; partition_id < server_conf->num_partitions; partition_id++)
                        {
                            uint64_t num_per_partition_ops = states[thread_id]->num_per_partition_ops[partition_id];
                            uint64_t new_num_per_partition_ops = num_per_partition_ops - states[thread_id]->last_num_per_partition_ops[partition_id];
                            states[thread_id]->last_num_per_partition_ops[partition_id] = num_per_partition_ops;
                            total_num_per_partition_ops[partition_id] += new_num_per_partition_ops;
                        }
                    }
                    for (partition_id = 0; partition_id < server_conf->num_partitions; partition_id++)
                        if (max_ops < total_num_per_partition_ops[partition_id])
                            max_ops = total_num_per_partition_ops[partition_id];
                    for (partition_id = 0; partition_id < server_conf->num_partitions; partition_id++)
                        printf(" %zu[%.3f]", partition_id, (double)total_num_per_partition_ops[partition_id] / (double)max_ops);
#endif

#ifdef MEHCACHED_COLLECT_STATS
                    uint16_t partition_id;
                    for (partition_id = 0; partition_id < server_conf->num_partitions + server_conf->num_threads; partition_id++)
                        mehcached_print_stats(state->partitions[partition_id]);
#endif

                    printf("\n");
                    fflush(stdout);
                }

                prev_report = diff;
            }

            if (diff - prev_rate_update >= 0.1)
            {
                if (thread_id == 0)
                {
                    float max_loss = 0.;

                    uint8_t port_id;
                    //uint8_t loss_port_id = 0;
                    for (port_id = 0; port_id < server_conf->num_ports; port_id++)
                    {
                        struct rte_eth_stats stats;
                        rte_eth_stats_get(port_id, &stats);

#ifndef MEHCACHED_USE_SOFT_FDIR
                        uint64_t new_ipackets = stats.ipackets - last_ipackets[port_id];
                        last_ipackets[port_id] = stats.ipackets;
                        uint64_t new_ierrors = stats.ierrors - last_ierrors[port_id];
                        last_ierrors[port_id] = stats.ierrors;
#else
                        uint64_t ipackets = stats.ipackets;
                        uint64_t ierrors = stats.ierrors;
                        // treat dropped packets by soft fdir as dropped packets by NIC RX
                        {
                            uint8_t thread_id;
                            for (thread_id = 0; thread_id < server_conf->num_threads; thread_id++)
                            {
                                uint64_t num_soft_fdir_dropped = states[thread_id]->num_soft_fdir_dropped[port_id];
                                ipackets -= num_soft_fdir_dropped;
                                ierrors += num_soft_fdir_dropped;
                            }
                        }
                        uint64_t new_ipackets = ipackets - last_ipackets[port_id];
                        last_ipackets[port_id] = ipackets;
                        uint64_t new_ierrors = ierrors - last_ierrors[port_id];
                        last_ierrors[port_id] = ierrors;
#endif
                        // uint64_t new_opackets = stats.opackets - last_opackets[port_id];
                        // last_opackets[port_id] = stats.opackets;
                        // uint64_t new_oerrors = stats.oerrors - last_oerrors[port_id];
                        // last_oerrors[port_id] = stats.oerrors;

                        float iloss;
                        // float oloss;
                        if (new_ipackets + new_ierrors != 0)
                            iloss = (float)new_ierrors / (float)(new_ipackets + new_ierrors);
                        else
                            iloss = 0.f;
                        // if (new_opackets != 0)
                        //     oloss = (float)new_oerrors / (float)new_opackets;
                        // else
                        //     oloss = 0.f;

                        if (max_loss < iloss)
                        {
                            max_loss = iloss;
                            // loss_port_id = port_id;
                        }
                        // if (max_loss < oloss)
                        //     max_loss = oloss;

#ifndef NDEBUG
                        if (stats.fdirmiss != 0)
                            printf("non-zero fdirmiss on port %hhu\n", port_id);
                        if (stats.rx_nombuf != 0)
                            printf("non-zero rx_nombuf on port %hhu\n", port_id);
#endif
                        // printf(" port %hhu new_ipackets %lu new_ierrors %lu", port_id, new_ipackets, new_ierrors);
                    }
                    // printf(" max_loss i %f", max_loss);
                    // if (max_loss != 0)
                    //     printf("iloss port %hhu\n", loss_port_id);

                    for (port_id = 0; port_id < server_conf->num_ports; port_id++)
                    {
                        uint64_t opackets = 0;
                        uint64_t oerrors = 0;
                        uint8_t thread_id;
                        for (thread_id = 0; thread_id < server_conf->num_threads; thread_id++)
                        {
                            uint64_t num_tx_sent;
                            uint64_t num_tx_dropped;
                            mehcached_get_stats_lcore(port_id, thread_id, NULL, NULL, NULL, &num_tx_sent, &num_tx_dropped);
                            opackets += num_tx_sent;
                            oerrors += num_tx_dropped;
                            // XXX: how to handle integer wraps after a very long run?
                        }
                        uint64_t new_opackets = opackets - last_opackets[port_id];
                        last_opackets[port_id] = opackets;
                        uint64_t new_oerrors = oerrors - last_oerrors[port_id];
                        last_oerrors[port_id] = oerrors;

                        float oloss;
                        if (new_opackets + new_oerrors != 0)
                            oloss = (float)new_oerrors / (float)(new_opackets + new_oerrors);
                        else
                            oloss = 0.f;

                        if (max_loss < oloss)
                            max_loss = oloss;
                    }
                    // printf(" max_loss io %f", max_loss);

                    uint32_t new_target_request_rate;
                    if (max_loss <= 0.01f)
                        new_target_request_rate = (uint32_t)((float)state->target_request_rate * (1.f + (0.01f - max_loss)));
                    else if (max_loss <= 0.02f)
                        new_target_request_rate = (uint32_t)((float)state->target_request_rate / (1.f + (max_loss - 0.01f)));
                    else
                        new_target_request_rate = (uint32_t)((float)state->target_request_rate / (1.f + (0.02f - 0.01f)));
                    if (new_target_request_rate < 1000) // cap at 1 Kops
                        new_target_request_rate = 1000;
                    if (new_target_request_rate > 20000000) // cap at 20 Mops
                        new_target_request_rate = 20000000;

#ifdef MEHCACHED_MEASURE_LATENCY
                    if (target_request_rate_from_user > 0)
                        new_target_request_rate = target_request_rate_from_user;
#endif

                    uint8_t thread_id;
                    for (thread_id = 0; thread_id < server_conf->num_threads; thread_id++)
                        states[thread_id]->target_request_rate = new_target_request_rate;
                }

                prev_rate_update = diff;
            }
            
        }
        */
    }

#else

    
    while (!exiting)
    {
        // invariant: 0 <= stage3_index <= stage2_index <= stage1_index <= stage0_index <= packet_count <= pipeline_size
        size_t packet_count = 0;
        size_t stage0_index = 0;
        size_t stage1_index = 0;
        size_t stage2_index = 0;
        size_t stage3_index = 0;

        // RX
        //struct rte_mbuf *packet_mbufs[pipeline_size];

        // SW, change arraysize -> enso burst size 1024
        // stage0
        struct mehcached_batch_packet *packets[enso_pipeline_size];
        // stage1
        // stage1 & stage2
#ifdef NETBENCH_SERVER_MEHCACHED
        struct mehcached_prefetch_state prefetch_state[enso_pipeline_size][MEHCACHED_MAX_BATCH_SIZE];
#endif
        uint8_t port_id = thread_conf->port_ids[next_port_index];

        // SW, Enso logic
        // ENSO running process demo
        uint8_t* buf = NULL;
    
        uint32_t next_rx = rte_eth_rx_enso_next(ensoDevice);
        if(next_rx < 0) continue;

        // receive packets
        uint32_t new_byte = rte_eth_rx_enso_burst(ensoDevice, &buf);
        assert(buf);
        if(new_byte == 0) continue;
        printf("======== Recieve %u bytes from Rx pipe ========\n", new_byte);

        // set up tx buffer
        uint8_t* tx_buf = rte_eth_alloc_tx_buffer(ensoDevice, target_size);
        assert(tx_buf);
        rxTxState.pending_tx.current_tx_buffer = tx_buf;
        rxTxState.pending_tx.start_tx_buffer = tx_buf;
        setProcessingUnit(ensoDevice->rx_pipe, &mica_unit, new_byte, buf, enso_pipeline_size);


        t_end = mehcached_stopwatch_now();
        

        // update RX byte statistics
        {
            // size_t packet_index;
            // for (packet_index = 0; packet_index < packet_count; packet_index++)
            //     state->bytes_rx += (uint64_t)(packet_mbufs[packet_index]->data_len + 24);   // 24 for PHY overheads
            state->bytes_rx += new_byte; 
        }


        while (stage3_index < mica_unit.end)
        {
          if (stage0_index < mica_unit.end && stage0_index - stage3_index < max_pending_packets && stage0_index - stage1_index < stage_gap)
            {
                //struct rte_mbuf *mbuf = packet_mbufs[stage0_index];

                packets[stage0_index] = getPacketFromRxPipe(&mica_unit);
                
                //rte_pktmbuf_mtod(mbuf, struct mehcached_batch_packet *);
                if (packets[stage0_index] != NULL)
                {
                    dumpRxPktInfo(packets[stage0_index]);
                    //printf("  [Stage0] limit pkt %u , prefetch %u pkt\n", mica_unit.end, stage0_index);
                    stage0_index++;
                }
                else
                {
                    //printf("  [Stage0] NULL packet returned! limit %u , current %u\n", mica_unit.end, stage0_index);
                    // current stage0 ptr to NULL so, -1
                }        
            }
            else if (stage1_index < stage0_index && stage1_index - stage2_index < stage_gap)
            {
#ifdef NETBENCH_SERVER_MEHCACHED
                struct mehcached_batch_packet *packet = packets[stage1_index];
                assert(packet->num_requests <= MEHCACHED_MAX_BATCH_SIZE);
                uint8_t request_index;
                for (request_index = 0; request_index < packet->num_requests; request_index++)
                {
                    struct mehcached_request *req = (struct mehcached_request *)packet->data + request_index; // -> packet->data + 16B x request_index offset
                    if (req->operation != MEHCACHED_NOOP_READ && req->operation != MEHCACHED_NOOP_WRITE)
                    {
                        uint64_t key_hash = req->key_hash;
                        uint16_t partition_id;
                        partition_id = mehcached_get_partition_id(state, key_hash);
#endif
                        struct mehcached_table *partition = state->partitions[partition_id];
                        mehcached_prefetch_table(partition, key_hash, &prefetch_state[stage1_index][request_index]);
                    }
                }
                stage1_index++;
            }
            else if (stage2_index < stage1_index && stage2_index - stage3_index < stage_gap)
            {
#ifdef NETBENCH_SERVER_MEHCACHED
                struct mehcached_batch_packet *packet = packets[stage2_index];
                uint8_t request_index;
                for (request_index = 0; request_index < packet->num_requests; request_index++)
                {
                    struct mehcached_request *req = (struct mehcached_request *)packet->data + request_index;
                    if (req->operation != MEHCACHED_NOOP_READ && req->operation != MEHCACHED_NOOP_WRITE)
                        mehcached_prefetch_alloc(&prefetch_state[stage2_index][request_index]);
                }
#endif
                stage2_index++;
            }
            else if (stage3_index < stage2_index)
            {
                //struct rte_mbuf *mbuf = packet_mbufs[stage3_index];
                struct mehcached_batch_packet *response_packet = (struct mehcached_batch_packet *)rxTxState.pending_tx.current_tx_buffer;

                struct mehcached_batch_packet *packet = packets[stage3_index];
#ifdef NETBENCH_SERVER_MEHCACHED
                struct mehcached_request *req = (struct mehcached_request *)packet->data + 0;
                uint16_t partition_id = mehcached_get_partition_id(state, req->key_hash);
#endif

                uint8_t new_key_values[RTE_ETHER_MAX_LEN - RTE_ETHER_CRC_LEN - sizeof(struct mehcached_batch_packet)];
                size_t new_key_value_length = RTE_ETHER_MAX_LEN - RTE_ETHER_CRC_LEN - sizeof(struct mehcached_batch_packet) - sizeof(struct mehcached_request) * (size_t)packet->num_requests;

#ifdef NETBENCH_SERVER_MEHCACHED
                struct mehcached_table *partition = state->partitions[partition_id];
                //struct mehcached_request *requests = (struct mehcached_request *)packet->data;

                uint8_t alloc_id;
                if (partition_id < state->server_conf->num_partitions)
                    // alloc_id = (uint8_t)((MEHCACHED_CONCURRENT_TABLE_WRITE(state->server_conf, partition_id) && !MEHCACHED_CONCURRENT_ALLOC_WRITE(state->server_conf, partition_id)) ? (rte_lcore_id() >> 1) : 0);
                    alloc_id = (uint8_t)((MEHCACHED_CONCURRENT_TABLE_WRITE(state->server_conf, partition_id) && !MEHCACHED_CONCURRENT_ALLOC_WRITE(state->server_conf, partition_id)) ? rte_lcore_id() : 0);
                else
                    alloc_id = 0;

                bool readonly;
                if (thread_id == state->server_conf->partitions[partition_id].thread_id)
                    readonly = false;
                else if (MEHCACHED_CONCURRENT_TABLE_WRITE(state->server_conf, partition_id))
                    readonly = false;
                else
                    readonly = true;

#ifdef MEHCACHED_COLLECT_PER_PARTITION_LOAD
                state->num_per_partition_ops[partition_id] += packet->num_requests;
#endif

                //mehcached_process_batch(alloc_id, partition, requests, packet->num_requests, packet->data + sizeof(struct mehcached_request) * (size_t)packet->num_requests, new_key_values, &new_key_value_length, readonly);
#endif
//#if defined(NETBENCH_SERVER_MEMCACHED) || defined(NETBENCH_SERVER_MASSTREE)
                uint8_t *out_data_p = new_key_values;
                const uint8_t *out_data_end = out_data_p + new_key_value_length;

                uint8_t request_index;
                const uint8_t *in_data_p = packet->data + sizeof(struct mehcached_request) * (size_t)packet->num_requests;
                for (request_index = 0; request_index < packet->num_requests; request_index++)
                {
                    struct mehcached_request *req = (struct mehcached_request *)packet->data + request_index;
                    size_t key_length = MEHCACHED_KEY_LENGTH(req->kv_length_vec);
                    size_t value_length = MEHCACHED_VALUE_LENGTH(req->kv_length_vec);
                    const uint8_t *key = in_data_p;
                    const uint8_t *value = in_data_p + MEHCACHED_ROUNDUP8(key_length);
                    //const uint8_t *key = (const uint8_t *)&req->key_hash;
                    //key_length = sizeof(req->key_hash);
                    in_data_p += MEHCACHED_ROUNDUP8(key_length) + MEHCACHED_ROUNDUP8(value_length);

                    switch (req->operation)
                    {
                        case MEHCACHED_NOOP_READ:
                        case MEHCACHED_NOOP_WRITE:
                            {
                                req->result = MEHCACHED_OK;
                                req->kv_length_vec = 0;
                            }
                            break;
                        case MEHCACHED_ADD:
                        case MEHCACHED_SET:
                            {
                                if (0)
                                {
                                    // for debug: concurrent read/write validation
                                    uint64_t v = *(uint64_t *)value;
                                    if ((v & 0xffffffff) != ((~v >> 32) & 0xffffffff))
                                    {
                                        static unsigned int p = 0;

                                        if ((p++ & 63) == 0)
                                            fprintf(stderr, "thread %hhu partition %hu unexpected value being written: %lu %lu\n", thread_id, partition_id, (v & 0xffffffff), ((~v >> 32) & 0xffffffff));
                                    }
                                }
#ifdef NETBENCH_SERVER_MEHCACHED
                                if (mehcached_set(alloc_id, partition, req->key_hash, key, key_length, value, value_length, req->expire_time, req->operation == MEHCACHED_SET))
#endif
                                {
                                    req->result = MEHCACHED_OK;
                                }
                                else
                                {
                                    req->result = MEHCACHED_ERROR;
                                }
                                req->kv_length_vec = 0;
                            }
                            break;
                        case MEHCACHED_GET:
                            {
                                size_t out_value_length = (size_t)(out_data_end - out_data_p);
                                uint8_t *out_value = out_data_p;
#ifdef NETBENCH_SERVER_MEHCACHED
                                if (mehcached_get(alloc_id, partition, req->key_hash, key, key_length, out_value, &out_value_length, &req->expire_time, readonly))
#endif
                                {
                                    req->result = MEHCACHED_OK;
                                    //debug
                                    get_s++;


                                    if (0)
                                    {
                                        // for debug: concurrent read/write validation
                                        uint64_t v = *(uint64_t *)out_value;
                                        if ((v & 0xffffffff) != ((~v >> 32) & 0xffffffff))
                                        {
                                            static unsigned int p = 0;
                                            if ((p++ & 63) == 0)
                                                fprintf(stderr, "thread %hhu partition %hu unexpected value being read: %lu %lu\n", thread_id, partition_id, (v & 0xffffffff), ((~v >> 32) & 0xffffffff));
                                        }
                                    }
                                }
                                else
                                {
                                    req->result = MEHCACHED_ERROR;  // TODO: return a correct failure code
                                    out_value_length = 0;
                                    //debug
                                    get_f++;
                                }
                                req->kv_length_vec = MEHCACHED_KV_LENGTH_VEC(0, out_value_length);
                                out_data_p += MEHCACHED_ROUNDUP8(out_value_length);
                            }
                            break;
                        case MEHCACHED_INCREMENT:
                            {
                                // TODO: check if the output space is large enough
                                // TODO: use expire_time
                                uint64_t increment;
                                assert(value_length == sizeof(uint64_t));
                                mehcached_memcpy8((uint8_t *)&increment, value, sizeof(uint64_t));
                                size_t out_value_length = sizeof(uint64_t);
                                uint8_t *out_value = out_data_p;
#ifdef NETBENCH_SERVER_MEHCACHED
                                if (mehcached_increment(alloc_id, partition, req->key_hash, key, key_length, increment, (uint64_t *)out_value, req->expire_time))
#endif
                                    req->result = MEHCACHED_OK;
                                else
                                {
                                    req->result = MEHCACHED_ERROR;  // TODO: return a correct failure code
                                    out_value_length = 0;
                                }
                                req->kv_length_vec = MEHCACHED_KV_LENGTH_VEC(0, out_value_length);
                                out_data_p += MEHCACHED_ROUNDUP8(out_value_length);
                            }
                            break;
                        default:
                            fprintf(stderr, "invalid operation or not implemented\n");
                            break;
                    }
                }
                new_key_value_length = (size_t)(out_data_p - new_key_values);
//#endif

                // need to modify for ENSO??
                //rte_memcpy(packet->data + sizeof(struct mehcached_request) * (size_t)packet->num_requests, new_key_values, new_key_value_length);
                
                rte_memcpy(response_packet->data + sizeof(struct mehcached_request) * (size_t)packet->num_requests, new_key_values, new_key_value_length);

                // need to modify for ENSO
                // Cpy ether,ip,upd,request header rx->tx
                cpyHeaderRxToTx(&rxTxState, packet);
                // accumulate data for TX pipe
                setPacketToTxPipe(state, &rxTxState);
                rxTxState.pending_tx.current_tx_buffer = getNextPkt(rxTxState.pending_tx.current_tx_buffer);
                rxTxState.pending_tx.count++;
		        //mehcached_remote_send_response(state, mbuf, port_id);
                stage3_index++;
                //debug
                counter_d++;
            }
        }
        
        // after processing all packet send TX
        rte_eth_rx_enso_clear(ensoDevice);
        uint32_t tx_size = (rxTxState.pending_tx.current_tx_buffer - rxTxState.pending_tx.start_tx_buffer);
    
        if (tx_size > 0)
        {
            printf("======== Send %u packets, %u bytes to Tx pipe ========\n",rxTxState.pending_tx.count, tx_size);
            state->bytes_tx += tx_size;
            rte_eth_tx_enso_burst(ensoDevice, tx_size);
        }
            
        acc_p += rxTxState.pending_tx.count;

        rxTxState.pending_tx.count = 0;
        //fflush(stdout);

        if((counter_d > 0) && (counter_d % 512 == 0))
        {
            printf("------acc statistics------\n");
            printf("Acc packet %u\n", acc_p);
            printf("GET success %u\n", get_s);
            printf("GET failed %u\n", get_f);
            fflush(stdout);
        }

        t_end = mehcached_stopwatch_now();


    }

#endif
    return 0;
}




struct mehcached_diagnosis_arg
{
    uint8_t port_id;
    struct rte_mbuf *ret_mbuf;
};
static
int
mehcached_diagnosis_receive_packet_proc(void *arg)
{
    struct mehcached_diagnosis_arg *diagnosis_arg = (struct mehcached_diagnosis_arg *)arg;
    diagnosis_arg->ret_mbuf = mehcached_receive_packet(diagnosis_arg->port_id);
    return 0;
}

static
void
mehcached_diagnosis(struct mehcached_server_conf *server_conf)
{
    size_t noerror_count = 0;
    size_t error_count = 0;
    uint64_t t_start = mehcached_stopwatch_now();
    uint64_t t_end;

    while (!exiting)
    {
        // diagnosis for wrong mapping
        uint8_t port_id;
        uint8_t thread_id;
        for (port_id = 0; port_id < server_conf->num_ports; port_id++)
        {
            for (thread_id = 0; thread_id < server_conf->num_threads; thread_id++)
            {
                struct mehcached_diagnosis_arg diagnosis_arg;
                diagnosis_arg.port_id = port_id;
                rte_eal_remote_launch(mehcached_diagnosis_receive_packet_proc, &diagnosis_arg, (unsigned int)thread_id);
                rte_eal_mp_wait_lcore();

                struct rte_mbuf *mbuf = diagnosis_arg.ret_mbuf;
                if (mbuf == NULL)
                    continue;

                struct mehcached_batch_packet *packet = rte_pktmbuf_mtod(mbuf, struct mehcached_batch_packet *);

                struct rte_ether_hdr *eth = (struct rte_ether_hdr *)rte_pktmbuf_mtod(mbuf, unsigned char *);
                struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)((unsigned char *)eth + sizeof(struct rte_ether_hdr));
                struct rte_udp_hdr *udp = (struct rte_udp_hdr *)((unsigned char *)ip + sizeof(struct rte_ipv4_hdr));

                uint16_t mapping_id = rte_be_to_cpu_16(udp->dst_port);

                bool correct_port = true;
                bool correct_queue = true;
                if (mapping_id < 1024)
                {
                    uint8_t request_index;
                    for (request_index = 0; request_index < packet->num_requests; request_index++)
                    {
                        struct mehcached_request *req = (struct mehcached_request *)packet->data + request_index;

                        uint64_t key_hash = req->key_hash;
                        uint16_t partition_id = (uint16_t)(key_hash >> 48) & (uint16_t)(server_conf->num_partitions - 1);
                        uint8_t expected_thread_id = server_conf->partitions[partition_id].thread_id;

                        correct_queue = thread_id == expected_thread_id;

                        correct_port = false;
                        uint8_t port_index;
                        for (port_index = 0; port_index < server_conf->threads[expected_thread_id].num_ports; port_index++)
                        {
                            if (server_conf->threads[expected_thread_id].port_ids[port_index] == port_id)
                            {
                                correct_port = true;
                                break;
                            }
                        }
                    }
                }
                else
                {
                    // TODO: implement
                }

                if (error_count < 16)   // report up to 16 errors per period
                {
                    if (!correct_port)
                        printf("wrong port: mapping = %hu, port = %hhu; fdir.hash = %hu, fdir.id = %hu\n", mapping_id, port_id, mbuf->hash.fdir.hash, mbuf->hash.fdir.id);

                    if (!correct_queue)
                        printf("wrong queue: mapping = %hu, port = %hhu, queue = %hhu; fdir.hash = %hu, fdir.id = %hu\n", mapping_id, port_id, thread_id, mbuf->hash.fdir.hash, mbuf->hash.fdir.id);
                }

                if (correct_port && correct_queue)
                    noerror_count++;
                else
                    error_count++;

                mehcached_packet_free(mbuf);
            }
        }

        t_end = mehcached_stopwatch_now();
        if (mehcached_stopwatch_diff_in_s(t_end, t_start) >= 1.)
        {
            printf("noerror = %zu, error = %zu\n", noerror_count, error_count);
            t_start = t_end;
            noerror_count = 0;
            error_count = 0;
        }
    }
}

// this must be the same as that of netbench_client.c
static
uint64_t
mehcached_hash_key(uint64_t int_key)
{
    return hash((const uint8_t *)&int_key, 8);
}

static
int
mehcached_benchmark_prepopulate_proc(void *arg)
{
    struct server_state **states = (struct server_state **)arg;

    uint8_t thread_id = (uint8_t)rte_lcore_id();
    struct server_state *state = states[thread_id];
    struct mehcached_server_conf *server_conf = state->server_conf;
    struct mehcached_prepopulation_conf *prepopulation_conf = state->prepopulation_conf;
    unsigned int node_id = rte_lcore_to_socket_id(thread_id);

    printf("prepopulation: num_items %lu key_length %zu value_length %zu on core %hhu\n", prepopulation_conf->num_items, prepopulation_conf->key_length, prepopulation_conf->value_length, thread_id);
    
    //printf("[DEBUG] state pointer address: %p\n", (void*)state);
    //printf("[DEBUG] server_conf pointer address: %p\n", (void*)server_conf);
    //printf("[DEBUG] prepopulation_conf pointer address: %p\n", (void*)prepopulation_conf);


    if (prepopulation_conf->num_items == 0)
        return 0;

    // uint64_t t_start = mehcached_stopwatch_now();
    // uint64_t t_last_report = t_start;


    uint64_t key[MEHCACHED_ROUNDUP8(prepopulation_conf->key_length) / 8 + 1];
    uint64_t value[MEHCACHED_ROUNDUP8(prepopulation_conf->value_length) / 8 + 1];

    memset(key, 0, sizeof(key));
    memset(value, 0, sizeof(value));
    
    // printf("[DEBUG] Address of key right after declaration : %p\n", (void *)key);
    // printf("[DEBUG] key[0] : %lu\n", key[0]);
    // printf("[DEBUG] key[1] : %lu\n", key[1]);
    // printf("[DEBUG] key array size: %zu\n", sizeof(key));
    // printf("[DEBUG] key[0] size: %zu\n", sizeof(key[0]));
    // printf("[DEBUG] value array size: %zu\n", sizeof(value));

    size_t log16_num_items = 0;
    while (((size_t)1 << (log16_num_items * 4)) < (prepopulation_conf->num_items + 1))
        log16_num_items++;
    //printf("[DEBUG] log16_num_items: %zu\n", log16_num_items);
    
    size_t key_position_step = prepopulation_conf->key_length / log16_num_items;
    if (key_position_step == 0)
        key_position_step = 1;
    //printf("[DEBUG] key_pos_step: %lu\n", key_position_step);
    assert(key_position_step * log16_num_items <= prepopulation_conf->key_length);

    uint64_t key_index;
    uint64_t key_index_last_report = 0;
    for (key_index = 0; key_index < prepopulation_conf->num_items; key_index++)
    {
        /*
        if ((key_index & 0xff) == 0)
        {
            uint64_t t_end = mehcached_stopwatch_now();
            if (t_end - t_last_report >= mehcached_stopwatch_1_sec)
            {
                // double tput = (double)(key_index - key_index_last_report) / mehcached_stopwatch_diff_in_s(t_end, t_last_report);
                // printf("prepopulation: %3.2lf%% @ %2.2lf M items/sec\n", 100. * (double)key_index / (double)prepopulation_conf->num_items, tput / 1000000.);
                fflush(stdout);
                key_index_last_report = key_index;
                t_last_report += mehcached_stopwatch_1_sec;
            }
        }
        */

        uint64_t key_hash = mehcached_hash_key(key_index);
        uint16_t partition_id = mehcached_get_partition_id(state, key_hash);
        uint8_t owner_thread_id = server_conf->partitions[partition_id].thread_id;
        //printf("[DEBUG] Address of key: %p\n", (void *)key);

        //printf("[DEBUG] key_index: %llu, key_hash: %llu, partition_id: %u, owner_thread_id: %u\n",
        //        key_index, key_hash, partition_id, owner_thread_id);


        // if (owner_thread_id != thread_id)
        //     continue;
        // if (rte_lcore_to_socket_id(owner_thread_id) != node_id)
        //    continue;

        // from netbench_client.c

        // variable-length hexadecimal key
        // variable-length hexadecimal key
size_t key_length = 0;
{
    // Why using this loop?? Upper using memset(key, 0, sizeof(key))
    size_t i;
    for (i = 0; i < sizeof(key) / sizeof(key[0]); i++) {
        key[i] = 0;  // for keys that need zero paddings to an 8-byte boundary
    }

    // printf("[DEBUG] Address of key: %p\n", (void *)key);
    // printf("[DEBUG] key[0] : %lu\n", key[0]);
    // printf("[DEBUG] key[1] : %lu\n", key[1]);

    uint64_t key_index_copy = key_index + 1;
    // Debugging: Print initial key_index_copy value
    //printf("[DEBUG] Initial key_index_copy: %llu\n", key_index_copy);

    while (key_index_copy > 0) {
        key_index_copy >>= 4;
        key_length += key_position_step;
    }
    
    // Debugging: Print key_length after first loop
    //printf("[DEBUG] Calculated key_length: %zu\n", key_length);

    key_index_copy = key_index + 1;
    size_t char_index = key_length;
    while (key_index_copy > 0) {
        char_index -= key_position_step;
        ((char *)key)[char_index] = (char)(key_index_copy & 15);
        
        // Debugging: Print the intermediate values of key_index_copy and char_index
        //printf("[DEBUG] key_index_copy: %llu, char_index: %zu, key[char_index]: %d\n", key_index_copy, char_index, key[char_index]);

        key_index_copy >>= 4;
    }
}

// Debugging: Print the final key array
//printf("[DEBUG] Final key: ");
// for (size_t i = 0; i < key_length; i++) {
//     printf("%02x ", key[i]);
// }
// printf("\n");

// Debugging: Print the final value
*(uint64_t *)value = (key_index & 0xffffffff) | ((~key_index & 0xffffffff) << 32);
//printf("[DEBUG] Final value: 0x%llx\n", *(uint64_t *)value);

#ifdef NETBENCH_SERVER_MEHCACHED
        struct mehcached_table *partition = state->partitions[partition_id];
        //printf("[DEBUG] partition pointer address: %p\n", (void*)partition);
        uint8_t alloc_id;
        if (partition_id < server_conf->num_partitions)
            alloc_id = (uint8_t)((MEHCACHED_CONCURRENT_TABLE_WRITE(server_conf, partition_id) && !MEHCACHED_CONCURRENT_ALLOC_WRITE(server_conf, partition_id)) ? thread_id : 0);
        else
            alloc_id = 0;

        //printf("[DEBUG] alloc id %u\n", alloc_id);
        uint32_t expire_time = 0;
        bool overwrite = false;
        if (mehcached_set(alloc_id, partition, key_hash, (const uint8_t *)key, key_length, (const uint8_t *)value, prepopulation_conf->value_length, expire_time, overwrite))
#endif
#ifdef NETBENCH_SERVER_MEMCACHED
        if (process_update((char *)key, key_length, (char *)value, prepopulation_conf->value_length, false, true, false))
#endif
#ifdef NETBENCH_SERVER_MASSTREE
        if (masstree_put(state->masstree, thread_id, (const char *)key, key_length, (const char *)value, prepopulation_conf->value_length))
#endif
#ifdef NETBENCH_SERVER_RAMCLOUD
        if (ramcloud_put(state->ramcloud, thread_id, (const char *)key, key_length, (const char *)value, prepopulation_conf->value_length))
#endif
        {
            ;
        }
        else
        {
            //fprintf(stderr, "failed to insert key %lu on core %hhu\n", key_index, thread_id);
        }
    }
    // if (thread_id == 0)
    //     printf("\n");

    return 0;
}

static
void
mehcached_benchmark_server(int cpu_mode, int port_mode)
{
    //struct mehcached_server_conf *server_conf = mehcached_get_server_conf(machine_filename, server_name);
    struct mehcached_server_conf* server_conf = malloc(sizeof(struct mehcached_server_conf));
    memset(server_conf, 0, sizeof(struct mehcached_server_conf));
    make_server_config(server_conf);
    //struct mehcached_prepopulation_conf *prepopulation_conf = mehcached_get_prepopulation_conf(prepopulation_filename, server_name);
    struct mehcached_prepopulation_conf* prepopulation_conf = malloc(sizeof(struct mehcached_prepopulation_conf));
    memset(prepopulation_conf, 0, sizeof(struct mehcached_prepopulation_conf));
    make_prepopulation_conf(prepopulation_conf);


    // using num_items command line inpt
    // for (int i = 0; i < server_conf->num_partitions; i++)
    // {
    //     server_conf->partitions[i].num_items = 
    // }

    // mehcached_stopwatch_init_start();

    //not using NUMA
    #ifndef USE_NOT_SHM
    printf("initializing shm\n");
    const size_t page_size = 1048576 * 2;
    const size_t num_numa_nodes = 1;
    const size_t num_pages_to_try = 2048; //3072;// 4GB //16384;
    const size_t num_pages_to_reserve = 2048 - 1024;//16384 - 2048;	// give 2048 pages to dpdk

    mehcached_shm_init(page_size, num_numa_nodes, num_pages_to_try, num_pages_to_reserve);

    char memory_str[10];
    snprintf(memory_str, sizeof(memory_str), "%zu", (num_pages_to_try - num_pages_to_reserve) * 2);   // * 2 is because the used huge page size is 2 MB
    #else
    // preallocate hugepage num = 3072, 6GB
    // const size_t num_hugepage = 2048; 
    const size_t num_hugepage = 2048*2;
     // define hugepage 
    char memory_str[10];
    snprintf(memory_str, sizeof(memory_str), "%zu", (num_hugepage));   // * 2 is because the used huge page size is 2 MB
    #endif

    printf("initializing DPDK\n");
    printf("num_threads : %d \n", server_conf->num_threads);
    uint64_t cpu_mask = ((uint64_t)1 << server_conf->num_threads) - 1;
    char cpu_mask_str[10];
    snprintf(cpu_mask_str, sizeof(cpu_mask_str), "%lx", cpu_mask);

   
    
   char *rte_argv[] = {"",
        "-l", "0",
        "-n", "4",    // 4 for server 
        //"-m", memory_str,
        //"--log-level=pmd.tx,debug",
        //"--log-level=pmd.rx,debug",
        //"--log-level=ethdev,debug",
    };

    int rte_argc = sizeof(rte_argv) / sizeof(rte_argv[0]); 
    
    //rte_set_log_level(RTE_LOG_DEBUG);
    //rte_log_set_global_level(RTE_LOG_DEBUG);
    // change log stream stderr -> stdout
    rte_openlog_stream(stdout);

    int ret = rte_eal_init(rte_argc, rte_argv);
    if (ret < 0)
    {
        fprintf(stderr, "failed to initialize EAL\n");
        return;
    }

    

    uint8_t num_ports_max;
    uint64_t port_mask = ((size_t)1 << server_conf->num_ports) - 1;
    if (!mehcached_init_network(cpu_mask, port_mask, &num_ports_max))
    {
        fprintf(stderr, "failed to initialize network\n");
        return;
    }
    assert(server_conf->num_ports <= num_ports_max);

    // ToDo, using preallocated mac instead of server config mac addr
    printf("setting MAC address\n");
    uint8_t port_id;
    for (port_id = 0; port_id < server_conf->num_ports; port_id++)
    {
        struct rte_ether_addr mac_addr;
        /*
        memcpy(&mac_addr, server_conf->ports[port_id].mac_addr, sizeof(struct rte_ether_addr));
        if (rte_eth_dev_mac_addr_add(port_id, &mac_addr, 0) != 0)
        {
            fprintf(stderr, "failed to add a MAC address\n");
            return;
        }
        */
       if(rte_eth_dev_is_valid_port(port_id))
       {
            rte_eth_macaddr_get(port_id, &(mac_addr));
            printf("    MAC address for port #%d:\n", port_id);
            printf("        ");
            for (int j = 0; j < RTE_ETHER_ADDR_LEN; ++j) {
                printf("%02X:", mac_addr.addr_bytes[j]);
            }
            printf("\n");
       }
        
    }


printf("configuring mappings\n");

    uint16_t partition_id;
    uint8_t thread_id;
#ifdef USE_HOT_ITEMS
    uint8_t hot_item_id;
#endif

    /*
    for (port_id = 0; port_id < server_conf->num_ports; port_id++)
    {
        if (!mehcached_set_dst_port_mask(port_id, 0xffff))
            return;
    }
    */
    
    

    for (partition_id = 0; partition_id < server_conf->num_partitions; partition_id++)
    {
        server_conf->partitions[partition_id].thread_id %= (uint8_t)(server_conf->num_threads >> cpu_mode);
        uint8_t thread_id = server_conf->partitions[partition_id].thread_id;
        /*
        for (port_id = 0; port_id < server_conf->num_ports; port_id++)
            if (!mehcached_set_dst_port_mapping(port_id, (uint16_t)partition_id, thread_id % (uint8_t)(server_conf->num_threads >> cpu_mode)))
                return;
        */
        
    }
    /*
    for (thread_id = 0; thread_id < server_conf->num_threads; thread_id++)
    {
        for (port_id = 0; port_id < server_conf->num_ports; port_id++)
            if (!mehcached_set_dst_port_mapping(port_id, (uint16_t)(1024 + thread_id), thread_id % (uint8_t)(server_conf->num_threads >> cpu_mode)))
                return;
    }
    */
    

#ifdef USE_HOT_ITEMS
    for (hot_item_id = 0; hot_item_id < server_conf->num_hot_items; hot_item_id++)
    {
        server_conf->hot_items[hot_item_id].thread_id %= (uint8_t)(server_conf->num_threads >> cpu_mode);
        uint8_t thread_id = server_conf->hot_items[hot_item_id].thread_id;
        /*
        for (port_id = 0; port_id < server_conf->num_ports; port_id++)
            if (!mehcached_set_dst_port_mapping(port_id, (uint16_t)(2048 + hot_item_id), thread_id))
                return;
        
        */
                
    }
#endif

    printf("cleaning up pending packets\n");
    for (thread_id = 1; thread_id < server_conf->num_threads; thread_id++)
    {
	    rte_eal_launch(mehcached_benchmark_consume_packets_proc, (void *)(size_t)server_conf->num_ports, (unsigned int)thread_id);
    }
    rte_eal_launch(mehcached_benchmark_consume_packets_proc, (void *)(size_t)server_conf->num_ports, 0);

    rte_eal_mp_wait_lcore();


    for (port_id = 0; port_id < server_conf->num_ports; port_id++)
        rte_eth_stats_reset(port_id);


    printf("initializing server states\n");
    // not using shm
    size_t mem_start = mehcached_get_memuse();

    struct server_state *states[server_conf->num_threads];
#ifdef NETBENCH_SERVER_MEHCACHED
    struct mehcached_table *partitions[server_conf->num_partitions + server_conf->num_threads];
#endif

    for (thread_id = 0; thread_id < server_conf->num_threads; thread_id++)
    {
        // ToDo : server_state memory allocating in numa x -> using rte_malloc (allocate in hugepage area) 
        // using thread_id for specifying where thread is located in NUMA aware system
        // so, don't need if not using numa
        #ifndef USE_NOT_SHM
        struct server_state *state = mehcached_shm_malloc_contiguous(sizeof(struct server_state), thread_id);
        #else
        struct server_state *state = rte_zmalloc(NULL, sizeof(struct server_state), 0);
        #endif
        if (state == NULL)
            printf("[ERROR] Memory allocation for server_state failed!!\n");

        states[thread_id] = state;
        // rte_zmalloc -> initializing 0 when alloc mem
        //memset(state, 0, sizeof(struct server_state));

        state->server_conf = server_conf;
        state->prepopulation_conf = prepopulation_conf;
#ifdef NETBENCH_SERVER_MEHCACHED
        state->partitions = partitions;
#endif

        state->target_request_rate = 20000000;  // 20 Mops

#ifdef MEHCACHED_USE_SOFT_FDIR
        size_t mailbox_index;
        size_t ring_size = 256;
        // for (mailbox_index = 0; mailbox_index < NUM_THREAD; mailbox_index++)
        for (mailbox_index = 0; mailbox_index < MEHCACHED_MAX_NUMA_NODES; mailbox_index++)
        {
            char ring_name[64];
            snprintf(ring_name, sizeof(ring_name), "soft_fdir_mailbox_%hhu_%zu", thread_id, mailbox_index);
            // struct rte_ring *ring = rte_ring_create(ring_name, (unsigned int)ring_size, (int)rte_lcore_to_socket_id(thread_id), RING_F_SP_ENQ | RING_F_SC_DEQ);
            struct rte_ring *ring = rte_ring_create(ring_name, (unsigned int)ring_size, (int)mailbox_index, RING_F_SC_DEQ);
            if (ring == NULL)
            {
                fprintf(stderr, "failed to allocate soft_fdir mailbox %zu on thread %hhu\n", mailbox_index, thread_id);

            }
            state->soft_fdir_mailbox[mailbox_index] = ring;
        }
#endif
        state->cpu_mode = cpu_mode;
        state->port_mode = port_mode;

        printf("[DEBUG] no error for server state set up!! \n");
    }

#ifdef NETBENCH_SERVER_MEHCACHED
    for (partition_id = 0; partition_id < server_conf->num_partitions; partition_id++)
    {
        uint8_t thread_id = server_conf->partitions[partition_id].thread_id;

        //uint8_t num_allocs = (uint8_t)((MEHCACHED_CONCURRENT_TABLE_WRITE(server_conf, partition_id) && !MEHCACHED_CONCURRENT_ALLOC_WRITE(server_conf, partition_id)) ? (server_conf->num_threads >> 1) : 1);
        uint8_t num_allocs = (uint8_t)((MEHCACHED_CONCURRENT_TABLE_WRITE(server_conf, partition_id) && !MEHCACHED_CONCURRENT_ALLOC_WRITE(server_conf, partition_id)) ? server_conf->num_threads : 1);
        uint64_t num_items = server_conf->partitions[partition_id].num_items;
        uint64_t alloc_size = server_conf->partitions[partition_id].alloc_size;
        double mth_threshold = server_conf->partitions[partition_id].mth_threshold;

        // consider per-item overhead
        size_t alloc_overhead = sizeof(struct mehcached_item);
#ifdef MEHCACHED_ALLOC_DYNAMIC
        alloc_overhead += MEHCAHCED_DYNAMIC_OVERHEAD;
#endif
        alloc_size += alloc_overhead * num_items;

        if (num_allocs > 1)
        {
            alloc_size /= num_allocs;
            // we need much larger pools for concurrent_table_write && !concurrent_alloc_write because the server cannot perform in-place updates well (due to different alloc_id), which incurs less efficient use of logs
            alloc_size = alloc_size * 2;    // 100% larger size; comparable success rate for uniform requests (94.2% for CRCW vs 97.1% for EREW/CREW)
        }

        // use larger space to compensate space efficiency
        num_items = num_items * 12 / 10;
        alloc_size = alloc_size * 12 / 10;

        // for debugging
        // printf("[DEBUG] thread_id: %u\n", thread_id);
        // printf("[DEBUG] num_allocs: %u\n", num_allocs);
        // printf("[DEBUG] num_items: %lu\n", num_items);
        // printf("[DEBUG] alloc_size: %lu\n", alloc_size);
        // printf("[DEBUG] mth_threshold: %lf\n", mth_threshold);
        // printf("[DEBUG] alloc_overhead (sizeof(struct mehcached_item)): %zu\n", alloc_overhead);
        // printf("[DEBUG] num_items after increasing : %lu\n", num_items);
        // printf("[DEBUG] alloc_size after increasing : %lu\n", alloc_size);

        // ToDo : server_state memory allocating in numa x -> using rte_malloc (allocate in hugepage area)
        #ifndef USE_NOT_SHM 
        partitions[partition_id] = mehcached_shm_malloc_contiguous(sizeof(struct mehcached_table), thread_id);
        #else
        partitions[partition_id] = rte_zmalloc(NULL, sizeof(struct mehcached_table), 0);
        #endif
        if (partitions[partition_id] == NULL)
        {
            printf("[ERROR] Memory allocation for partition failed!!\n");
            exit(1);
        }
            

        size_t table_numa_node = rte_lcore_to_socket_id((unsigned int)thread_id); // always 0, because of not using numa
        size_t alloc_numa_nodes[num_allocs];
        uint8_t alloc_id;
        for (alloc_id = 0; alloc_id < num_allocs; alloc_id++)
            alloc_numa_nodes[alloc_id] = table_numa_node;
        //printf("[DEBUG] alloc numa node size %zu\n", sizeof(alloc_numa_nodes));
        //printf("[DEBUG] table numa node %zu\n", table_numa_node);

        bool concurrent_table_read = MEHCACHED_CONCURRENT_TABLE_READ(server_conf, partition_id);
        bool concurrent_table_write = MEHCACHED_CONCURRENT_TABLE_WRITE(server_conf, partition_id);
        bool concurrent_alloc_write = MEHCACHED_CONCURRENT_ALLOC_WRITE(server_conf, partition_id);
        
        if(!concurrent_table_read && !concurrent_table_write && !concurrent_alloc_write)
            printf("[SETUP] concurrent deactivate \n");

        mehcached_table_init(partitions[partition_id],
            (num_items + MEHCACHED_ITEMS_PER_BUCKET - 1) / MEHCACHED_ITEMS_PER_BUCKET,
            num_allocs,
            alloc_size,
            concurrent_table_read, concurrent_table_write, concurrent_alloc_write,
            table_numa_node, alloc_numa_nodes,
            mth_threshold);
    }
    for (thread_id = 0; thread_id < server_conf->num_threads; thread_id++)
    {
        uint16_t partition_id = (uint16_t)(server_conf->num_partitions + thread_id);

        uint8_t num_allocs = 1;
        uint64_t num_items = 256;
        uint64_t alloc_size = 2048576;


        // ToDo : server_state memory allocating in numa x -> using rte_malloc (allocate in hugepage area)
        
        #ifndef USE_NOT_SHM
        partitions[partition_id] = mehcached_shm_malloc_contiguous(sizeof(struct mehcached_table), thread_id);
        #else
        partitions[partition_id] = rte_zmalloc(NULL, sizeof(struct mehcached_table), 0);
        #endif
        if (partitions[partition_id] == NULL)
            printf("[ERROR] Memory allocation for partition failed!!\n");

        size_t table_numa_node = rte_lcore_to_socket_id((unsigned int)thread_id);
        size_t alloc_numa_nodes[num_allocs];
        uint8_t alloc_id;
        for (alloc_id = 0; alloc_id < num_allocs; alloc_id++)
            alloc_numa_nodes[alloc_id] = table_numa_node;

        // CREW for hot items
        bool concurrent_table_read = true;
        bool concurrent_table_write = false;
        bool concurrent_alloc_write = false;
        mehcached_table_init(partitions[partition_id],
            (num_items + MEHCACHED_ITEMS_PER_BUCKET - 1) / MEHCACHED_ITEMS_PER_BUCKET,
            num_allocs,
            alloc_size,
            concurrent_table_read, concurrent_table_write, concurrent_alloc_write,
            table_numa_node, alloc_numa_nodes,
            MEHCACHED_MTH_THRESHOLD_FIFO);
    }
#endif
#ifdef NETBENCH_SERVER_MEMCACHED
    {
        uint64_t total_num_items = 0;
        uint64_t total_alloc_size = 0;
        for (partition_id = 0; partition_id < server_conf->num_partitions; partition_id++)
        {
            total_num_items += server_conf->partitions[partition_id].num_items;
            total_alloc_size += server_conf->partitions[partition_id].alloc_size;
        }

        // memcached uses quite much more space if the average item size is large
        if (total_alloc_size / total_num_items >= 64)
            total_alloc_size = total_alloc_size * 13 / 10;

        // consider per-item overhead
        total_alloc_size += 88 * total_num_items;

        uint64_t t = server_conf->num_threads;
        char t_str[64];
        sprintf(t_str, "%lu", t);
        printf("t = %lu\n", t);

        uint64_t m = total_alloc_size / 1048576;
        char m_str[64];
        sprintf(m_str, "%lu", m);
        printf("m = %lu\n", m);

        uint64_t hashpower = 0;
        while (((uint64_t)1 << hashpower) < total_num_items)
            hashpower++;
        hashpower -= 2; // each bucket has 4 slots
        hashpower += 1; // avoid full hash table (memc3 will abort)
        char hashpower_str[64];
        sprintf(hashpower_str, "hashpower=%lu", hashpower);
        printf("hashpower = %lu\n", hashpower);

        const char *argv[] = {"(internal)", "-v", "-u", "root", "-L", "-C", "-m", m_str, "-o", hashpower_str};
        int argc = sizeof(argv) / sizeof(argv[0]);
        if (init(argc, (char **)argv) != 0)
            fprintf(stderr, "failed to initialize memcached\n");
    }
#endif
#ifdef NETBENCH_SERVER_MASSTREE
    {
#ifndef NETBENCH_SERVER_MASSTREE_P
        masstree_t t = masstree_init(server_conf->num_threads, false);
#else
        masstree_t t = masstree_init(server_conf->num_threads, true);
#endif
        for (thread_id = 0; thread_id < server_conf->num_threads; thread_id++)
            states[thread_id]->masstree = t;
    }
#endif
#ifdef NETBENCH_SERVER_RAMCLOUD
    {
        ramcloud_t t = ramcloud_init(server_conf->num_threads);
        for (thread_id = 0; thread_id < server_conf->num_threads; thread_id++)
            states[thread_id]->ramcloud = t;
    }
#endif

    // mehcached_stopwatch_init_end();

    printf("prepopulating servers\n");
    
    // for (thread_id = 1; thread_id < server_conf->num_threads; thread_id++)
    //     rte_eal_launch(mehcached_benchmark_prepopulate_proc, states, (unsigned int)thread_id);
    // rte_eal_launch(mehcached_benchmark_prepopulate_proc, states, 0);
    assert(rte_lcore_to_socket_id(0) == 0);
    assert(rte_lcore_to_socket_id(1) == 1);
    rte_eal_launch(mehcached_benchmark_prepopulate_proc, states, 0);
    rte_eal_mp_wait_lcore();
    if(server_conf->num_threads > 1)
    { 
    	rte_eal_launch(mehcached_benchmark_prepopulate_proc, states, 1);
    	rte_eal_mp_wait_lcore();
    }

    size_t mem_diff = mehcached_get_memuse() - mem_start;
    printf("memory:   %10.2lf MB\n", (double)mem_diff * 0.000001);


    printf("running servers\n");

    struct sigaction new_action;
    new_action.sa_handler = signal_handler;
    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = 0;
    sigaction(SIGINT, &new_action, NULL);
    sigaction(SIGTERM, &new_action, NULL);

    // print stat about dpdk malloc_heap
    // rte_malloc_dump_stats(stdout, NULL);
    // rte_malloc_dump_heaps(stdout);
    // fflush(stdout);

    // use this for diagnosis (the actual server will not be run)
    // mehcached_diagnosis(server_conf);
     /* If we are in simulation, take checkpoint here. */


    for (thread_id = 1; thread_id < server_conf->num_threads; thread_id++)
    {
	    if (!rte_lcore_is_enabled(thread_id)) continue;
	    rte_eal_launch(mehcached_benchmark_server_proc, states, (unsigned int)thread_id);
    }
    rte_eal_launch(mehcached_benchmark_server_proc, states, 0);

    rte_eal_mp_wait_lcore();


    // mehcached_free_network(port_mask);
    // free state & table because of not using shm
    // Only consider thread_id = 0 because 
    //free(states[0]);
    rte_free(states[0]);
    for (partition_id = 0; partition_id < server_conf->num_partitions; partition_id++)
    {
        //free(partitions[partition_id]);
        rte_free(partitions[partition_id]);
    }
        

    printf("finished\n");
}

int
main(int argc, const char *argv[])
{


#ifndef MEHCACHED_MEASURE_LATENCY
    if (argc < 3)
    {
        //printf("%s MACHINE-FILENAME SERVER-NAME CPU-MODE PORT-MODE PREPOPULATION-FILENAME\n", argv[0]);
        printf("%s CPU-MODE PORT-MODE \n", argv[0]);
	return EXIT_FAILURE;
    }
#else
    if (argc < 7)
    {
        printf("%s MACHINE-FILENAME SERVER-NAME CPU-MODE PORT-MODE PREPOPULATION-FILENAME TARGET-REQUEST-RATE\n", argv[0]);
        return EXIT_FAILURE;
    }
    target_request_rate_from_user = (uint32_t)atoi(argv[6]);
#endif


    //mehcached_benchmark_server(argv[1], argv[2], atoi(argv[3]), atoi(argv[4]), argv[5]);

    mehcached_benchmark_server(atoi(argv[1]), atoi(argv[2]));	
    return EXIT_SUCCESS;
}


