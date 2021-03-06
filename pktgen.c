#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <pthread.h> 
#include <signal.h>
#include <sys/time.h> 
#include <unistd.h>

#include "./utils/common.h"
#include "./utils/pkt-ops.h"
#include "./utils/pkt-header.h"
// #include "./utils/pkt-puller.h" // zipf with on-flying seq gen; take small amount of memory (100k distinct packets)
// #include "./utils/pkt-puller2.h" // preloaded normal or zipf traffic; take a lot of memory (2m packets)
#include "./utils/pkt-puller3.h" // zipf with preloaded seq, a bit faster than the first; modest memory (100k distinct packets, and 2m seq number)
#include "./utils/raw_socket.h"
#include "./utils/parsing_mac.h"

#define MAX_WAITING_CYCLES 2999538 // this is from empirical tests 

static volatile uint64_t unack_pkts[4] = {0,0,0,0};
static volatile uint64_t sent_pkts[4] = {0,0,0,0};
static volatile uint64_t received_pkts[4] = {0,0,0,0};

static volatile uint8_t force_quit_send[4];
static volatile uint8_t force_quit_recv[4];

static uint8_t dstmac[6];
__thread int sockfd  = 0;
__thread struct sockaddr_ll send_sockaddr;
__thread struct ifreq if_mac;

static volatile uint8_t print_order = 0;
static volatile uint32_t finished_nfs = 0;
int num_nfs = 1;

void *send_pkt_func(void *arg) {
	// which nf's traffic this thread sends
    int nf_idx = *(int*)arg;
    set_affinity(nf_idx);
    init_socket(&sockfd, &send_sockaddr, &if_mac, dstmac, nf_idx);
        
    struct ether_hdr* eh;
    for(int i = 0; i < PKT_NUM; i++){
        eh = (struct ether_hdr*) pkts[i].content;
        /* Ethernet header */
        eh->s_addr.addr_bytes[0] = ((uint8_t *)&if_mac.ifr_hwaddr.sa_data)[0];
    	eh->s_addr.addr_bytes[1] = ((uint8_t *)&if_mac.ifr_hwaddr.sa_data)[1];
    	eh->s_addr.addr_bytes[2] = ((uint8_t *)&if_mac.ifr_hwaddr.sa_data)[2];
    	eh->s_addr.addr_bytes[3] = ((uint8_t *)&if_mac.ifr_hwaddr.sa_data)[3];
    	eh->s_addr.addr_bytes[4] = ((uint8_t *)&if_mac.ifr_hwaddr.sa_data)[4];
    	eh->s_addr.addr_bytes[5] = ((uint8_t *)&if_mac.ifr_hwaddr.sa_data)[5];
    	eh->d_addr.addr_bytes[0] = dstmac[0];
    	eh->d_addr.addr_bytes[1] = dstmac[1];
    	eh->d_addr.addr_bytes[2] = dstmac[2];
    	eh->d_addr.addr_bytes[3] = dstmac[3];
    	eh->d_addr.addr_bytes[4] = dstmac[4];
    	eh->d_addr.addr_bytes[5] = dstmac[5];
    	/* Ethertype field */
    	eh->ether_type = htons(ETH_P_IP);
    }
    pkt_t* pkt_buf[MAX_BATCH_SIZE];

    struct timeval start, end; 
    uint64_t burst_size;
    struct tcp_hdr *tcph;
    uint8_t warmup_end = 0;
    gettimeofday(&start, NULL);

    // uint32_t max_waiting_cycles = 0;
    uint32_t waiting_cycles = 0;
    while(sent_pkts[nf_idx] < TEST_NPKTS*2 + WARMUP_NPKTS){
        while(sent_pkts[nf_idx] >= received_pkts[nf_idx] && (unack_pkts[nf_idx] = sent_pkts[nf_idx] - received_pkts[nf_idx]) >= MAX_UNACK_WINDOW){
            // ad-hoc solution to break deadlock caused by packet loss -- should rarely happen
            waiting_cycles ++;
            if(waiting_cycles > MAX_WAITING_CYCLES){
                sent_pkts[nf_idx] = received_pkts[nf_idx];
                printf("[send_pacekts th%d]: deadlock detected (caused by packet loss or NF initing), forcely resolving...\n", nf_idx);
            }
            if(force_quit_send[nf_idx])
				break;
        }
        // max_waiting_cycles = MAX(max_waiting_cycles, waiting_cycles);
        waiting_cycles = 0;
        barrier();

        burst_size = MAX_UNACK_WINDOW - unack_pkts[nf_idx];
        burst_size = MIN(burst_size, TEST_NPKTS*2 + WARMUP_NPKTS - sent_pkts[nf_idx]);

        for(int i = 0; i < burst_size; i++){
            pkt_buf[i] = next_pkt(nf_idx);
        	
            eh = (struct ether_hdr*)pkt_buf[i]->content;
            eh->ether_type = htons((uint16_t)nf_idx + CUSTOM_PROTO_BASE);

            tcph = (struct tcp_hdr *) (pkt_buf[i]->content + sizeof(struct ipv4_hdr) + sizeof(struct ether_hdr));
            tcph->sent_seq = 0xdeadbeef;
            tcph->recv_ack = sent_pkts[nf_idx] + i;
        }

        sendto_batch(sockfd, burst_size, pkt_buf, &send_sockaddr);

        __atomic_fetch_add(&sent_pkts[nf_idx], burst_size, __ATOMIC_SEQ_CST);
        // sent_pkts[nf_idx] += burst_size;

        if(!warmup_end && sent_pkts[nf_idx] >= WARMUP_NPKTS){
            gettimeofday(&start, NULL);
            warmup_end = 1; 
        }
        if((sent_pkts[nf_idx] / MAX_BATCH_SIZE) % (PRINT_INTERVAL / MAX_BATCH_SIZE) == 0){
            printf("[send_pacekts th%d] number of pkts     sent: %llu\n", nf_idx, sent_pkts[nf_idx]);
        }

       if(force_quit_send[nf_idx])
			break;
    }
    gettimeofday(&end, NULL); 
    if(warmup_end){
        // __atomic_fetch_add(&sent_pkts[nf_idx], -WARMUP_NPKTS, __ATOMIC_SEQ_CST);
        sent_pkts[nf_idx] -= WARMUP_NPKTS;
    }
    double time_taken; 
    time_taken = (end.tv_sec - start.tv_sec) * 1e6; 
    time_taken = (time_taken + (end.tv_usec - start.tv_usec)) * 1e-6;

	close(sockfd);
    // commenting force_quit, so that we let each NF process the same number of packets. 
    // ending all packet sending.
    force_quit_send[nf_idx] = 1;
    // ending all packet receiving. 
    force_quit_recv[nf_idx] = 1;

    __atomic_fetch_add(&finished_nfs, 1, __ATOMIC_SEQ_CST);

    while(print_order != nf_idx || finished_nfs != num_nfs){
        sleep(1);
    }
    printf("[send_pacekts th%d]: %llu pkt sent, %.8lf Mpps\n", nf_idx, sent_pkts[nf_idx], (double)(sent_pkts[nf_idx]) * 1e-6 / time_taken);
    // printf("max_waiting_cycles = %u\n", max_waiting_cycles);
    barrier();
    print_order = nf_idx + 1;
   
    while(print_order != num_nfs){
        sleep(1);
    }
    exit(0);
    // any of the following two will crash. ?? why
    // pthread_exit(arg);
    // return NULL;
    // pthread_exit(NULL);
}

static volatile uint64_t invalid_pkts = 0;
void * recv_pkt_func(void *arg){
    // which nf's traffic this thread sends
    int nf_idx = *(int*)arg;
    set_affinity(nf_idx + num_nfs);
    // set_affinity(nf_idx);
    init_socket(&sockfd, &send_sockaddr, &if_mac, dstmac, nf_idx);

    int recv_nf_idx = 0, numpkts = 0;
    struct ether_hdr *eh;
    struct tcp_hdr *tcph;

    pkt_t* pkt_buf[MAX_BATCH_SIZE];
    for(int i = 0; i < MAX_BATCH_SIZE; i++){
        pkt_buf[i] = malloc(sizeof(pkt_t));
        pkt_buf[i]->content = malloc(BUF_SIZ);
    }
    uint8_t warmup_end = 0;
    uint64_t lost_pkt_during_cold_start;
    uint64_t curr_invalid_pkts = 0;
	while(!force_quit_recv[nf_idx]){
        numpkts = recvfrom_batch(sockfd, BUF_SIZ, pkt_buf);
        if(numpkts <= 0){
            continue;
        }
        curr_invalid_pkts = 0;
        for(int i = 0; i < numpkts; i++){
        	eh = (struct ether_hdr *) pkt_buf[i]->content;
        	tcph = (struct tcp_hdr *) (pkt_buf[i]->content + sizeof(struct ipv4_hdr) + sizeof(struct ether_hdr));
            recv_nf_idx = (int)htons((eh->ether_type)) - CUSTOM_PROTO_BASE;

            // printf("[recv_pacekts %d] recv_nf_idx = %d, tcph->sent_seq = %x\n", nf_idx, recv_nf_idx, tcph->sent_seq);

            if(tcph->sent_seq != 0xdeadbeef){
                curr_invalid_pkts ++;
                __atomic_fetch_add(&invalid_pkts, 1, __ATOMIC_SEQ_CST);
                continue;
            }
            if(recv_nf_idx < 4)
                __atomic_fetch_add(&received_pkts[recv_nf_idx], 1, __ATOMIC_SEQ_CST);
            barrier();

            if(nf_idx == recv_nf_idx){
                uint32_t pkt_idx = tcph->recv_ack;
                uint64_t curr_received_pkts = received_pkts[nf_idx];
                uint32_t lost_pkts = pkt_idx + 1 - curr_received_pkts;

                // TODO: the packets might get re-ordered. 
                // printf("%lu, %llu\n", pkt_idx, curr_received_pkts);
                
                // these packets are lost
                // if(lost_pkts != 0){
                //     __atomic_fetch_add(&received_pkts[nf_idx], lost_pkts, __ATOMIC_SEQ_CST);
                // }

                if(!warmup_end && pkt_idx >= WARMUP_NPKTS - 1)
                {
            		warmup_end = 1;
                    lost_pkt_during_cold_start = lost_pkts; 
                    printf("[recv_pacekts th%d] warm up ends, lost_pkt_during_cold_start = %llu\n", nf_idx, lost_pkt_during_cold_start);
            		printf("[recv_pacekts th%d] pkt_idx %u, received_pkts[nf_idx] %llu\n", nf_idx, pkt_idx, curr_received_pkts);
                }
                if(curr_received_pkts % PRINT_INTERVAL == 0){

                    printf("[recv_pacekts th%d] number of pkts received: %llu\n", nf_idx, curr_received_pkts);
                }
                if(tcph->recv_ack == 0xFFFFFFFF){
                    goto finished;
                }
            }
        }
    }
finished:
	close(sockfd);
    force_quit_send[nf_idx] = 1;
    force_quit_recv[nf_idx] = 1;

    for(int i = 0; i < MAX_BATCH_SIZE; i++){
        free(pkt_buf[i]->content);
        free(pkt_buf[i]);
    }

    while(print_order != num_nfs){
        sleep(1);
    }
    exit(0);
    // pthread_exit(NULL);
	// return NULL;
}


static void signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		printf("\n\nSignal %d received, preparing to exit...\n",
				signum);
        for(int i = 0; i < 4; i++){
    		force_quit_send[i] = 1;
            force_quit_recv[i] = 1;
        }
	}
}

int main(int argc, char *argv[]){
    signal(SIGINT, signal_handler);
    for(int i = 0; i < 4; i++){
		force_quit_send[i] = 0;
        force_quit_recv[i] = 0;
    }

    // for pkt_puller2;
    // char * tracepath = "./data/ictf2010_2mpacket.dat";
    // char * tracepath = "./data/ictf2010_2mpacket_zipf.dat"
    // for pkt_puller and 3;
    char * tracepath = "./data/ictf2010_100kflow.dat";
    
    int option;
    while((option = getopt(argc, argv, ":n:t:d:")) != -1){
        switch (option) {
            case 'n':
                num_nfs = atoi(optarg);
                printf("number of NFs: %d\n", num_nfs);
                break;
            case 't':
                tracepath = optarg;
                printf("trace path: %s\n", tracepath);
                break;
            case 'd':
                parse_mac(dstmac, optarg);
                printf("dstmac: %02x:%02x:%02x:%02x:%02x:%02x\n", dstmac[0], dstmac[1], dstmac[2], dstmac[3], dstmac[4], dstmac[5]);
                break;
             case ':':  
                printf("option -%c needs a value\n", optopt);  
                break;  
            case '?':  
                printf("unknown option: -%c\n", optopt); 
                break; 
        }
    }

    load_pkt(tracepath);

    pthread_t threads[14];
    int nf_idx[7] = {0, 1, 2, 3, 4, 5, 6};
    for(int i = 0; i < num_nfs*2; i += 2){
        pthread_create(&threads[i], NULL, send_pkt_func, (void*)(nf_idx+i/2));
        pthread_create(&threads[i+1], NULL, recv_pkt_func, (void*)(nf_idx+i/2));
    }
    
    for(int i = 0; i < num_nfs*2; i += 2){
        pthread_join(threads[i], NULL);
        pthread_join(threads[i+1], NULL);
    }    

    return 0;
}
    