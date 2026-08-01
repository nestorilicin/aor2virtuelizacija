#include "console.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t console_mutex;
static int console_inited = 0;

int console_init(void) {
    if (console_inited)
        return 0;
    if (pthread_mutex_init(&console_mutex, NULL) != 0) {
        perror("pthread_mutex_init(console_mutex)");
        return -1;
    }
    console_inited = 1;
    return 0;
}

void console_destroy(void) {
    if (!console_inited)
        return;
    pthread_mutex_destroy(&console_mutex);
    console_inited = 0;
}

static void console_print_atomic(const char* buf, size_t len) {
    if (len == 0)
        return;
    pthread_mutex_lock(&console_mutex);
    /* write in one go to stdout */
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
    pthread_mutex_unlock(&console_mutex);
}

void console_write_char(struct vm_args* va, char c) {
    if (!va)
        return;

    int pos = va->console_buf_pos;
    if (pos < CONSOLE_BUF_SIZE - 1) {
        va->console_buf[pos++] = c;
        va->console_buf[pos] = '\0';
        va->console_buf_pos = pos;
    } else {
        console_print_atomic(va->console_buf, pos);
        va->console_buf_pos = 0;
        if (c != '\n') {
            va->console_buf[0] = c;
            va->console_buf[1] = '\0';
            va->console_buf_pos = 1;
        } else {
            console_print_atomic("\n", 1);
        }
        return;
    }

    if (c == '\n') {
        console_print_atomic(va->console_buf, va->console_buf_pos);
        va->console_buf_pos = 0;
        va->console_buf[0] = '\0';
    }
}

void console_flush(struct vm_args* va) {
    if (!va)
        return;
    int pos = va->console_buf_pos;
    if (pos > 0) {
        console_print_atomic(va->console_buf, pos);
        va->console_buf_pos = 0;
        va->console_buf[0] = '\0';
    }
}
