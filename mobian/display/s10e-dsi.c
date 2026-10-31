/* s10e-dsi: send DCS packets to the S10e panel and read it back, straight through DSIM's SFR FIFOs (/dev/mem).
 *   s10e-dsi w B0 B1 ...      DCS write (short 0x05/0x15 or long 0x39)
 *   s10e-dsi r REG N          DCS read of N bytes (max-return-size, then a read with BTA)
 * The kernel's DSI host has no RX path; this is for probing only. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DSIM_BASE	0x19080000
#define INTSRC		0x50
#define PKTHDR		0x58
#define PAYLOAD		0x5c
#define RXFIFO		0x60
#define FIFOCTRL	0x68
#define RX_DATA_DONE	(1u << 18)
#define BTA_TOUT	(1u << 20)
#define LPRX_TOUT	(1u << 21)
#define EMPTY_RX	(1u << 12)
#define FULL_PH		(1u << 11)
#define EMPTY_PH	(1u << 10)
#define FULL_PL		(1u << 9)

static volatile uint32_t *r;
#define R(o)	r[(o) / 4]

static long now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000L + t.tv_nsec / 1000;
}

static int wait_bits(unsigned off, uint32_t mask, int set, long us)
{
	long end = now_us() + us;
	while (((R(off) & mask) != 0) != set)
		if (now_us() > end) {
			fprintf(stderr, "timeout reg 0x%x mask 0x%x (0x%08x)\n", off, mask, R(off));
			return -1;
		}
	return 0;
}

static int send(uint8_t id, const uint8_t *d, unsigned n, int bta)
{
	if (wait_bits(FIFOCTRL, FULL_PH, 0, 100000))
		return -1;
	uint32_t hdr;
	if (id == 0x39 || id == 0x29) {
		for (unsigned i = 0; i < n; i += 4) {
			uint32_t v = 0;
			memcpy(&v, d + i, n - i < 4 ? n - i : 4);
			if (wait_bits(FIFOCTRL, FULL_PL, 0, 100000))
				return -1;
			R(PAYLOAD) = v;
		}
		hdr = id | (n & 0xff) << 8 | (n >> 8) << 16;
	} else
		hdr = id | (n > 0 ? d[0] : 0) << 8 | (n > 1 ? d[1] : 0) << 16;
	R(PKTHDR) = hdr | (bta ? 1u << 24 : 0);
	return wait_bits(FIFOCTRL, EMPTY_PH, 1, 100000);
}

static int dcs_write(const uint8_t *d, unsigned n)
{
	return send(n == 1 ? 0x05 : n == 2 ? 0x15 : 0x39, d, n, 0);
}

int main(int argc, char **argv)
{
	if (argc < 3)
		return fprintf(stderr, "usage: s10e-dsi w B... | r REG N\n"), 1;
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0)
		return perror("/dev/mem"), 1;
	r = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, DSIM_BASE);
	if (r == MAP_FAILED)
		return perror("mmap"), 1;

	if (argv[1][0] == 'w') {
		uint8_t buf[512];
		unsigned n = 0;
		for (int i = 2; i < argc && n < sizeof buf; i++)
			buf[n++] = strtoul(argv[i], NULL, 16);
		return dcs_write(buf, n) ? 2 : 0;
	}

	uint8_t reg = strtoul(argv[2], NULL, 16);
	unsigned cnt = argc > 3 ? strtoul(argv[3], NULL, 0) : 1;
	/* drain stale RX */
	for (int i = 0; i < 64 && !(R(FIFOCTRL) & EMPTY_RX); i++)
		(void)R(RXFIFO);
	R(INTSRC) = RX_DATA_DONE | BTA_TOUT | LPRX_TOUT;
	uint8_t mr[2] = { cnt & 0xff, cnt >> 8 };
	if (send(0x37, mr, 2, 0))
		return 2;
	if (send(0x06, &reg, 1, 1))
		return 2;
	long end = now_us() + 200000;
	while (R(FIFOCTRL) & EMPTY_RX)
		if (now_us() > end)
			return fprintf(stderr, "read timeout INTSRC 0x%08x FIFOCTRL 0x%08x\n", R(INTSRC), R(FIFOCTRL)), 3;
	usleep(2000);	/* let the rest of a long packet land */
	R(INTSRC) = RX_DATA_DONE | BTA_TOUT | LPRX_TOUT;
	for (int guard = 0; guard < 64 && !(R(FIFOCTRL) & EMPTY_RX); guard++) {
		uint32_t v = R(RXFIFO), t = v & 0xff;
		if (t == 0x1c || t == 0x1a) {
			unsigned sz = (v >> 8) & 0xffff;
			for (unsigned i = 0; i < sz; i += 4) {
				uint32_t w = R(RXFIFO);
				for (unsigned j = 0; j < 4 && i + j < sz; j++)
					printf("%02x ", (w >> (8 * j)) & 0xff);
			}
		} else if (t == 0x21 || t == 0x11)
			printf("%02x ", (v >> 8) & 0xff);
		else if (t == 0x22 || t == 0x12)
			printf("%02x %02x ", (v >> 8) & 0xff, (v >> 16) & 0xff);
		else if (t == 0x02)
			fprintf(stderr, "[ack/err 0x%04x] ", (v >> 8) & 0xffff);
		else if (t == 0x08)
			;
		else
			fprintf(stderr, "[rx 0x%08x] ", v);
	}
	printf("\n");
	return 0;
}
