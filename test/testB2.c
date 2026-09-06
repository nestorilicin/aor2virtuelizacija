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
void __attribute__((noreturn)) __attribute__((section(".start"))) _start(void) {
    int fd1 = open("1bad.txt", 8);   /* pocinje ciframa -> nevažeće */
    serial_puts("B2: open('1bad.txt') -> "); serial_put_int(fd1); serial_putc('\n');

    int fd2 = open("bad name.txt", 8); /* razmak -> nevažeće */
    serial_puts("B2: open('bad name.txt') -> "); serial_put_int(fd2); serial_putc('\n');

    int fd3 = open("valid_name1.txt", 8);
    serial_puts("B2: open('valid_name1.txt') -> "); serial_put_int(fd3); serial_putc('\n');

    halt_forever();
}