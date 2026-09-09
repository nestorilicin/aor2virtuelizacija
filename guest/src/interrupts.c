#include "interrupts.h"
#include "descriptors.h"
#include "files.h"
#include "io.h"

#define VM_ROLE_READER 0
#define VM_ROLE_WRITER 1

#define CHUNK_SIZE 128
#define PORT_SHARED_DATA 0x510
#define PORT_SHARED_ACK 0x520

#define WRITER_INPUT_FILE "input.txt"
#define READER_OUTPUT_FILE "output.txt"

#define O_RD 1
#define O_WR 2
#define O_CREATE 8

#define SIGNAL_KILL_COUNT 0xFFFFFFFFu
#define SIGNAL_PRINT_STOP_COUNT 0xFFFFFFFEu

static inline void outb_u32(uint16_t port, uint32_t value) {
    outb(port, (uint8_t)(value & 0xFF));
    outb(port, (uint8_t)((value >> 8) & 0xFF));
    outb(port, (uint8_t)((value >> 16) & 0xFF));
    outb(port, (uint8_t)((value >> 24) & 0xFF));
}

static inline uint32_t inb_u32(uint16_t port) {
    uint32_t v = 0;
    v |= (uint32_t)inb(port);
    v |= (uint32_t)inb(port) << 8;
    v |= (uint32_t)inb(port) << 16;
    v |= (uint32_t)inb(port) << 24;
    return v;
}

/* "general-regs-only" sprečava GCC da emituje SSE instrukcije, koje su zabranjene unutar __attribute__((interrupt)) handlera */
static void __attribute__((interrupt, target("general-regs-only"))) irq32_handler(struct interrupt_frame* frame) {
    (void)frame;
	static char buf[CHUNK_SIZE];
    uint8_t mode = inb(PORT_SHARED_DATA);
 
    if (mode == VM_ROLE_WRITER) {
        // writer 0 je sentinel vrednost za kraj
        int fd = open(WRITER_INPUT_FILE, O_RD);
        if (fd < 0) {
            outb_u32(PORT_SHARED_DATA, 0);
            (void)inb_u32(PORT_SHARED_ACK);
            return;
        }
        while (1) {
            int n = read(fd, buf, CHUNK_SIZE);
            if (n <= 0) {
                close(fd);
                outb_u32(PORT_SHARED_DATA, 0);
                (void)inb_u32(PORT_SHARED_ACK);
                return;
            }

            outb_u32(PORT_SHARED_DATA, (uint32_t)n);
            for (int i = 0; i < n; i++)
                outb(PORT_SHARED_DATA, (uint8_t)buf[i]);

            
            (void)inb_u32(PORT_SHARED_ACK);

            char cmd = (char)inb(0xE9);
            if (cmd == 'k') {
                asm volatile("int $34");
            }
        }
 
    } else {
        // reader
        int fd = open(READER_OUTPUT_FILE, O_WR | O_CREATE);
 
        while (1) {
            uint32_t count = inb_u32(PORT_SHARED_DATA);
 
            if (count == SIGNAL_KILL_COUNT) {
                asm volatile("int $34");
            }
            if (count == SIGNAL_PRINT_STOP_COUNT) {
                asm volatile("int $35");
                break;
            }
            if (count == 0) {
                outb_u32(PORT_SHARED_ACK, 0);
                break;
            }
            uint32_t received = 0;
            while (received < count)
                buf[received++] = (char)inb(PORT_SHARED_DATA);

            if (fd >= 0)
                write(fd, buf, (int)received);
 
            outb_u32(PORT_SHARED_ACK, received);
        }
 
        if (fd >= 0)
            close(fd);
    }
}
static void __attribute__((interrupt, target("general-regs-only"))) irq34_handler(struct interrupt_frame* frame) {
    (void)frame;
    for (;;) {
        asm volatile("hlt");
    }
}

static void __attribute__((interrupt, target("general-regs-only"))) irq35_handler(struct interrupt_frame* frame) {
    (void)frame;
    const char *s;
    for (s = "Hello World!\n"; *s; ++s)
        outb(0xE9, (uint8_t)*s);
    
}
// danko kod
static struct idt_entry idt[IDT_ENTRIES];
static void set_idt_gate(unsigned n, void (*handler)(struct interrupt_frame*)) {
    uint64_t addr = (uint64_t)(uintptr_t)handler;
    idt[n].offset_low = addr & 0xFFFF;
    idt[n].selector = 0x08; /* 64-bit code segment */
    idt[n].ist = 0;
    idt[n].type_attr = 0x8E; /* P=1, DPL=0, 64-bit interrupt gate */
    idt[n].offset_mid = (addr >> 16) & 0xFFFF;
    idt[n].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[n].reserved = 0;
}

void init_idt(void) {
    struct dt_ptr p;

    set_idt_gate(32, irq32_handler);
    set_idt_gate(34, irq34_handler);
    set_idt_gate(35, irq35_handler);

    p.limit = sizeof(idt) - 1;
    p.base = (uint64_t)(uintptr_t)idt;
    __asm__ volatile("lidt %0" : : "m"(p) : "memory");
}
