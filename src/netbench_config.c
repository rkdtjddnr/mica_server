#include "netbench_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stddef.h>


const char* server_name = "server";



uint8_t get_concurrent_table_read(enum ConcurrencyType type) {
    return (type == CREW || type == CRCW || type == CRCWS) ? 1 : 0;
}

uint8_t get_concurrent_table_write(enum ConcurrencyType type) {
    return (type == CRCW || type == CRCWS) ? 1 : 0;
}

uint8_t get_concurrent_alloc_write(enum ConcurrencyType type) {
    return (type == CRCWS) ? 1 : 0;
}

uint8_t get_thread_id(enum ConcurrencyType type, int partition_id) {
    if (type == CREW0) return 0;
    return partition_id % 16;  // 기본은 mod 16
}

void fill_partitions(struct mehcached_server_conf *conf,
                     uint16_t num_partitions,
                     uint64_t total_items,
                     uint64_t total_alloc_size,
                     double mth_threshold,
                     enum ConcurrencyType concurrency)
{
    for (uint16_t i = 0; i < num_partitions; i++) {
        struct mehcached_server_partition_conf *p = &conf->partitions[i];

        p->num_items = total_items / num_partitions;
        p->alloc_size = total_alloc_size / num_partitions;
        p->concurrent_table_read = get_concurrent_table_read(concurrency);
        p->concurrent_table_write = get_concurrent_table_write(concurrency);
        p->concurrent_alloc_write = get_concurrent_alloc_write(concurrency);
        p->thread_id = get_thread_id(concurrency, i);
        p->mth_threshold = mth_threshold;

        conf->num_partitions++;
	assert(conf->num_partitions <= NUM_PART);
    }
} 




void make_server_config(struct mehcached_server_conf* s_conf)
{

    uint8_t server_mac[NUM_PORT][MAC_SZ] = {
        { 0xde, 0xad, 0x1e, 0x00, 0x00, 0x01 },
        //{ 0xde, 0xad, 0x1e, 0x00, 0x00, 0x02 }
    };

    uint8_t server_ip[NUM_PORT][IP_SZ] = {
        { 10, 0, 2, 1 },
        //{ 10, 0, 2, 2 }
    };

    size_t i, j;
    printf("[CONFIG] making server port conf \n");
    for(i = 0; i < NUM_PORT; i++)
    {
        // memcpy(s_conf->ports[i].mac_addr, server_mac[i], MAC_SZ);
        // memcpy(s_conf->ports[i].ip_addr, server_ip[i], IP_SZ);
        for(j = 0; j < MAC_SZ; j++)
            s_conf->ports[i].mac_addr[j] = server_mac[i][j];
        for (j = 0; j < IP_SZ; j++)
            s_conf->ports[i].ip_addr[j] = server_ip[i][j];    
        s_conf->num_ports++;
        assert(s_conf->num_ports <= NUM_PORT);
    }


    printf("[CONFIG] making server thread conf \n");
    for(i = 0; i < NUM_THREAD; i++)
    {   
        for(j = 0; j < PER_TH_PORT; j++)
        {
            s_conf->threads[i].port_ids[j] = j;
            s_conf->threads[i].num_ports++;
			assert(s_conf->threads[i].num_ports <= NUM_PORT);
        }
        s_conf->num_threads++;
		assert(s_conf->num_threads <= NUM_THREAD);
    }

    

    printf("[CONFIG] making server partition conf \n");
    uint64_t total_alloc_size = NUM_ITEMS * (KEY_LEN + VAL_LEN);  // key_len + val_len
    double mth_threshold = 1.0;
    enum ConcurrencyType concurrency = EREW;

    fill_partitions(s_conf, NUM_PART, NUM_ITEMS, total_alloc_size, mth_threshold, concurrency);

    #ifdef USE_HOT_ITEMS
    // To Do ...
    #endif

}



void make_client_config(struct mehcached_client_conf* c_conf)
{
    

    uint8_t client_mac[NUM_PORT][MAC_SZ] = {
        { 0xde, 0xad, 0x1e, 0x00, 0x00, 0x04 },
        //{ 0xde, 0xad, 0x1e, 0x00, 0x00, 0x05 }
    };

    uint8_t client_ip[NUM_PORT][IP_SZ] = {
        { 10, 0, 2, 4 },
        //{ 10, 0, 2, 5 }
    };
    
    size_t i;
    printf("[CONFIG] making client port conf \n");
    for(i = 0; i < NUM_PORT; i++)
    {
        memcpy(c_conf->ports[c_conf->num_ports].mac_addr, client_mac[i], MAC_SZ);
        memcpy(c_conf->ports[c_conf->num_ports].ip_addr, client_ip[i], IP_SZ);
        c_conf->num_ports++;
        assert(c_conf->num_ports <= NUM_PORT);
    }

    printf("[CONFIG] making client thread conf \n");
    for(i = 0; i < NUM_THREAD; i++)
    {
        c_conf->num_threads++;
        assert(c_conf->num_threads <= NUM_THREAD);
    }

}


void make_prepopulation_conf(struct mehcached_prepopulation_conf* p_conf)
{
    

    printf("[CONFIG] making prepopulation conf \n");
    p_conf->num_items = NUM_PREPOP;
    p_conf->key_length = KEY_LEN;
    p_conf->value_length = VAL_LEN;

}   



void make_workload_conf(struct mehcached_workload_conf* w_conf)
{
    

    printf("[CONFIG] making workload conf \n");
    size_t i,j;

    for(i = 0; i < NUM_THREAD; i++)
    {
        for(j = 0; j < PER_TH_PORT; j++)
        {
            w_conf->threads[w_conf->num_threads].port_ids[w_conf->threads[w_conf->num_threads].num_ports] = j;
            w_conf->threads[w_conf->num_threads].num_ports++;
			assert(w_conf->threads[w_conf->num_threads].num_ports <= NUM_PORT);
        }
        strcpy(w_conf->threads[w_conf->num_threads].server_name, server_name);
        w_conf->threads[w_conf->num_threads].partition_mode = PARTITION_MODE;
        w_conf->threads[w_conf->num_threads].num_items = NUM_ITEMS;
        w_conf->threads[w_conf->num_threads].key_length = KEY_LEN;
        w_conf->threads[w_conf->num_threads].value_length = VAL_LEN;
        w_conf->threads[w_conf->num_threads].zipf_theta = ZIPF;
        w_conf->threads[w_conf->num_threads].get_ratio = GET_RATIO;
        w_conf->threads[w_conf->num_threads].put_ratio = PUT_RATIO;
        w_conf->threads[w_conf->num_threads].increment_ratio = INC_RATIO;
        w_conf->threads[w_conf->num_threads].batch_size = BATCH_SZ;
        w_conf->threads[w_conf->num_threads].num_operations = NUM_OP;
        w_conf->threads[w_conf->num_threads].duration = DURATION;
        w_conf->num_threads++;
        assert(w_conf->num_threads <= NUM_THREAD);
    }

}

