#if !defined(SHARED_H)
#define SHARED_H

#include <pthread.h>
#include <stdint.h>

#define BUFFER_SIZE 256
#define VM_ROLE_READER 0
#define VM_ROLE_WRITER 1

struct shared_state {
    pthread_mutex_t mutex;
    pthread_cond_t cond_data;
    pthread_cond_t cond_done;

    uint8_t buf[BUFFER_SIZE];
    uint32_t buf_len;

    int data_ready;
    int total_readers;
    int readers_done;

    uint32_t seq;

};

extern struct shared_state g_shared;

int shared_init(int total_readers);
void shared_destroy(void);



#endif // SHARED_H
