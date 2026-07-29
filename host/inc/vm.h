#ifndef VM_H
#define VM_H

#include <stddef.h>
#include <stdint.h>
#include <linux/kvm.h>

#define MEM_SIZE         (2u * 1024u * 1024u)
#define GUEST_START_ADDR 0x8000
#define GUEST_CODE_PAGES 16

#define IRQ_NUM   32
#define IRQ_COUNT 3

#define PDE64_PRESENT (1u << 0)
#define PDE64_RW      (1u << 1)
#define PDE64_USER    (1u << 2)
#define PDE64_PS      (1u << 7)

#define CR0_PE   (1u << 0)
#define CR0_PG   (1u << 31)
#define CR4_PAE  (1u << 5)
#define EFER_LME (1u << 8)
#define EFER_LMA (1u << 10)

struct vm {
	int kvm_fd;
	int vm_fd;
	int vcpu_fd;
	char *mem;
	size_t mem_size;
	struct kvm_run *run;
	int run_mmap_size;
};

int  vm_init(struct vm *v, size_t mem_size);
void vm_destroy(struct vm *v);
void setup_long_mode(struct vm *v, struct kvm_sregs *sregs);
int  load_guest_image(struct vm *v, const char *image_path, uint64_t load_addr);
int  inject_irq(struct vm *v, unsigned int vector);

#endif /* VM_H */
