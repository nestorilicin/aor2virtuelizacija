#include <stdint.h>
#include "io.h"
#include "files.h"
#define O_WR 2

static void outb_raw(uint16_t port, uint8_t value) {
    asm("outb %0,%1" : : "a" (value), "Nd" (port) : "memory");
}
static void serial_putc(char c) { outb_raw(0xE9, (uint8_t)c); }
static void serial_puts(const char *s) { for (; *s; ++s) serial_putc(*s); }
static void serial_put_int(long v) {
    char buf[24]; int i = 0;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    do { buf[i++] = '0' + (u % 10); u /= 10; } while (u);
    if (neg) serial_putc('-');
    while (i > 0) serial_putc(buf[--i]);
}
static __attribute__((noreturn)) void halt_forever(void) {
    for (;;) asm volatile("hlt");
}

void
__attribute__((noreturn))
__attribute__((section(".start")))
_start(void)
{
    int fd = open("shared.txt", O_WR);
    serial_puts("B4-WRITER: open(shared.txt, O_WR) -> fd=");
    serial_put_int(fd);
    serial_putc('\n');
    if (fd < 0)
        halt_forever();

    int n = write(fd, "MODIFIED", 8);
    serial_puts("B4-WRITER: write() -> n=");
    serial_put_int(n);
    serial_putc('\n');

    close(fd);
    serial_puts("B4-WRITER: DONE\n");
    halt_forever();
}