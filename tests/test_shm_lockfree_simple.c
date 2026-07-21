#include "xlink.h"
#include "shm_ipc.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    printf("START\n");
    fflush(stdout);
    
    shm_destroy("test_lfq_mini");
    
    xlink_opt_t opt = { .flags = XLINK_CREATE };
    xlink_channel_t *tx = xlink_open(XLINK_SHM, "test_lfq_mini", &opt);
    printf("tx=%p\n", (void*)tx);
    if (!tx) return 1;
    
    printf("lfq_init=%d\n", xlink_lfq_init(tx, 64));
    
    xlink_msg_t m = { (void*)"hi", 2 };
    printf("send_batch=%d\n", xlink_send_batch(tx, &m, 1));
    printf("lfq_count=%zu\n", xlink_lfq_count(tx));
    
    xlink_channel_t *rx = xlink_open(XLINK_SHM, "test_lfq_mini", NULL);
    printf("rx=%p\n", (void*)rx);
    
    printf("lfq_flush=%d\n", xlink_lfq_flush(tx));
    
    char buf[8];
    size_t sz = sizeof(buf);
    printf("recv=%d\n", xlink_recv(rx, buf, &sz));
    
    xlink_close(rx);
    xlink_close(tx);
    printf("DONE\n");
    return 0;
}
