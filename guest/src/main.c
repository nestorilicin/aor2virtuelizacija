#include <stdint.h>

/*
	B2:
	1) otvaranje nepostojeceg fajla BEZ O_CREATE mora da vrati -1
	   (ne sme implicitno da ga kreira).
	2) O_RDWR: kreirati fajl, upisati, premotati na pocetak (lseek SEEK_SET)
	   i procitati u ISTOJ sesiji (bez close/open), pa uporediti sa upisanim.
*/

#define O_RD     1
#define O_RDWR   4
#define O_CREATE 8
#define SEEK_SET 1

extern int open(const char *path, int flags);
extern int close(int fd);
extern int read(int fd, char *buf, int count);
extern int write(int fd, const char *buf, int count);
extern int lseek(int fd, const int offset, int off_flag);

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

void
__attribute__((noreturn))
__attribute__((section(".start")))
_start(void)
{
	int missing_fd = open("b2_missing.txt", O_RD);
	serial_puts("B2: open(missing, O_RD) -> fd=");
	serial_put_int(missing_fd);
	serial_puts(missing_fd < 0 ? " (OK, expected -1)\n" : " (FAIL, expected -1)\n");

	const char *msg = "RDWR-OK";
	char buf[16];

	int fd = open("b2_rdwr.txt", O_RDWR | O_CREATE);
	serial_puts("B2: open(O_RDWR|O_CREATE) -> fd=");
	serial_put_int(fd);
	serial_putc('\n');
	if (fd < 0)
		halt_forever();

	write(fd, msg, 7);

	int pos = lseek(fd, 0, SEEK_SET);
	serial_puts("B2: lseek(0, SEEK_SET) -> pos=");
	serial_put_int(pos);
	serial_putc('\n');

	int n = read(fd, buf, 7);
	buf[n < 0 ? 0 : n] = '\0';
	serial_puts("B2: read-back -> \"");
	serial_puts(buf);
	serial_puts("\"\n");

	close(fd);

	serial_puts("B2: DONE\n");

	halt_forever();
}
