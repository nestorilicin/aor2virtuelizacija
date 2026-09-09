#include <stdint.h>

/*
	B3 (writer): otvara DELJENI fajl (prosledjen preko -f/--file hipervizoru)
	i upisuje u njega. 

    Pokrenuti zajedno sa b3_shared_reader.img i deljenim fajlom, npr:
		printf 'ORIGINAL' > shared.txt
		./hypervisor -m 4 -p 4 -g b3_shared_writer.img b3_shared_reader.img --file shared.txt
	Nakon izvrsavanja, shared.txt na disku domacina mora i dalje da sadrzi
	"ORIGINAL" (proveriti sa cat shared.txt).
*/

#define O_WR 2

extern int open(const char *path, int flags);
extern int close(int fd);
extern int write(int fd, const char *buf, int count);


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
	int fd = open("shared.txt", O_WR);
	serial_puts("B3-WRITER: open(shared.txt, O_WR) -> fd=");
	serial_put_int(fd);
	serial_putc('\n');
	if (fd < 0)
		halt_forever();

	int n = write(fd, "MODIFIED", 8);
	serial_puts("B3-WRITER: write() -> n=");
	serial_put_int(n);
	serial_putc('\n');

	close(fd);

	serial_puts("B3-WRITER: DONE\n");

	halt_forever();
}
