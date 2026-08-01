#if !defined(VFS_H)
#define VFS_H
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#include "vm.h"
#include "protocol.h"

struct parser_state {
    int current_cmd;

    char path_buf[128];
    int path_len;
    int waiting_for_flag;

    int tmp_fd;
    int tmp_count;

    int w_fd;
    int w_count;
    int w_pos;
    char w_buf[512];

    int l_fd;
    uint32_t l_offset_acc;
    int l_offset_pos;
    int l_whence;

    int result32_pending;
    uint32_t result32_val;
    int result32_pos;
};

void parser_init(struct parser_state *ps);

int allocate_guest_fd(struct vm_args* va, int host_fd);
int map_fd(struct vm_args* va, int guest_fd);
int guest_flags_to_host(int guest_flags);

void process_open(struct vm_args* va, struct parser_state* ps, uint8_t value);
void process_close(struct vm_args* va, struct parser_state* ps, uint8_t value);
void process_read(struct vm_args* va, struct parser_state* ps, uint8_t value);
void process_write(struct vm_args* va, struct parser_state* ps, uint8_t value);
void process_lseek(struct vm_args* va, struct parser_state* ps, uint8_t value);
#endif // VFS_H
