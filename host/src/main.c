#include "vm.h"

#include <fcntl.h>
#include <linux/kvm.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_GUESTS 10

static int allocate_guest_fd(struct vm_args* va, int host_fd) {
    if (va->next_guest_fd >= MAX_FDS)
        return -1;
    int guest_fd = va->next_guest_fd++;
    va->guest_fd_to_host_fd[guest_fd] = host_fd;
    return guest_fd;
}

static int map_fd(struct vm_args* va, int guest_fd) {
    if (guest_fd < 0 || guest_fd >= MAX_FDS)
        return -1;
    return va->guest_fd_to_host_fd[guest_fd];
}

static int guest_flags_to_host(int guest_flags) {
    int host_flags = 0;
    if (guest_flags & 4)
        host_flags |= O_RDWR;
    else if (guest_flags & 2)
        host_flags |= O_WRONLY;
    else
        host_flags |= O_RDONLY;
    if (guest_flags & 8)
        host_flags |= O_CREAT;
    return host_flags;
}

struct parser_state {
    int current_cmd;
    char path_buf[128];
    int path_len;
    int waiting_for_flag;
    int tmp_fd;
    int tmp_count;
    /* write state */
    int w_fd;
    int w_count;
    int w_pos;
    char w_buf[512];
    /* lseek state */
    int l_fd;
    uint32_t l_offset_acc;
    int l_offset_pos;
    int l_whence;
    /* pending 32-bit result (for lseek or other ops) */
    int result32_pending;
    uint32_t result32_val;
    int result32_pos;
};

static void process_open(struct vm_args* va, struct parser_state* ps, uint8_t value) {
    if (!ps->waiting_for_flag) {
        if (value != 0) {
            if (ps->path_len < (int)sizeof(ps->path_buf) - 1) {
                ps->path_buf[ps->path_len++] = (char)value;
            }
        } else {
            ps->path_buf[ps->path_len] = '\0';
            ps->waiting_for_flag = 1;
        }
    } else {
        int guest_flags = (int)value;
        int host_flags = guest_flags_to_host(guest_flags);
        int host_fd;
        if (host_flags & O_CREAT)
            host_fd = open(ps->path_buf, host_flags, 0644);
        else
            host_fd = open(ps->path_buf, host_flags);
        int guest_fd = (host_fd >= 0) ? allocate_guest_fd(va, host_fd) : -1;
        va->last_result = guest_fd;
        /* reset parser state */
        ps->current_cmd = 0;
        ps->path_len = 0;
        ps->waiting_for_flag = 0;
    }
}

static void process_close(struct vm_args* va, struct parser_state* ps, uint8_t value) {
    int guest_fd = (int)value;
    int host_fd = map_fd(va, guest_fd);
    int res = -1;
    if (host_fd >= 0)
        res = close(host_fd);
    if (guest_fd >= 0 && guest_fd < MAX_FDS)
        va->guest_fd_to_host_fd[guest_fd] = -1;
    va->last_result = (res == 0) ? 0 : -1;
    ps->current_cmd = 0;
}

static void process_read(struct vm_args* va, struct parser_state* ps, uint8_t value) {
    if (ps->tmp_fd == -1) {
        ps->tmp_fd = (int)value;
    } else {
        ps->tmp_count = (int)value;
        int host_fd = map_fd(va, ps->tmp_fd);
        if (host_fd < 0) {
            va->last_result = -1;
            va->read_buffer_len = 0;
            va->read_buffer_pos = 0;
            va->read_pending = 0;
        } else {
            int toread = ps->tmp_count;
            if (toread > (int)sizeof(va->read_buffer))
                toread = (int)sizeof(va->read_buffer);
            int n = read(host_fd, va->read_buffer, toread);
            va->read_buffer_len = (n > 0) ? n : 0;
            va->read_buffer_pos = 0;
            va->last_result = n;
            va->read_pending = 1; /* next IN returns n, then data */
        }
        /* reset */
        ps->tmp_fd = -1;
        ps->tmp_count = 0;
        ps->current_cmd = 0;
    }
}

static void process_write(struct vm_args* va, struct parser_state* ps, uint8_t value) {
    if (ps->w_fd == -1) {
        ps->w_fd = (int)value;
    } else if (ps->w_count == 0) {
        ps->w_count = (int)value;
        if (ps->w_count < 0)
            ps->w_count = 0;
        if (ps->w_count > (int)sizeof(ps->w_buf))
            ps->w_count = (int)sizeof(ps->w_buf);
        ps->w_pos = 0;
    } else {
        if (ps->w_pos < (int)sizeof(ps->w_buf)) {
            ps->w_buf[ps->w_pos++] = (char)value;
        }
        if (ps->w_pos >= ps->w_count) {
            int host_fd = map_fd(va, ps->w_fd);
            if (host_fd < 0) {
                va->last_result = -1;
            } else {
                int n = write(host_fd, ps->w_buf, ps->w_count);
                va->last_result = n;
            }
            /* reset */
            ps->w_fd = -1;
            ps->w_count = 0;
            ps->w_pos = 0;
            ps->current_cmd = 0;
        }
    }
}

static void process_lseek(struct vm_args* va, struct parser_state* ps, uint8_t value) {
    /* protokol:
       outb(cmd=5), outb(fd),
       outb(offset_byte0), outb(offset_byte1), outb(offset_byte2), outb(offset_byte3),
       outb(whence)  (1 = SEEK_SET, 2 = SEEK_END)
       rezultat: 4 inb poziva (LSB first) koji vraćaju 32-bit signed offset ili 0xFFFFFFFF na grešku
    */
    if (ps->l_fd == -1) {
        ps->l_fd = (int)value;
    } else if (ps->l_offset_pos < 4) {
        ps->l_offset_acc |= ((uint32_t)value) << (8 * ps->l_offset_pos);
        ps->l_offset_pos++;
    } else {
        /* poslednji bajt: whence */
        ps->l_whence = (int)value;
        int host_fd = map_fd(va, ps->l_fd);
        off_t res = (off_t)-1;
        if (host_fd >= 0) {
            int wh = SEEK_SET;
            if (ps->l_whence == 2)
                wh = SEEK_END;
            /* interpretiramo l_offset_acc kao signed 32-bit */
            int32_t soff = (int32_t)ps->l_offset_acc;
            res = lseek(host_fd, (off_t)soff, wh);
        }
        if (res == (off_t)-1) {
            ps->result32_val = 0xFFFFFFFFu;
        } else {
            ps->result32_val = (uint32_t)res;
        }
        ps->result32_pending = 1;
        ps->result32_pos = 0;
        /* reset lseek parser state */
        ps->l_fd = -1;
        ps->l_offset_acc = 0;
        ps->l_offset_pos = 0;
        ps->l_whence = 0;
        ps->current_cmd = 0;
    }
}
void* run_vm(void* arg) {
    struct vm_args* va = (struct vm_args*)arg;
    struct kvm_sregs sregs;
    struct kvm_regs regs;
    int stop = 0;
    int ret = 0;
    int irq_pending = IRQ_COUNT;

    if (vm_init(&va->v, va->mem_size)) {
        fprintf(stderr, "Failed to init the VM (image %s)\n", va->guest_image);
        return NULL;
    }

    if (ioctl(va->v.vcpu_fd, KVM_GET_SREGS, &sregs) < 0) {
        perror("KVM_GET_SREGS");
        vm_destroy(&va->v);
        return NULL;
    }

    setup_long_mode(&va->v, &sregs, va->page_size);

    if (ioctl(va->v.vcpu_fd, KVM_SET_SREGS, &sregs) < 0) {
        perror("KVM_SET_SREGS");
        vm_destroy(&va->v);
        return NULL;
    }

    if (load_guest_image(&va->v, va->guest_image, GUEST_START_ADDR) < 0) {
        fprintf(stderr, "Failed to load guest image %s\n", va->guest_image);
        vm_destroy(&va->v);
        return NULL;
    }

    memset(&regs, 0, sizeof(regs));
    regs.rflags = 0x2;
    regs.rip = GUEST_START_ADDR;
    regs.rsp = va->mem_size - 0x1000;

    if (ioctl(va->v.vcpu_fd, KVM_SET_REGS, &regs) < 0) {
        perror("KVM_SET_REGS");
        vm_destroy(&va->v);
        return NULL;
    }

    va->v.run->request_interrupt_window = (irq_pending > 0);

    for (int i = 0; i < MAX_FDS; i++) {
        va->guest_fd_to_host_fd[i] = -1;
        va->next_guest_fd = 3;
        va->last_result = 0;
        va->read_buffer_len = 0;
        va->read_buffer_pos = 0;
        va->read_pending = 0;
    }
    /* parser state per-VM (drži se u okviru niti) */
    struct parser_state ps;
    memset(&ps, 0, sizeof(ps));
    ps.current_cmd = 0;
    ps.path_len = 0;
    ps.waiting_for_flag = 0;
    ps.tmp_fd = -1;
    ps.tmp_count = 0;
    ps.w_fd = -1;
    ps.w_count = 0;
    ps.w_pos = 0;
    while (!stop) {
        ret = ioctl(va->v.vcpu_fd, KVM_RUN, 0);
        if (ret == -1) {
            perror("KVM_RUN failed");
            vm_destroy(&va->v);
            return NULL;
        }

        switch (va->v.run->exit_reason) {
            case KVM_EXIT_IO: {
                char* p = (char*)va->v.run;
                uint16_t port = va->v.run->io.port;
                if (port == 0xE9) {
                    if (va->v.run->io.direction == KVM_EXIT_IO_OUT) {
                        printf("%c", *(p + va->v.run->io.data_offset));
                        fflush(stdout);
                    } else if (va->v.run->io.direction == KVM_EXIT_IO_IN) {
                        *(p + va->v.run->io.data_offset) = getchar();
                    }
                    break;
                }
                if (port == 0x0278) {
                    if (va->v.run->io.direction == KVM_EXIT_IO_OUT) {
                        uint8_t value = *(p + va->v.run->io.data_offset);

                        /* Ako nemamo aktivnu komandu, prvi bajt je komanda */
                        if (ps.current_cmd == 0) {
                            ps.current_cmd = (int)value;
                            ps.path_len = 0;
                            ps.waiting_for_flag = 0;
                            ps.tmp_fd = -1;
                            ps.tmp_count = 0;
                            ps.w_fd = -1;
                            ps.w_count = 0;
                            ps.w_pos = 0;
                            /* ne resetujemo read_buffer ovde; read_pending ostaje dok gost ne pročita */
                            ps.l_fd = -1;
                            ps.l_offset_acc = 0;
                            ps.l_offset_pos = 0;
                            ps.l_whence = 0;
                            
                            break;
                        }

                        /* dispatch po komandi */
                        switch (ps.current_cmd) {
                            case V_FILE_OPEN:
                                process_open(va, &ps, value);
                                break;
                            case V_FILE_CLOSE:
                                process_close(va, &ps, value);
                                break;
                            case V_FILE_READ:
                                process_read(va, &ps, value);
                                break;
                            case V_FILE_WRITE:
                                process_write(va, &ps, value);
                                break;
                            case V_FILE_LSEEK:

                                process_lseek(va, &ps, value);
                                break;
                            default:
                                /* nepoznata komanda */
                                va->last_result = -1;
                                ps.current_cmd = 0;
                                break;
                        }
                        break;
                    } else if (va->v.run->io.direction == KVM_EXIT_IO_IN) {
                        /* gost očekuje rezultat ili bajt iz read_buffer */
                        uint8_t* dst = (uint8_t*)(p + va->v.run->io.data_offset);
                        if (ps.result32_pending) {
                            *dst = (uint8_t)((ps.result32_val >> (8 * ps.result32_pos)) & 0xFF);

                            ps.result32_pos++;

                            if (ps.result32_pos >= 4) {
                                ps.result32_pending = 0;
                                ps.result32_pos = 0;
                            }

                            break;
                        }
                        if (va->read_pending == 1) {
                            /* prvo vratimo broj pročitanih bajtova (n) kao jedan bajt */
                            *dst = (uint8_t)va->last_result;
                            va->read_pending = 2; /* sledeci IN vraća podatke (ako ih ima) */
                        } else if (va->read_pending == 2 && va->read_buffer_pos < va->read_buffer_len) {
                            /* vraćamo podatke iz bafera redom */
                            *dst = (uint8_t)va->read_buffer[va->read_buffer_pos++];
                            /* ako smo poslali sve podatke, resetujemo stanje */
                            if (va->read_buffer_pos >= va->read_buffer_len) {
                                va->read_pending = 0;
                                va->read_buffer_len = 0;
                                va->read_buffer_pos = 0;
                            }
                        } else {
                            /* nema pending read, vratimo poslednji rezultat kao jedan bajt */
                            *dst = (uint8_t)va->last_result;
                        }
                    }
                }
                break;
            }
            case KVM_EXIT_IRQ_WINDOW_OPEN:
                if (irq_pending > 0) {
                    if (inject_irq(&va->v, IRQ_NUM) < 0) {
                        stop = 1;
                    }
                    irq_pending--;
                } else {
                    va->v.run->request_interrupt_window = 0;
                }
                break;
            case KVM_EXIT_HLT:
                printf("KVM_EXIT_HLT (guest %s)\n", va->guest_image);
                stop = 1;
                break;
            case KVM_EXIT_SHUTDOWN:
                fprintf(stderr, "KVM_EXIT_SHUTDOWN (guest %s)\n", va->guest_image);
                stop = 1;
                break;
            case KVM_EXIT_FAIL_ENTRY:
                fprintf(stderr, "KVM_EXIT_FAIL_ENTRY (guest %s) reason=0x%llx\n", va->guest_image,
                        (unsigned long long)va->v.run->fail_entry.hardware_entry_failure_reason);
                stop = 1;
                break;
            case KVM_EXIT_INTERNAL_ERROR:
                fprintf(stderr, "KVM_EXIT_INTERNAL_ERROR (guest %s) suberror=%d\n", va->guest_image,
                        va->v.run->internal.suberror);
                stop = 1;
                break;
            default:
                fprintf(stderr, "Unexpected exit reason %d (guest %s)\n", va->v.run->exit_reason, va->guest_image);
                stop = 1;
                break;
        }
    }

    vm_destroy(&va->v);
    return NULL;
}

int main(int argc, char* argv[]) {
    size_t mem_size = 0;
    int page_size = 0;
    const char* guest_images[MAX_GUESTS];
    int guest_count = 0;
    // Parsiranje argumenata - uradio robot
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") || !strcmp(argv[i], "--memory")) {
            mem_size = atoi(argv[++i]) << 20;
            if (mem_size != (2 << 20) && mem_size != (4 << 20) && mem_size != (8 << 20) && mem_size != (16 << 20)) {
                fprintf(stderr, "Invalid memory size. Allowed: 2,4,8,16 MB\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--page")) {
            page_size = atoi(argv[++i]);
            if (page_size != 4 && page_size != 2) {
                fprintf(stderr, "Invalid page size. Allowed: 4 or 2\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "-g") || !strcmp(argv[i], "--guest")) {
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                if (guest_count >= MAX_GUESTS) {
                    fprintf(stderr, "Too many guests (max %d)\n", MAX_GUESTS);
                    return 1;
                }
                guest_images[guest_count++] = argv[++i];
            }
        }
    }

    if (mem_size == 0 || page_size == 0 || guest_count == 0) {
        fprintf(stderr, "Usage: %s -m <2|4|8> -p <4|2> -g <guest1.img> [guest2.img ...]\n", argv[0]);
        return 1;
    }

    pthread_t tids[MAX_GUESTS];
    struct vm_args args[MAX_GUESTS];

    for (int g = 0; g < guest_count; g++) {
        args[g].guest_image = guest_images[g];
        args[g].mem_size = mem_size;
        args[g].page_size = page_size;
        pthread_create(&tids[g], NULL, run_vm, &args[g]);
    }

    for (int g = 0; g < guest_count; g++) {
        pthread_join(tids[g], NULL);
    }

    return 0;
}
