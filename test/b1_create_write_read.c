#include <stdint.h>
#include "io.h"
#include "files.h"
/*
	B1: kreira fajl, upisuje u njega, zatvara ga, ponovo ga otvara za citanje
	i ispisuje procitani sadrzaj na serijski izlaz (0xE9).
*/

#define O_RD     1
#define O_WR     2
#define O_CREATE 8



static void serial_putc(char c)
{
	outb(0xE9, (uint8_t)c);
}

static void serial_puts(const char *s)
{
	for (; *s; ++s)
		serial_putc(*s);
}

static void serial_put_int(long v)
{
	char buf[24];
	int i = 0;
	int neg = v < 0;
	unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;

	do {
		buf[i++] = '0' + (u % 10);
		u /= 10;
	} while (u);

	if (neg)
		serial_putc('-');
	while (i > 0)
		serial_putc(buf[--i]);
}

static __attribute__((noreturn)) void halt_forever(void)
{
	for (;;)
		asm volatile("hlt");
}

extern int open(const char *path, int flags);
extern int close(int fd);
extern int read(int fd, char *buf, int count);
extern int write(int fd, const char *buf, int count);

void
__attribute__((noreturn))
__attribute__((section(".start")))
_start(void)
{
	const char *path = "b1_data.txt";
	const char *msg  = "Hello, file!\n";
	char buf[64];

	int wfd = open(path, O_WR | O_CREATE);
	serial_puts("B1: open(O_WR|O_CREATE) -> fd=");
	serial_put_int(wfd);
	serial_putc('\n');
	if (wfd < 0)
		halt_forever();

	int wn = write(wfd, msg, 13);
	serial_puts("B1: write() -> n=");
	serial_put_int(wn);
	serial_putc('\n');

	close(wfd);

	int rfd = open(path, O_RD);
	serial_puts("B1: open(O_RD) -> fd=");
	serial_put_int(rfd);
	serial_putc('\n');
	if (rfd < 0)
		halt_forever();

	int rn = read(rfd, buf, sizeof(buf) - 1);
	if (rn < 0)
		rn = 0;
	buf[rn] = '\0';

	serial_puts("B1: read() -> \"");
	serial_puts(buf);
	serial_puts("\"\n");

	close(rfd);

	serial_puts("B1: DONE\n");

	halt_forever();
}
