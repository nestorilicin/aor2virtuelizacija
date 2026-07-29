#include "descriptors.h"
#include "interrupts.h"
#include "io.h"

static struct idt_entry idt[IDT_ENTRIES];

/*
	"general-regs-only" sprečava GCC da emituje SSE instrukcije, koje su zabranjene unutar
	__attribute__((interrupt)) handlera
*/
static void __attribute__((interrupt, target("general-regs-only")))
irq0_handler(struct interrupt_frame *frame)
{
	const char *s;

	for (s = "IRQ0 received!\n"; *s; ++s)
		outb(0xE9, *s);
}

static void set_idt_gate(unsigned n, void (*handler)(struct interrupt_frame *))
{
	uint64_t addr = (uint64_t)(uintptr_t)handler;
	idt[n].offset_low  = addr & 0xFFFF;
	idt[n].selector    = 0x08;  /* 64-bit code segment */
	idt[n].ist         = 0;
	idt[n].type_attr   = 0x8E;  /* P=1, DPL=0, 64-bit interrupt gate */
	idt[n].offset_mid  = (addr >> 16) & 0xFFFF;
	idt[n].offset_high = (addr >> 32) & 0xFFFFFFFF;
	idt[n].reserved    = 0;
}

void init_idt(void)
{
	struct dt_ptr p;

	set_idt_gate(32, irq0_handler);

	p.limit = sizeof(idt) - 1;
	p.base  = (uint64_t)(uintptr_t)idt;
	asm volatile("lidt %0" : : "m"(p) : "memory");
}
