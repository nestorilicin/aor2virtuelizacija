#include "shared.h"
#include <string.h>

struct shared_state g_shared;

int shared_init(int total_readers){
    memset(&g_shared, 0, sizeof(g_shared));
    if(pthread_mutex_init(&g_shared.mutex, NULL) != 0)
        return -1;
    if(pthread_cond_init(&g_shared.cond_data, NULL) != 0)
        return -1;
    if(pthread_cond_init(&g_shared.cond_done, NULL) != 0)
        return -1;
    g_shared.total_readers = total_readers;
    g_shared.data_ready = 0;
    g_shared.readers_done = 0;
    g_shared.buf_len = 0;
    g_shared.stop_signal = STOP_SIGNAL_NONE;
    return 0;
}   

void shared_destroy(void){
    pthread_cond_destroy(&g_shared.cond_done);
    pthread_cond_destroy(&g_shared.cond_data);
    pthread_mutex_destroy(&g_shared.mutex);
}