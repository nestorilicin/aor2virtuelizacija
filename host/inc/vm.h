#ifndef VM_H
#define VM_H

#include <linux/kvm.h>
#include <stddef.h>
#include <stdint.h>

#define MEM_SIZE (2u * 1024u * 1024u)
#define GUEST_START_ADDR 0x8000
#define GUEST_CODE_PAGES 16

#define IRQ_NUM 32
#define IRQ_COUNT 1

#define PDE64_PRESENT (1u << 0)
#define PDE64_RW (1u << 1)
#define PDE64_USER (1u << 2)
#define PDE64_PS (1u << 7)

#define CR0_PE (1u << 0)
#define CR0_PG (1u << 31)
#define CR4_PAE (1u << 5)
#define EFER_LME (1u << 8)
#define EFER_LMA (1u << 10)

#define V_FILE_OPEN 1
#define V_FILE_CLOSE 2
#define V_FILE_READ 3
#define V_FILE_WRITE 4
#define V_FILE_LSEEK 5
#define MAX_FDS 64
struct vm {
    int kvm_fd;
    int vm_fd;
    int vcpu_fd;
    char* mem;
    size_t mem_size;
    struct kvm_run* run;
    int run_mmap_size;
};

#define MAX_SHARED_FILES 64
#define MAX_COW_ENTRIES 64
#define MAX_PATH_LEN 256
#define CONSOLE_BUF_SIZE 512

struct shared_list {
    char path[MAX_PATH_LEN];
};

struct cow_entry {
    char orig_path[MAX_PATH_LEN];  
    char local_path[MAX_PATH_LEN]; 
    int active;                    
};
struct vm_args {
    struct vm v;
    const char* guest_image;
    size_t mem_size;
    int page_size;

    int guest_fd_to_host_fd[MAX_FDS];
    int next_guest_fd;
    int last_result;
    char read_buffer[32];
    int read_buffer_len;
    int read_buffer_pos;
    int read_pending;

    struct shared_list* shared_files; /* pokazivač na globalnu listu u main */
    int shared_count;

    struct cow_entry cow_map[MAX_COW_ENTRIES];
    int cow_count;

    int vm_id;

    char console_buf[CONSOLE_BUF_SIZE];
    int console_buf_pos;

    // FAZA V
    int irq_role; // WRITER ili READER
    int irq_mode_sent;
    // writer
    int w_count_pos;
    uint32_t w_count;
    uint32_t w_data_pos;
    uint8_t w_tmp_buf[256]; // BUFFFER_SIZE
    int w_ack_pos;

    // reader
    int r_count_pos;
    uint32_t r_count;
    uint32_t r_data_pos;
    int r_ack_pos;
    uint32_t r_ack_acc;

    uint32_t r_last_seq;

};

int vm_init(struct vm* v, size_t mem_size);
void vm_destroy(struct vm* v);
void setup_long_mode(struct vm* v, struct kvm_sregs* sregs, int page_size);
int load_guest_image(struct vm* v, const char* image_path, uint64_t load_addr);
int inject_irq(struct vm* v, unsigned int vector);

#endif /* VM_H */
