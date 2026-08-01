#include "protocol.h"
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
    }
    va->next_guest_fd = 3;
    va->last_result = 0;
    va->read_buffer_len = 0;
    va->read_buffer_pos = 0;
    va->read_pending = 0;

    /* parser state per-VM (drži se u okviru niti) */
    struct parser_state ps;
    parser_init(&ps);

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
                            parser_set_value(&ps, value);
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
