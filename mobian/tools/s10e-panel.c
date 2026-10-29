/*
 * s10e-panel: send MIPI DSI commands to the Galaxy S10e's S6E3FA7 panel
 * through the Exynos 9820 DSIM0 command FIFO, the way the downstream driver
 * does (dpu20/cal_9820/dsim_reg.c), while mainline has no DSI driver and the
 * panel runs on what S-Boot set up.
 *
 *   s10e-panel <byte> [byte...]     one DCS write (short for 1-2 bytes, long otherwise)
 *   s10e-panel aor <0x14..0x8d4>    brightness: AMOLED off ratio, bigger = dimmer
 *   s10e-panel status               DSIM FIFO state
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DSIM0_BASE	0x19080000
#define DSIM_PKTHDR	0x58
#define DSIM_PAYLOAD	0x5c
#define DSIM_FIFOCTRL	0x68
#define FULL_PH_SFR	(1 << 11)
#define EMPTY_PH_SFR	(1 << 10)
#define FULL_PL_SFR	(1 << 9)

#define DCS_SHORT_WRITE		0x05
#define DCS_SHORT_WRITE_PARAM	0x15
#define DCS_LONG_WRITE		0x39

static volatile uint32_t *dsim;

static uint32_t rd(int off) { return dsim[off / 4]; }
static void wr(int off, uint32_t v) { dsim[off / 4] = v; }

static int wait_clear(uint32_t bits)
{
	for (int i = 0; i < 100000; i++) {
		if (!(rd(DSIM_FIFOCTRL) & bits))
			return 0;
		usleep(10);
	}
	fprintf(stderr, "s10e-panel: FIFO stuck, FIFOCTRL=%08x\n", rd(DSIM_FIFOCTRL));
	return -1;
}

static int send(const uint8_t *b, int n)
{
	if (wait_clear(FULL_PH_SFR))
		return -1;
	if (n == 1) {
		wr(DSIM_PKTHDR, DCS_SHORT_WRITE | b[0] << 8);
	} else if (n == 2) {
		wr(DSIM_PKTHDR, DCS_SHORT_WRITE_PARAM | b[0] << 8 | b[1] << 16);
	} else {
		/* payload first, little-endian words, then the header with the count */
		for (int i = 0; i < n; i += 4) {
			uint32_t w = 0;
			for (int j = 0; j < 4 && i + j < n; j++)
				w |= (uint32_t)b[i + j] << (8 * j);
			if (wait_clear(FULL_PL_SFR))
				return -1;
			wr(DSIM_PAYLOAD, w);
		}
		wr(DSIM_PKTHDR, DCS_LONG_WRITE | (n & 0xff) << 8 | (n >> 8) << 16);
	}
	/* let the packet leave before the next one */
	for (int i = 0; i < 100000 && !(rd(DSIM_FIFOCTRL) & EMPTY_PH_SFR); i++)
		usleep(10);
	return 0;
}

int main(int argc, char **argv)
{
	uint8_t b[64];
	int n = 0, fd;

	if (argc < 2) {
		fprintf(stderr, "usage: s10e-panel <byte>... | aor <0x14..0x8d4> | status\n");
		return 2;
	}
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	dsim = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, DSIM0_BASE);
	if (dsim == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	if (!strcmp(argv[1], "status")) {
		printf("FIFOCTRL=%08x\n", rd(DSIM_FIFOCTRL));
		return 0;
	}
	if (!strcmp(argv[1], "aor")) {
		/* the downstream set-bl sequence, reduced to the AOR it changes below ~60 nits */
		static const uint8_t key_on[] = { 0xf0, 0x5a, 0x5a }, key_off[] = { 0xf0, 0xa5, 0xa5 };
		static const uint8_t update[] = { 0xf7, 0x03 };
		long v = argc > 2 ? strtol(argv[2], NULL, 0) : -1;
		uint8_t aor[3] = { 0xb1, 0, 0 };

		if (v < 0x14 || v > 0x8d4) {
			fprintf(stderr, "s10e-panel: aor wants 0x14 (brightest) .. 0x8d4 (dimmest)\n");
			return 2;
		}
		aor[1] = v >> 8;
		aor[2] = v & 0xff;
		return send(key_on, 3) || send(aor, 3) || send(update, 2) || send(key_off, 3);
	}
	for (int i = 1; i < argc && n < (int)sizeof(b); i++)
		b[n++] = strtoul(argv[i], NULL, 16);
	return send(b, n) ? 1 : 0;
}
