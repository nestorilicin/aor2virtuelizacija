#include <stdint.h>

#define ECHO_COUNT 5

/*
	A2: proverava da hipervizor podržava čitanje (IN) sa porta 0xE9.
    Gost cita ECHO_COUNT bajtova i odmah ih vraća nazad na isti port.

	Pokrenuti npr.: printf 'ABCDE' | ./hypervisor -m 2 -p 4 -g a2_echo.img
*/

static void outb(uint16_t port, uint8_t value)
{
	asm("outb %0,%1" : /* empty */ : "a" (value), "Nd" (port) : "memory");
}

static void serial_putc(char c)
{
	outb(0xE9, (uint8_t)c);
}

static void serial_puts(const char *s)
{
	for (; *s; ++s)
		serial_putc(*s);
}

static inline uint8_t inb(uint16_t port)
{
	uint8_t value;
	asm volatile("inb %1,%0" : "=a"(value) : "Nd"(port) : "memory");
	return value;
}

static __attribute__((noreturn)) void halt_forever(void) {
    for (;;)
        asm volatile("hlt");
}
void
__attribute__((noreturn))
__attribute__((section(".start")))
_start(void)
{

	serial_puts("A2: START\n");

	for (int i = 0; i < ECHO_COUNT; ++i) {
		uint8_t c = inb(0xE9);
		serial_putc(c);
	}

	serial_puts("\nA2: DONE\n");

	halt_forever();
}
