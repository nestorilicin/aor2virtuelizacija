#include <stdint.h>
#include "io.h"
#include "files.h"
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
#define O_RDWR 4
#define O_CREATE 8
void __attribute__((noreturn)) __attribute__((section(".start"))) _start(void) {
    int fd = open("local1.txt", O_RDWR | O_CREATE);
    serial_puts("B1: open -> fd="); serial_put_int(fd); serial_putc('\n');

    int n = write(fd, "hello", 5);
    serial_puts("B1: write -> n="); serial_put_int(n); serial_putc('\n');

    lseek(fd, 0, 1); /* SEEK_SET */
    char buf[16];
    n = read(fd, buf, 5);
    buf[n] = '\0';
    serial_puts("B1: read -> \""); serial_puts(buf); serial_puts("\"\n");

    close(fd);
    serial_puts("B1: DONE\n");
    halt_forever();
}