#include <stdint.h>
#include "io.h"
#include "files.h"

#define O_RD 1

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
    int fd = open("shared.txt", O_RD);
    serial_puts("B4-READER: open(shared.txt, O_RD) -> fd=");
    serial_put_int(fd);
    serial_putc('\n');
    if (fd < 0)
        halt_forever();

    char buf[32];
    int n = read(fd, buf, sizeof(buf) - 1);
    buf[n < 0 ? 0 : n] = '\0';
    serial_puts("B4-READER: read() -> \"");
    serial_puts(buf);
    serial_puts("\"\n");

    close(fd);
    serial_puts("B4-READER: DONE\n");
    halt_forever();
}