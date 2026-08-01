#if !defined(VFS_H)
#define VFS_H
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "protocol.h"
#include "vm.h"

void parser_init(struct parser_state* ps);

int allocate_guest_fd(struct vm_args* va, int host_fd);
int map_fd(struct vm_args* va, int guest_fd);
int guest_flags_to_host(int guest_flags);

void process_open(struct vm_args* va, struct parser_state* ps, uint8_t value);
void process_close(struct vm_args* va, struct parser_state* ps, uint8_t value);
void process_read(struct vm_args* va, struct parser_state* ps, uint8_t value);
void process_write(struct vm_args* va, struct parser_state* ps, uint8_t value);
void process_lseek(struct vm_args* va, struct parser_state* ps, uint8_t value);
#endif // VFS_H
