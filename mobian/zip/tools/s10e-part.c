/* s10e-part -- carve a 'linux' partition out of the end of 'userdata' on
   the Samsung Galaxy S10e (SM-G970F) UFS LUN 0, from inside LineageOS
   recovery.

   Usage:
     s10e-part [--sector-size N] show  DISK
     s10e-part [--sector-size N] plan  DISK LINUX_GIB
     s10e-part [--sector-size N] apply DISK LINUX_GIB

   It is deliberately narrow: it only ever shrinks the last partition,
   'userdata', and adds one partition, 'linux', in the space freed behind
   it.  Everything is read and checked (header CRC, entries CRC, layout)
   before a single byte is written.

   Phone facts this relies on:
   - The logical sector size is 4096, so it is asked for (BLKSSZGET),
     never assumed to be 512.  For a regular file (tests) it defaults to
     4096, or --sector-size.
   - Samsung writes no backup GPT; the alternate-LBA field points at a
     sector that may hold garbage.  A backup is only touched when it is
     really there (signature, CRCs, and the same entries as the primary).
   - Recovery has system/vendor/cache/efs mounted, so BLKRRPART fails;
     the kernel is told with BLKPG_RESIZE_PARTITION + BLKPG_ADD_PARTITION.

   Exit codes: 0 ok; 1 refused (nothing written); 2 a write failed;
   3 the GPT was written but the kernel was not told (reboot to see it).

   Build: see the Makefile next to this file.  No libraries beyond libc.  */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/blkpg.h>
#include <linux/fs.h>

#define GIB (UINT64_C (1) << 30)
#define MIB (UINT64_C (1) << 20)
#define MIN_LINUX_GIB 8
#define MIN_USERDATA (UINT64_C (16) * GIB)
#define NAME_UNITS 36           /* UTF-16 code units in an entry name */

/* 0FC63DAF-8483-4772-8E79-3D69D8477DE4, in the on-disk mixed-endian
   byte order (first three fields little-endian).  */
static const uint8_t linux_type[16] = {
  0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
  0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4
};

static const uint8_t zero_guid[16];

/* ---- small helpers ---------------------------------------------------- */

static void __attribute__ ((noreturn, format (printf, 2, 3)))
die (int code, const char *fmt, ...)
{
  va_list ap;

  fputs ("error: ", stderr);
  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
  fputc ('\n', stderr);
  exit (code);
}

static void *
xcalloc (size_t n)
{
  void *p = calloc (1, n);

  if (!p)
    die (1, "out of memory");
  return p;
}

/* The GPT is little-endian whatever the host is; spell it out instead of
   casting structs over the buffer.  */
static uint32_t
get32 (const uint8_t *p)
{
  return (uint32_t) p[0] | (uint32_t) p[1] << 8
         | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

static uint64_t
get64 (const uint8_t *p)
{
  return (uint64_t) get32 (p) | (uint64_t) get32 (p + 4) << 32;
}

static void
put32 (uint8_t *p, uint32_t v)
{
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

static void
put64 (uint8_t *p, uint64_t v)
{
  put32 (p, (uint32_t) v);
  put32 (p + 4, (uint32_t) (v >> 32));
}

/* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) as the UEFI spec uses;
   bitwise is plenty fast for 16 KiB and keeps the binary tiny.  */
static uint32_t
crc32 (const uint8_t *p, size_t n)
{
  uint32_t c = 0xffffffffu;

  while (n--)
    {
      c ^= *p++;
      for (int k = 0; k < 8; k++)
        c = (c >> 1) ^ (0xedb88320u & -(c & 1));
    }
  return ~c;
}

static void
guid_str (const uint8_t *g, char out[37])
{
  snprintf (out, 37,
            "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-"
            "%02X%02X%02X%02X%02X%02X",
            g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
            g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

/* ---- the disk ---------------------------------------------------------- */

struct disk
{
  const char *path;
  int fd;
  int is_blk;
  uint32_t ss;                  /* logical sector size in bytes */
  uint64_t nsect;               /* sectors on the disk */
};

static int
read_at (struct disk *d, uint64_t lba, void *buf, size_t len)
{
  uint8_t *p = buf;
  uint64_t off = lba * d->ss;

  while (len)
    {
      ssize_t r = pread (d->fd, p, len, (off_t) off);

      if (r < 0 && errno == EINTR)
        continue;
      if (r <= 0)
        return -1;
      p += r;
      off += (uint64_t) r;
      len -= (size_t) r;
    }
  return 0;
}

static void
write_at (struct disk *d, uint64_t lba, const void *buf, size_t len)
{
  const uint8_t *p = buf;
  uint64_t off = lba * d->ss;

  while (len)
    {
      ssize_t r = pwrite (d->fd, p, len, (off_t) off);

      if (r < 0 && errno == EINTR)
        continue;
      if (r <= 0)
        die (2, "write at LBA %" PRIu64 " failed: %s (GPT may be"
             " half-written; restore the backup)", lba, strerror (errno));
      p += r;
      off += (uint64_t) r;
      len -= (size_t) r;
    }
}

static void
disk_open (struct disk *d, const char *path, int rw, uint32_t ss_opt)
{
  struct stat st;

  d->path = path;
  d->fd = open (path, (rw ? O_RDWR : O_RDONLY) | O_CLOEXEC);
  if (d->fd < 0)
    die (1, "%s: %s", path, strerror (errno));
  if (fstat (d->fd, &st) < 0)
    die (1, "%s: %s", path, strerror (errno));

  if (S_ISBLK (st.st_mode))
    {
      int ss = 0;
      uint64_t bytes = 0;

      d->is_blk = 1;
      if (ioctl (d->fd, BLKSSZGET, &ss) < 0 || ss <= 0)
        die (1, "%s: BLKSSZGET: %s", path, strerror (errno));
      if (ioctl (d->fd, BLKGETSIZE64, &bytes) < 0)
        die (1, "%s: BLKGETSIZE64: %s", path, strerror (errno));
      /* A wrong --sector-size on the real disk would put every LBA in
         the wrong place; the kernel knows better, so a mismatch is a
         mistake by the caller.  */
      if (ss_opt && ss_opt != (uint32_t) ss)
        die (1, "%s has %d-byte sectors, not %u", path, ss, ss_opt);
      d->ss = (uint32_t) ss;
      d->nsect = bytes / d->ss;
    }
  else if (S_ISREG (st.st_mode))
    {
      /* An image of the phone's disk: 4096 unless told otherwise.  */
      d->is_blk = 0;
      d->ss = ss_opt ? ss_opt : 4096;
      d->nsect = (uint64_t) st.st_size / d->ss;
    }
  else
    die (1, "%s is neither a block device nor a regular file", path);

  if (d->ss < 512 || d->ss > 65536 || (d->ss & (d->ss - 1)))
    die (1, "unsupported sector size %u", d->ss);
}

/* ---- GPT --------------------------------------------------------------- */

struct gpt
{
  uint64_t lba;                 /* where this header lives */
  uint8_t *hdr;                 /* one whole sector */
  uint32_t hsz;
  uint64_t alt, first, last, ent_lba;
  uint32_t nent, esz;
  size_t ent_bytes;             /* nent * esz, the CRC'd part */
  size_t ent_sects;
  uint8_t *ent;                 /* ent_sects sectors */
};

static void
gpt_free (struct gpt *g)
{
  free (g->hdr);
  free (g->ent);
  memset (g, 0, sizeof *g);
}

/* Read and validate the header at LBA and its entry array.  Returns NULL
   on success, else a short reason (and G holds nothing).  */
static const char *
gpt_load (struct disk *d, uint64_t lba, struct gpt *g)
{
  static char why[128];
  uint8_t *h;
  uint32_t want, got;

  memset (g, 0, sizeof *g);
  if (lba >= d->nsect)
    return "header LBA is beyond the end of the disk";
  g->lba = lba;
  g->hdr = h = xcalloc (d->ss);
  if (read_at (d, lba, h, d->ss) < 0)
    return gpt_free (g), "cannot read header";
  if (memcmp (h, "EFI PART", 8) != 0)
    return gpt_free (g), "no GPT signature";

  /* Samsung's header size is 512, not 92: the CRC covers exactly
     header-size bytes, so take the field as it is (within reason).  */
  g->hsz = get32 (h + 12);
  if (g->hsz < 92 || g->hsz > d->ss)
    return gpt_free (g), "implausible header size";
  want = get32 (h + 16);
  put32 (h + 16, 0);
  got = crc32 (h, g->hsz);
  put32 (h + 16, want);
  if (got != want)
    {
      snprintf (why, sizeof why, "header CRC mismatch (stored %08x,"
                " computed %08x)", want, got);
      return gpt_free (g), why;
    }
  if (get64 (h + 24) != lba)
    return gpt_free (g), "header's own LBA field is wrong";

  g->alt = get64 (h + 32);
  g->first = get64 (h + 40);
  g->last = get64 (h + 48);
  g->ent_lba = get64 (h + 72);
  g->nent = get32 (h + 80);
  g->esz = get32 (h + 84);

  if (g->esz < 128 || g->esz % 8 || g->esz > 4096)
    return gpt_free (g), "implausible entry size";
  if (g->nent == 0 || g->nent > 4096)
    return gpt_free (g), "implausible entry count";
  if (g->first > g->last || g->last >= d->nsect)
    return gpt_free (g), "usable range does not fit the disk";

  g->ent_bytes = (size_t) g->nent * g->esz;
  g->ent_sects = (g->ent_bytes + d->ss - 1) / d->ss;
  if (g->ent_lba + g->ent_sects > d->nsect)
    return gpt_free (g), "entry array runs off the disk";
  /* The array must sit outside the usable area, or a partition could
     overwrite the table.  */
  if (!(g->ent_lba + g->ent_sects <= g->first || g->ent_lba > g->last))
    return gpt_free (g), "entry array overlaps the usable area";

  g->ent = xcalloc (g->ent_sects * d->ss);
  if (read_at (d, g->ent_lba, g->ent, g->ent_sects * d->ss) < 0)
    return gpt_free (g), "cannot read entry array";
  want = get32 (h + 88);
  got = crc32 (g->ent, g->ent_bytes);
  if (got != want)
    {
      snprintf (why, sizeof why, "entries CRC mismatch (stored %08x,"
                " computed %08x)", want, got);
      return gpt_free (g), why;
    }
  return NULL;
}

static uint8_t *
entry (struct gpt *g, uint32_t i)
{
  return g->ent + (size_t) i * g->esz;
}

static int
entry_used (struct gpt *g, uint32_t i)
{
  return memcmp (entry (g, i), zero_guid, 16) != 0;
}

/* Entry name as ASCII ('?' for anything else) -- enough to compare to
   'userdata' / 'linux' and to print.  */
static void
entry_name (struct gpt *g, uint32_t i, char out[NAME_UNITS + 1])
{
  const uint8_t *n = entry (g, i) + 56;
  int k;

  for (k = 0; k < NAME_UNITS; k++)
    {
      unsigned u = n[2 * k] | n[2 * k + 1] << 8;

      if (u == 0)
        break;
      out[k] = (u >= 0x20 && u < 0x7f) ? (char) u : '?';
    }
  out[k] = 0;
}

static uint64_t
entry_start (struct gpt *g, uint32_t i)
{
  return get64 (entry (g, i) + 32);
}

static uint64_t
entry_end (struct gpt *g, uint32_t i)
{
  return get64 (entry (g, i) + 40);
}

/* Every used entry inside the usable range and start <= end: anything
   else means a table this tool does not understand.  */
static void
check_entries (struct gpt *g)
{
  for (uint32_t i = 0; i < g->nent; i++)
    if (entry_used (g, i))
      {
        uint64_t s = entry_start (g, i), e = entry_end (g, i);

        if (s > e || s < g->first || e > g->last)
          die (1, "partition %u (%" PRIu64 "-%" PRIu64 ") lies outside"
               " the usable range", i + 1, s, e);
      }
}

static int
find_named (struct gpt *g, const char *name)
{
  char n[NAME_UNITS + 1];
  int found = -1;

  for (uint32_t i = 0; i < g->nent; i++)
    if (entry_used (g, i))
      {
        entry_name (g, i, n);
        if (strcmp (n, name) == 0)
          {
            if (found >= 0)
              die (1, "more than one partition is named '%s'", name);
            found = (int) i;
          }
      }
  return found;
}

static void
load_primary (struct disk *d, struct gpt *g)
{
  const char *why = gpt_load (d, 1, g);

  if (why)
    die (1, "%s: primary GPT at LBA 1: %s", d->path, why);
  check_entries (g);
}

/* ---- show -------------------------------------------------------------- */

static void
fmt_size (uint64_t bytes, char out[32])
{
  if (bytes >= GIB)
    snprintf (out, 32, "%.1f GiB", (double) bytes / GIB);
  else if (bytes >= MIB)
    snprintf (out, 32, "%.1f MiB", (double) bytes / MIB);
  else
    snprintf (out, 32, "%.1f KiB", (double) bytes / 1024);
}

static int
cmd_show (struct disk *d)
{
  struct gpt g, b;
  uint64_t max_end = 0;
  char name[NAME_UNITS + 1], type[37], sz[32];

  load_primary (d, &g);
  printf ("disk %s: %" PRIu64 " sectors of %u bytes, usable %" PRIu64
          "-%" PRIu64 ", %u entries of %u bytes at LBA %" PRIu64 "\n",
          d->path, d->nsect, d->ss, g.first, g.last, g.nent, g.esz,
          g.ent_lba);
  {
    const char *why = gpt_load (d, g.alt, &b);

    printf ("backup GPT at LBA %" PRIu64 ": %s\n", g.alt,
            why ? why : "valid");
    if (!why)
      gpt_free (&b);
  }
  printf ("%3s %12s %12s %10s  %-16s %s\n",
          "#", "start", "end", "size", "name", "type");
  for (uint32_t i = 0; i < g.nent; i++)
    if (entry_used (&g, i))
      {
        uint64_t s = entry_start (&g, i), e = entry_end (&g, i);

        entry_name (&g, i, name);
        guid_str (entry (&g, i), type);
        fmt_size ((e - s + 1) * d->ss, sz);
        printf ("%3u %12" PRIu64 " %12" PRIu64 " %10s  %-16s %s\n",
                i + 1, s, e, sz, name, type);
        if (e > max_end)
          max_end = e;
      }
  fmt_size ((g.last - max_end) * d->ss, sz);
  printf ("free after last partition: %" PRIu64 " sectors (%s), LBA %"
          PRIu64 "-%" PRIu64 "\n", g.last - max_end, sz,
          max_end + 1, g.last);
  gpt_free (&g);
  return 0;
}

/* ---- plan / apply ------------------------------------------------------ */

static uint64_t
parse_gib (const char *s)
{
  char *end;
  unsigned long long v;

  errno = 0;
  v = strtoull (s, &end, 10);
  if (errno || end == s || *end || s[0] == '-')
    die (1, "LINUX_GIB '%s' is not a whole number", s);
  if (v < MIN_LINUX_GIB)
    die (1, "LINUX_GIB %llu is below the minimum of %d", v, MIN_LINUX_GIB);
  /* Far beyond any phone, and keeps v * 2^30 well inside 64 bits.  */
  if (v > (UINT64_C (1) << 20))
    die (1, "LINUX_GIB %llu is absurd", v);
  return v;
}

static void
fill_random (uint8_t *p, size_t n)
{
  int fd = open ("/dev/urandom", O_RDONLY | O_CLOEXEC);

  if (fd < 0 || read (fd, p, n) != (ssize_t) n)
    die (1, "cannot read /dev/urandom");
  close (fd);
}

static void
tell_kernel (struct disk *d, int op, uint32_t pno, uint64_t start,
             uint64_t len, const char *what)
{
  struct blkpg_partition part;
  struct blkpg_ioctl_arg arg;

  memset (&part, 0, sizeof part);
  part.start = (long long) (start * d->ss);
  part.length = (long long) (len * d->ss);
  part.pno = (int) pno;
  memset (&arg, 0, sizeof arg);
  arg.op = op;
  arg.datalen = sizeof part;
  arg.data = &part;
  if (ioctl (d->fd, BLKPG, &arg) < 0)
    die (3, "GPT written, but BLKPG %s of partition %u failed: %s"
         " (reboot to see the new table)", what, pno, strerror (errno));
}

static int
cmd_plan (struct disk *d, const char *gib_arg, int apply)
{
  struct gpt g, b;
  uint64_t gib = parse_gib (gib_arg);
  uint64_t align = MIB / d->ss;             /* sectors per MiB */
  uint64_t lsects = gib * GIB / d->ss;      /* wanted linux size */
  uint64_t ud_start, ud_end, new_ud_end, lx_start, lx_end, top;
  int ud, lx, slot = -1, have_backup;
  const char *why;
  char sz1[32], sz2[32];

  load_primary (d, &g);

  /* A re-flash finds the partition from last time and reuses it.  */
  lx = find_named (&g, "linux");
  if (lx >= 0)
    {
      printf ("exists %d\n", lx + 1);
      gpt_free (&g);
      return 0;
    }

  ud = find_named (&g, "userdata");
  if (ud < 0)
    die (1, "no partition named 'userdata'");
  ud_start = entry_start (&g, (uint32_t) ud);
  ud_end = entry_end (&g, (uint32_t) ud);

  /* userdata must be last: no other partition may end at or after its
     start, which also rules out one hiding inside it.  */
  for (uint32_t i = 0; i < g.nent; i++)
    if (i != (uint32_t) ud && entry_used (&g, i)
        && entry_end (&g, i) >= ud_start)
      die (1, "partition %u ends at %" PRIu64 ", at or after userdata's"
           " start %" PRIu64 "; userdata is not the last partition",
           i + 1, entry_end (&g, i), ud_start);

  /* linux is the last LINUX_GIB of userdata as Samsung laid it out: the
     few MiB Samsung leaves free before the end of the disk stay free.  */
  top = ud_end + 1;
  if (lsects >= top)
    die (1, "%" PRIu64 " GiB does not fit on this disk", gib);
  new_ud_end = (top - lsects) / align * align - 1;
  lx_start = (new_ud_end + 1 + align - 1) / align * align;
  lx_end = ud_end;

  if (new_ud_end < ud_start
      || (new_ud_end - ud_start + 1) * d->ss < MIN_USERDATA)
    die (1, "userdata would be smaller than 16 GiB");
  /* The free space past userdata is already bigger than asked for:
     growing userdata over it is not this tool's business.  */
  if (new_ud_end > ud_end)
    die (1, "userdata would grow (end %" PRIu64 " -> %" PRIu64 ");"
         " refusing", ud_end, new_ud_end);
  if (lx_start > lx_end)
    die (1, "no room for linux after aligning");

  for (uint32_t i = 0; i < g.nent; i++)
    if (!entry_used (&g, i))
      {
        slot = (int) i;
        break;
      }
  if (slot < 0)
    die (1, "no empty slot in the %u-entry table", g.nent);

  /* Samsung has no backup; only a real one (header + same entries as
     the primary) is kept in step.  One that passes its own CRCs but
     disagrees with the primary means a disk we do not understand.  */
  why = gpt_load (d, g.alt, &b);
  have_backup = !why;
  if (have_backup)
    {
      if (b.alt != 1 || b.nent != g.nent || b.esz != g.esz
          || b.first != g.first || b.last != g.last
          || memcmp (b.ent, g.ent, g.ent_bytes) != 0)
        die (1, "backup GPT at LBA %" PRIu64 " is valid but does not match"
             " the primary", g.alt);
    }

  fmt_size ((new_ud_end - ud_start + 1) * d->ss, sz1);
  fmt_size ((lx_end - lx_start + 1) * d->ss, sz2);
  printf ("userdata (#%d): %" PRIu64 "-%" PRIu64 " -> %" PRIu64 "-%" PRIu64
          " (%s)\n", ud + 1, ud_start, ud_end, ud_start, new_ud_end, sz1);
  printf ("linux    (#%d): %" PRIu64 "-%" PRIu64 " (%s)\n",
          slot + 1, lx_start, lx_end, sz2);
  printf ("backup GPT: %s\n", have_backup ? "present, will be updated"
          : "none, disk end left alone");

  if (!apply)
    {
      if (have_backup)
        gpt_free (&b);
      gpt_free (&g);
      return 0;
    }

  /* ---- every check passed: build the new table in memory ---- */
  {
    uint8_t *e = entry (&g, (uint32_t) slot);
    uint8_t *ue = entry (&g, (uint32_t) ud);
    static const char nm[] = "linux";
    uint32_t ecrc;

    put64 (ue + 40, new_ud_end);

    memset (e, 0, g.esz);
    memcpy (e, linux_type, 16);
    fill_random (e + 16, 16);
    /* RFC 4122 version 4: the version nibble is the top of
       time_hi_and_version, a little-endian u16 at bytes 6-7.  */
    e[16 + 7] = (e[16 + 7] & 0x0f) | 0x40;
    e[16 + 8] = (e[16 + 8] & 0x3f) | 0x80;
    put64 (e + 32, lx_start);
    put64 (e + 40, lx_end);
    put64 (e + 48, 0);
    for (size_t k = 0; nm[k]; k++)
      {
        e[56 + 2 * k] = (uint8_t) nm[k];
        e[56 + 2 * k + 1] = 0;
      }

    ecrc = crc32 (g.ent, g.ent_bytes);
    put32 (g.hdr + 88, ecrc);
    put32 (g.hdr + 16, 0);
    put32 (g.hdr + 16, crc32 (g.hdr, g.hsz));

    if (have_backup)
      {
        memcpy (b.ent, g.ent, g.ent_bytes);
        put32 (b.hdr + 88, ecrc);
        put32 (b.hdr + 16, 0);
        put32 (b.hdr + 16, crc32 (b.hdr, b.hsz));
      }
  }

  /* Backup first, primary last: if we die in between, the primary --
     the one Samsung's bootloader reads -- is still the old, intact
     table.  Entries before header so a header never points at an array
     its CRC does not describe yet.  */
  if (have_backup)
    {
      write_at (d, b.ent_lba, b.ent, b.ent_sects * d->ss);
      write_at (d, b.lba, b.hdr, d->ss);
      if (fsync (d->fd) < 0)
        die (2, "fsync: %s", strerror (errno));
    }
  write_at (d, g.ent_lba, g.ent, g.ent_sects * d->ss);
  write_at (d, g.lba, g.hdr, d->ss);
  if (fsync (d->fd) < 0)
    die (2, "fsync: %s", strerror (errno));

  /* BLKRRPART would fail with partitions mounted; BLKPG edits the
     kernel's view one partition at a time.  Shrink first so the add
     does not overlap.  */
  if (d->is_blk)
    {
      tell_kernel (d, BLKPG_RESIZE_PARTITION, (uint32_t) ud + 1, ud_start,
                   new_ud_end - ud_start + 1, "resize");
      tell_kernel (d, BLKPG_ADD_PARTITION, (uint32_t) slot + 1, lx_start,
                   lx_end - lx_start + 1, "add");
    }

  printf ("created %d %" PRIu64 " %" PRIu64 "\n", slot + 1,
          lx_start * d->ss, (lx_end - lx_start + 1) * d->ss);
  if (have_backup)
    gpt_free (&b);
  gpt_free (&g);
  return 0;
}

/* ---- main -------------------------------------------------------------- */

static void __attribute__ ((noreturn))
usage (void)
{
  fputs ("usage: s10e-part [--sector-size N] show DISK\n"
         "       s10e-part [--sector-size N] plan DISK LINUX_GIB\n"
         "       s10e-part [--sector-size N] apply DISK LINUX_GIB\n"
         "--sector-size only applies to image files (default 4096);\n"
         "a block device's own size is always used.\n", stderr);
  exit (1);
}

int
main (int argc, char **argv)
{
  struct disk d;
  uint32_t ss_opt = 0;
  int a = 1;

  setvbuf (stdout, NULL, _IOLBF, 0);
  if (a + 1 < argc && strcmp (argv[a], "--sector-size") == 0)
    {
      char *end;
      unsigned long v = strtoul (argv[a + 1], &end, 10);

      if (*end || v == 0 || v > 65536)
        die (1, "bad --sector-size '%s'", argv[a + 1]);
      ss_opt = (uint32_t) v;
      a += 2;
    }
  if (argc - a < 2)
    usage ();

  memset (&d, 0, sizeof d);
  if (strcmp (argv[a], "show") == 0 && argc - a == 2)
    {
      disk_open (&d, argv[a + 1], 0, ss_opt);
      return cmd_show (&d);
    }
  if (strcmp (argv[a], "plan") == 0 && argc - a == 3)
    {
      disk_open (&d, argv[a + 1], 0, ss_opt);
      return cmd_plan (&d, argv[a + 2], 0);
    }
  if (strcmp (argv[a], "apply") == 0 && argc - a == 3)
    {
      disk_open (&d, argv[a + 1], 1, ss_opt);
      return cmd_plan (&d, argv[a + 2], 1);
    }
  usage ();
}
