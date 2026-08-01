#ifndef IO_H
#define IO_H

#include <stdint.h>

#define V_FILE_OPEN 1
#define V_FILE_CLOSE 2
#define V_FILE_READ 3
#define V_FILE_WRITE 4
#define V_FILE_LSEEK 5
#define SEEK_SET_GUEST 1
#define SEEK_END_GUEST 2

static inline void outb(uint16_t port, uint8_t value)
{
	asm("outb %0,%1" : /* empty */ : "a" (value), "Nd" (port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    asm volatile("inb %1,%0" : "=a"(ret) : "Nd"(port));
    return ret;
}
int open(const char *path, int flags) {
    outb(0x278, V_FILE_OPEN);
    // filename
    while (*path) {
        outb(0x278, (uint8_t)*path++);
    }
    outb(0x278, 0); // end
    // flags
    outb(0x278, (uint8_t)flags);
    

    return (int)(int8_t)inb(0x278);
}

int write(int fd, const char *buf, int count) {
    outb(0x278, V_FILE_WRITE);
    outb(0x278, (uint8_t)fd);
    outb(0x278, (uint8_t)count);
    for (int i = 0; i < count; i++) {
        outb(0x278, (uint8_t)buf[i]);
    }
    return (int)(int8_t)inb(0x278);
}

int read(int fd, char *buf, int count) {
    outb(0x278, V_FILE_READ);
    outb(0x278, (uint8_t)fd);
    outb(0x278, (uint8_t)count);
    int n = (int)(int8_t)inb(0x278);
    for (int i = 0; i < n; i++) {
        buf[i] = (char)inb(0x278);
    }
    return n;
}

int close(int fd) {
    outb(0x278, V_FILE_CLOSE);
    outb(0x278, (uint8_t)fd);
    return (int)(int8_t)inb(0x278);
}
int lseek(int fd, const int offset, int off_flag) {
    uint32_t uoffset = (uint32_t)(int32_t)offset;
    /* pošalji komandu */
    outb(0x278, (uint8_t)V_FILE_LSEEK);
    /* pošalji fd */
    outb(0x278, (uint8_t)fd);
    /* pošalji offset LSB first */
    outb(0x278, (uint8_t)(uoffset & 0xFFu));
    outb(0x278, (uint8_t)((uoffset >> 8) & 0xFFu));
    outb(0x278, (uint8_t)((uoffset >> 16) & 0xFFu));
    outb(0x278, (uint8_t)((uoffset >> 24) & 0xFFu));
    /* pošalji whence */
    outb(0x278, (uint8_t)off_flag);

    /* pročitaj 4 bajta rezultata LSB first */
    uint32_t r0 = (uint32_t)inb(0x278);
    uint32_t r1 = (uint32_t)inb(0x278);
    uint32_t r2 = (uint32_t)inb(0x278);
    uint32_t r3 = (uint32_t)inb(0x278);
    uint32_t result_u = (r0) | (r1 << 8) | (r2 << 16) | (r3 << 24);

    if (result_u == 0xFFFFFFFFu) {
        return -1;
    }
    return (int)(int32_t)result_u;
}
#endif /* IO_H */
