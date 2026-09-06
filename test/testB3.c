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
    int fd = open("seektest.txt", 4 | 8); /* O_RDWR|O_CREATE */
    write(fd, "0123456789", 10);

    int p1 = lseek(fd, 0, 2); /* SEEK_END */
    serial_puts("B3: SEEK_END -> "); serial_put_int(p1); serial_putc('\n');

    char buf[8];
    int n = read(fd, buf, 4);
    buf[n] = '\0';
    serial_puts("B3: read after SEEK_CUR -> \""); serial_puts(buf); serial_puts("\"\n");

    int p2 = lseek(fd, 2, 1); /* SEEK_SET */
    serial_puts("B3: SEEK_SET(2) -> "); serial_put_int(p2); serial_putc('\n');

    close(fd);
    halt_forever();
}