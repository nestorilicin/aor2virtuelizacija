#include "vm.h"

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
                if (va->v.run->io.port == 0xE9) {
                    if (va->v.run->io.direction == KVM_EXIT_IO_OUT) {
                        printf("%c", *(p + va->v.run->io.data_offset));
                        fflush(stdout);
                    } else if (va->v.run->io.direction == KVM_EXIT_IO_IN) {
                        *(p + va->v.run->io.data_offset) = getchar();
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
