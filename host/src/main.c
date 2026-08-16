#include "console.h"
#include "protocol.h"
#include "shared.h"
#include "vfs.h"
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
static void handle_port_510(struct vm_args* va, char* kvm_run_ptr) {
    int direction = va->v.run->io.direction;
    if (direction == KVM_EXIT_IO_OUT) {
        if (va->irq_role != VM_ROLE_WRITER)
            return; // not expected

        uint8_t byte = *(uint8_t*)(kvm_run_ptr + va->v.run->io.data_offset);

        if (va->w_count_pos < 4) {
            va->w_count |= ((uint32_t)byte) << (8 * va->w_count_pos);
            va->w_count_pos++;

            if (va->w_count_pos == 4) {
                if (va->w_count > BUFFER_SIZE)
                    va->w_count = BUFFER_SIZE;
                va->w_data_pos = 0;

                // 0 sentinel
                if (va->w_count == 0) {
                    pthread_mutex_lock(&g_shared.mutex);
                    g_shared.buf_len = 0;
                    g_shared.readers_done = 0;
                    g_shared.seq++;
                    g_shared.data_ready = 1;
                    pthread_cond_broadcast(&g_shared.cond_data);
                    pthread_mutex_unlock(&g_shared.mutex);

                    va->w_count_pos = 0;
                    va->w_count = 0;
                    va->w_data_pos = 0;
                    va->w_ack_pos = 0;
                }
            }
        } else {
            // temp bafer
            if (va->w_data_pos < BUFFER_SIZE)
                va->w_tmp_buf[va->w_data_pos] = byte;
            va->w_data_pos++;

            if (va->w_data_pos >= va->w_count) {
                // copy tmp -> g_shared.buf
                pthread_mutex_lock(&g_shared.mutex);
                memcpy(g_shared.buf, va->w_tmp_buf, va->w_count);
                g_shared.buf_len = va->w_count;
                g_shared.readers_done = 0;
                g_shared.seq++;
                g_shared.data_ready = 1;
                pthread_cond_broadcast(&g_shared.cond_data);
                pthread_mutex_unlock(&g_shared.mutex);

                va->w_count_pos = 0;
                va->w_count = 0;
                va->w_data_pos = 0;
                va->w_ack_pos = 0;
            }
        }
        return;
    }

    if (direction == KVM_EXIT_IO_IN) {
        uint8_t* dst = (uint8_t*)(kvm_run_ptr + va->v.run->io.data_offset);

        if (!va->irq_mode_sent) {
            *dst = (uint8_t)va->irq_role;
            va->irq_mode_sent = 1;
            return;
        }

        if (va->irq_role != VM_ROLE_READER)
            return;

        if (va->r_count_pos < 4) {
            pthread_mutex_lock(&g_shared.mutex);

            if (va->r_count_pos == 0) {
                while (!g_shared.data_ready || g_shared.seq == va->r_last_seq)
                    pthread_cond_wait(&g_shared.cond_data, &g_shared.mutex);
                va->r_last_seq = g_shared.seq;
            } else {
                while (!g_shared.data_ready)
                    pthread_cond_wait(&g_shared.cond_data, &g_shared.mutex);
            }

            *dst = (uint8_t)((g_shared.buf_len >> (8 * va->r_count_pos)) & 0xFF);
            pthread_mutex_unlock(&g_shared.mutex);

            va->r_count_pos++;
            if (va->r_count_pos == 4) {
                va->r_count = g_shared.buf_len;
                va->r_data_pos = 0;
            }
        } else if (va->r_data_pos < va->r_count) {
            pthread_mutex_lock(&g_shared.mutex);
            *dst = g_shared.buf[va->r_data_pos];
            pthread_mutex_unlock(&g_shared.mutex);
            va->r_data_pos++;
        }
    }
}

static void handle_port_520(struct vm_args* va, char* kvm_run_ptr) {
    int direction = va->v.run->io.direction;

    if (direction == KVM_EXIT_IO_IN) {
        if (va->irq_role != VM_ROLE_WRITER)
            return;

        uint8_t* dst = (uint8_t*)(kvm_run_ptr + va->v.run->io.data_offset);

        pthread_mutex_lock(&g_shared.mutex);
        while (g_shared.readers_done < g_shared.total_readers)
            pthread_cond_wait(&g_shared.cond_done, &g_shared.mutex);
        uint32_t written = g_shared.buf_len;
        pthread_mutex_unlock(&g_shared.mutex);

        *dst = (uint8_t)((written >> (8 * va->w_ack_pos)) & 0xFF);
        va->w_ack_pos++;

        if (va->w_ack_pos >= 4) {
            va->w_ack_pos = 0;
            pthread_mutex_lock(&g_shared.mutex);
            g_shared.data_ready = 0;
            g_shared.readers_done = 0;
            pthread_mutex_unlock(&g_shared.mutex);
        }
        return;
    }

    if (direction == KVM_EXIT_IO_OUT) {
        if (va->irq_role != VM_ROLE_READER)
            return;

        uint8_t byte = *(uint8_t*)(kvm_run_ptr + va->v.run->io.data_offset);

        va->r_ack_acc |= ((uint32_t)byte) << (8 * va->r_ack_pos);
        va->r_ack_pos++;

        if (va->r_ack_pos >= 4) {
            va->r_ack_pos = 0;
            va->r_ack_acc = 0;
            va->r_count_pos = 0;
            va->r_count = 0;
            va->r_data_pos = 0;

            pthread_mutex_lock(&g_shared.mutex);
            g_shared.readers_done++;
            if (g_shared.readers_done >= g_shared.total_readers)
                pthread_cond_signal(&g_shared.cond_done);
            pthread_mutex_unlock(&g_shared.mutex);
        }
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

    // druga faza
    for (int i = 0; i < MAX_FDS; i++) {
        va->guest_fd_to_host_fd[i] = -1;
    }
    va->next_guest_fd = 3;
    va->last_result = 0;
    va->read_buffer_len = 0;
    va->read_buffer_pos = 0;
    va->read_pending = 0;

    // treca faza
    va->irq_mode_sent = 0;
    va->w_count_pos = 0;
    va->w_count = 0;
    va->w_data_pos = 0;
    va->w_ack_pos = 0;
    va->r_count_pos = 0;
    va->r_count = 0;
    va->r_data_pos = 0;
    va->r_ack_pos = 0;
    va->r_ack_acc = 0;
    va->r_last_seq = (uint32_t)(-1);

    struct parser_state ps;
    parser_init(&ps);

    while (!stop) {
        ret = ioctl(va->v.vcpu_fd, KVM_RUN, 0);
        if (ret == -1) {
            perror("KVM_RUN failed");
            vm_destroy(&va->v);
            return NULL;
        }
        char* p = (char*)va->v.run;

        switch (va->v.run->exit_reason) {
            case KVM_EXIT_IO: {
                uint16_t port = va->v.run->io.port;
                if (port == 0xE9) {
                    if (va->v.run->io.direction == KVM_EXIT_IO_OUT) {
                        char ch = *(p + va->v.run->io.data_offset);
                        console_write_char(va, ch);
                    } else if (va->v.run->io.direction == KVM_EXIT_IO_IN) {
                        *(p + va->v.run->io.data_offset) = getchar();
                    }
                    break;
                }
                if (port == 0x0278) {
                    if (va->v.run->io.direction == KVM_EXIT_IO_OUT) {
                        uint8_t value = *(p + va->v.run->io.data_offset);

                        // no active cmd, first byte is cmd
                        if (ps.current_cmd == 0) {
                            parser_set_value(&ps, value);
                            break;
                        }
                        // calls from vfs.c
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
                                // not expected
                                va->last_result = -1;
                                ps.current_cmd = 0;
                                break;
                        }
                        break;
                    } else if (va->v.run->io.direction == KVM_EXIT_IO_IN) {
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
                            *dst = (uint8_t)va->last_result;
                            va->read_pending = 2;
                        } else if (va->read_pending == 2 && va->read_buffer_pos < va->read_buffer_len) {
                            // sending data
                            *dst = (uint8_t)va->read_buffer[va->read_buffer_pos++];
                            // all sent: reset
                            if (va->read_buffer_pos >= va->read_buffer_len) {
                                va->read_pending = 0;
                                va->read_buffer_len = 0;
                                va->read_buffer_pos = 0;
                            }
                        } else {
                            // return last_result
                            *dst = (uint8_t)va->last_result;
                        }
                    }
                    break;
                }
                if (port == 0x510) {
                    handle_port_510(va, p);
                    break;
                }
                if (port == 0x520) {
                    handle_port_520(va, p);
                    break;
                }
                break;
            }
            case KVM_EXIT_IRQ_WINDOW_OPEN: {
                if (irq_pending > 0) {
                    if (inject_irq(&va->v, IRQ_NUM) < 0) {
                        stop = 1;
                        break;
                    }
                    irq_pending--;
                }
                if (irq_pending == 0)
                    va->v.run->request_interrupt_window = 0;
                break;
            }
            case KVM_EXIT_HLT:
                console_flush(va);
                printf("KVM_EXIT_HLT (guest %s)\n", va->guest_image);
                stop = 1;
                break;
            case KVM_EXIT_SHUTDOWN: {
                struct kvm_regs dbg_regs;
                ioctl(va->v.vcpu_fd, KVM_GET_REGS, &dbg_regs);
                fprintf(stderr, "KVM_EXIT_SHUTDOWN (VM %d) RIP=0x%llx RSP=0x%llx\n", va->vm_id,
                        (unsigned long long)dbg_regs.rip, (unsigned long long)dbg_regs.rsp);
                stop = 1;
                break;
            }
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
    for (int i = 0; i < va->cow_count; i++) {
        if (va->cow_map[i].active) {
            unlink(va->cow_map[i].local_path);
            va->cow_map[i].active = 0;
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

    struct shared_list shared_files[MAX_SHARED_FILES];
    int shared_count = 0;
    // Parsiranje argumenata - uradio robot
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") || !strcmp(argv[i], "--memory")) {
            mem_size = atoi(argv[++i]) << 20;
            if (mem_size != (2 << 20) && mem_size != (4 << 20) && mem_size != (8 << 20)) {
                fprintf(stderr, "Invalid memory size. Allowed: 2,4,8 MB\n");
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
        } else if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--file")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing argument for -f/--file\n");
                return 1;
            }
            if (shared_count >= MAX_SHARED_FILES) {
                fprintf(stderr, "Too many shared files (max %d)\n", MAX_SHARED_FILES);
                return 1;
            }
            strncpy(shared_files[shared_count].path, argv[++i], MAX_PATH_LEN - 1);
            shared_files[shared_count].path[MAX_PATH_LEN - 1] = '\0';
            shared_count++;
            continue;
        }
    }
    if (console_init() < 0) {
        fprintf(stderr, "Failed to init console mutex\n");
        return 1;
    }
    if (mem_size == 0 || page_size == 0 || guest_count == 0) {
        fprintf(stderr, "Usage: %s -m <2|4|8> -p <4|2> -g <guest1.img> [guest2.img ...]\n", argv[0]);
        return 1;
    }

    int num_readers = (guest_count > 1) ? (guest_count - 1) : 0;
    if (num_readers > 0) {
        if (shared_init(num_readers) < 0) {
            fprintf(stderr, "Failed to init shared buffer\n");
            return 1;
        }
    }
    pthread_t tids[MAX_GUESTS];
    struct vm_args args[MAX_GUESTS];

    // init and thread create
    for (int g = 0; g < guest_count; g++) {
        args[g].guest_image = guest_images[g];
        args[g].mem_size = mem_size;
        args[g].page_size = page_size;

        args[g].shared_files = shared_files;
        args[g].shared_count = shared_count;
        args[g].vm_id = g;

        args[g].irq_role = (g == 0) ? VM_ROLE_WRITER : VM_ROLE_READER;

        for (int k = 0; k < MAX_COW_ENTRIES; k++) {
            args[g].cow_map[k].active = 0;
            args[g].cow_map[k].orig_path[0] = '\0';
            args[g].cow_map[k].local_path[0] = '\0';
        }
        args[g].cow_count = 0;

        pthread_create(&tids[g], NULL, run_vm, &args[g]);
    }

    for (int g = 0; g < guest_count; g++) {
        pthread_join(tids[g], NULL);
    }

    if (num_readers > 0)
        shared_destroy();

    console_destroy();
    return 0;
}
