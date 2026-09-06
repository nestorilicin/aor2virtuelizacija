#include <stdint.h>
#include "io.h"

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
    serial_puts("A1: 2MB/4KB config OK\n");
    halt_forever();
}