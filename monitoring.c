#include <stdio.h>
#include <stdlib.h>
#define VALUE_TYPE U32

#include "./utils/common.h"
#include "./utils/pkt-ops.h"
#include "./utils/common.h"
#include "./utils/dleft-hash.h"
#include "./utils/pkt-puller.h"

#define HT_SIZE (0.5 * 1024 * 1024)
static dleft_meta_t ht_meta;

int main(){

    if(-1 == dleft_init("monitoring", HT_SIZE, &ht_meta))
    {
        printf("bootmemory allocation error\n");
        return 0;
    }

    srand((unsigned)time(NULL));
   
    load_pkt("/users/yangzhou/ictf2010_1Mflow.dat");
    uint32_t pkt_cnt = 0;
    while(1){
        pkt_t *raw_pkt = next_pkt();
        uint8_t *pkt_ptr = raw_pkt->content;
        uint16_t pkt_len = raw_pkt->len;
        swap_mac_addr(pkt_ptr);

        five_tuple_t flow;
        get_five_tuple(pkt_ptr, &flow);
        dleft_add_value(&ht_meta, flow, 1);

        pkt_cnt ++;
        if(pkt_cnt % (1024 * 1024 / 64) == 0) {
            printf("%s packets processed: %u\n", "monitoring", pkt_cnt);
        }
    }    
    dleft_destroy(&ht_meta);

    return 0;
}