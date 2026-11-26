/*
 * r8s-sparse: tiny static helper run in TWRP for the Android backup.
 * No libc (raw syscalls), so one file builds for aarch64 (phone) and x86_64
 * (self-test on the build machine):
 *
 *   clang --target=aarch64-linux-gnu -O2 -ffreestanding -fno-stack-protector \
 *         -nostdlib -static -fuse-ld=lld -o r8s-sparse r8s-sparse.c
 *
 *   r8s-sparse trim <mountpoint>   FITRIM: tell the storage which blocks are free
 *   r8s-sparse digest <device>     read the device, print: chunks nonzero_bytes digest
 *   r8s-sparse dump <device>       stream only the non-zero 1 MiB chunks to stdout
 *   r8s-sparse restore <device>    write a dump from stdin back (discard first,
 *                                  or zero-fill gaps if discard doesn't read back 0)
 *
 * Stream: header  "R8SSPRS1" u64 total u32 chunk u32 0 u64 0
 *         record  u64 offset u32 len u32 crc32 data[len]   (non-zero chunks only)
 *         trailer u64 ~0 u32 0 u32 0 u64 chunks u64 digest u64 nonzero_bytes
 * digest = FNV-1a 64 over total, then (offset, crc32) of every non-zero chunk.
 * The device digest after restore must equal the trailer's: that proves every
 * data chunk and every zero gap.
 */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;
typedef long s64;

#if defined(__aarch64__)
enum { SYS_ioctl = 29, SYS_openat = 56, SYS_close = 57, SYS_lseek = 62, SYS_read = 63,
       SYS_write = 64, SYS_pread = 67, SYS_pwrite = 68, SYS_fsync = 82, SYS_exit = 94 };
static s64 sc(s64 n, s64 a, s64 b, s64 c, s64 d) {
    register s64 x8 __asm__("x8") = n, x0 __asm__("x0") = a, x1 __asm__("x1") = b,
                 x2 __asm__("x2") = c, x3 __asm__("x3") = d;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}
__asm__(".global _start\n_start:\n mov x0, sp\n bl cmain\n");
#elif defined(__x86_64__)
enum { SYS_read = 0, SYS_write = 1, SYS_close = 3, SYS_lseek = 8, SYS_ioctl = 16, SYS_pread = 17,
       SYS_pwrite = 18, SYS_fsync = 74, SYS_exit = 231, SYS_openat = 257 };
static s64 sc(s64 n, s64 a, s64 b, s64 c, s64 d) {
    s64 r;
    register s64 r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return r;
}
__asm__(".global _start\n_start:\n mov %rsp, %rdi\n and $-16, %rsp\n call cmain\n");
#else
#error unsupported arch
#endif

void *memset(void *d, int c, unsigned long n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }

#define CHUNK (1u << 20)
#define BLKGETSIZE64 0x80081272UL
#define BLKDISCARD   0x1277UL
#define FITRIM       0xC0185879UL
static u8 buf[CHUNK] __attribute__((aligned(4096)));
static u8 zbuf[CHUNK] __attribute__((aligned(4096)));
static u32 crctab[256];

static void die_exit(int code) { sc(SYS_exit, code, 0, 0, 0); for (;;) {} }
static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void out(int fd, const char *s) { sc(SYS_write, fd, (s64)s, (s64)slen(s), 0); }
static void outnum(int fd, u64 v) {
    char t[24]; int i = 23; t[i] = 0;
    do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    out(fd, t + i);
}
static void outhex(int fd, u64 v) {
    char t[17]; for (int i = 15; i >= 0; i--) { t[i] = "0123456789abcdef"[v & 15]; v >>= 4; } t[16] = 0; out(fd, t);
}
static void fail(const char *msg, s64 err) {
    out(2, "r8s-sparse: "); out(2, msg);
    if (err) { out(2, " (errno "); outnum(2, (u64)-err); out(2, ")"); }
    out(2, "\n"); die_exit(1);
}
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void crc_init(void) {
    for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; crctab[i] = c; }
}
static u32 crc32(const u8 *p, u64 n) {
    u32 c = 0xFFFFFFFFu; while (n--) c = crctab[(c ^ *p++) & 255] ^ (c >> 8); return c ^ 0xFFFFFFFFu;
}
static u64 fnv(u64 h, u64 v, int bytes) { for (int i = 0; i < bytes; i++) { h ^= (v >> (8 * i)) & 255; h *= 0x100000001b3UL; } return h; }
static int allzero(const u8 *p, u64 n) {
    const u64 *q = (const u64 *)p; u64 w = n / 8, acc = 0;
    for (u64 i = 0; i < w; i++) { acc |= q[i]; if ((i & 1023) == 1023 && acc) return 0; }
    for (u64 i = w * 8; i < n; i++) acc |= p[i];
    return acc == 0;
}

static void wr_all(int fd, const void *p, u64 n) {
    const u8 *q = p;
    while (n) { s64 r = sc(SYS_write, fd, (s64)q, (s64)n, 0); if (r == -4) continue; if (r <= 0) fail("write failed", r); q += r; n -= (u64)r; }
}
static void rd_all(int fd, void *p, u64 n) {
    u8 *q = p;
    while (n) { s64 r = sc(SYS_read, fd, (s64)q, (s64)n, 0); if (r == -4) continue; if (r < 0) fail("read failed", r); if (r == 0) fail("input ended early (stream cut off)", 0); q += r; n -= (u64)r; }
}
static void pread_all(int fd, void *p, u64 n, u64 off) {
    u8 *q = p;
    while (n) { s64 r = sc(SYS_pread, fd, (s64)q, (s64)n, (s64)off); if (r == -4) continue; if (r <= 0) fail("device read failed", r); q += r; n -= (u64)r; off += (u64)r; }
}
static void pwrite_all(int fd, const void *p, u64 n, u64 off) {
    const u8 *q = p;
    while (n) { s64 r = sc(SYS_pwrite, fd, (s64)q, (s64)n, (s64)off); if (r == -4) continue; if (r <= 0) fail("device write failed", r); q += r; n -= (u64)r; off += (u64)r; }
}
static void put64(u8 *p, u64 v) { for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (8 * i)); }
static void put32(u8 *p, u32 v) { for (int i = 0; i < 4; i++) p[i] = (u8)(v >> (8 * i)); }
static u64 get64(const u8 *p) { u64 v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static u32 get32(const u8 *p) { u32 v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }

static int open_dev(const char *path, int flags) {
    s64 fd = sc(SYS_openat, -100, (s64)path, flags, 0);
    if (fd < 0) fail("can't open device", fd);
    return (int)fd;
}
static u64 dev_size(int fd) {
    u64 sz = 0;
    if (sc(SYS_ioctl, fd, (s64)BLKGETSIZE64, (s64)&sz, 0) == 0) return sz;
    s64 r = sc(SYS_lseek, fd, 0, 2, 0);          /* regular file (self-test) */
    if (r < 0) fail("can't get device size", r);
    return (u64)r;
}

/* Walk the device; for each non-zero chunk call back (or just count). */
static void walk(int fd, u64 total, int to_stdout, u64 *chunks, u64 *nz, u64 *dig) {
    u64 h = fnv(0xcbf29ce484222325UL, total, 8), n = 0, bytes = 0;
    u8 rec[16];
    for (u64 off = 0; off < total; off += CHUNK) {
        u64 len = total - off < CHUNK ? total - off : CHUNK;
        pread_all(fd, buf, len, off);
        if (allzero(buf, len)) continue;
        u32 c = crc32(buf, len);
        h = fnv(fnv(h, off, 8), c, 4); n++; bytes += len;
        if (to_stdout) { put64(rec, off); put32(rec + 8, (u32)len); put32(rec + 12, c); wr_all(1, rec, 16); wr_all(1, buf, len); }
    }
    *chunks = n; *nz = bytes; *dig = h;
}

static int cmd_trim(const char *mnt) {
    s64 fd = sc(SYS_openat, -100, (s64)mnt, 0, 0);
    if (fd < 0) fail("can't open mount point", fd);
    u64 range[3] = { 0, ~0UL, 0 };
    s64 r = sc(SYS_ioctl, fd, (s64)FITRIM, (s64)range, 0);
    if (r < 0) fail("FITRIM not supported here", r);
    out(1, "trimmed "); outnum(1, range[1]); out(1, "\n");
    return 0;
}

static int cmd_digest(const char *dev) {
    int fd = open_dev(dev, 0);
    u64 total = dev_size(fd), n, nz, h;
    walk(fd, total, 0, &n, &nz, &h);
    outnum(1, n); out(1, " "); outnum(1, nz); out(1, " "); outhex(1, h); out(1, " "); outnum(1, total); out(1, "\n");
    return 0;
}

static int cmd_dump(const char *dev) {
    int fd = open_dev(dev, 0);
    u64 total = dev_size(fd), n, nz, h;
    u8 hdr[32] = { 'R','8','S','S','P','R','S','1' };
    put64(hdr + 8, total); put32(hdr + 16, CHUNK);
    wr_all(1, hdr, 32);
    walk(fd, total, 1, &n, &nz, &h);
    u8 tr[40]; memset(tr, 0, sizeof tr);
    put64(tr, ~0UL); put64(tr + 16, n); put64(tr + 24, h); put64(tr + 32, nz);
    wr_all(1, tr, 40);
    out(2, "chunks "); outnum(2, n); out(2, " nonzero "); outnum(2, nz); out(2, " digest "); outhex(2, h); out(2, "\n");
    return 0;
}

static int cmd_restore(const char *dev) {
    u8 hdr[32], rec[16];
    rd_all(0, hdr, 32);
    if (!(hdr[0] == 'R' && hdr[1] == '8' && hdr[6] == 'S' && hdr[7] == '1')) fail("input is not an r8s-sparse stream", 0);
    u64 total = get64(hdr + 8);
    if (get32(hdr + 16) != CHUNK) fail("unexpected chunk size", 0);
    int fd = open_dev(dev, 2);
    if (dev_size(fd) != total) fail("device size differs from the backup", 0);

    /* Discard everything, then check it reads back as zeros. If not (or no
       discard, e.g. a file), write zeros into the gaps ourselves. */
    int zerofill = 1;
    u64 range[2] = { 0, total };
    if (sc(SYS_ioctl, fd, (s64)BLKDISCARD, (s64)range, 0) == 0) {
        zerofill = 0;
        for (int i = 0; i <= 8 && !zerofill; i++) {
            u64 off = (total / 8 * (u64)i) & ~(u64)(CHUNK - 1);
            if (off >= total) off = (total - 1) & ~(u64)(CHUNK - 1);
            u64 len = total - off < CHUNK ? total - off : CHUNK;
            pread_all(fd, buf, len, off);
            if (!allzero(buf, len)) zerofill = 1;
        }
    }
    out(2, zerofill ? "mode: zero-fill gaps\n" : "mode: discard\n");

    u64 next = 0, h = fnv(0xcbf29ce484222325UL, total, 8), n = 0;
    for (;;) {
        rd_all(0, rec, 16);
        u64 off = get64(rec); u32 len = get32(rec + 8), c = get32(rec + 12);
        if (off == ~0UL) break;
        if (len == 0 || len > CHUNK || off % CHUNK || off < next || off + len > total) fail("corrupt record in stream", 0);
        rd_all(0, buf, len);
        if (crc32(buf, len) != c) fail("chunk checksum mismatch (backup file damaged?)", 0);
        if (zerofill) for (; next < off; next += CHUNK) pwrite_all(fd, zbuf, CHUNK, next);
        pwrite_all(fd, buf, len, off);
        next = off + len; n++;
        h = fnv(fnv(h, off, 8), c, 4);
    }
    u8 tr[24]; rd_all(0, tr, 24);
    if (zerofill) for (; next < total; ) { u64 l = total - next < CHUNK ? total - next : CHUNK; pwrite_all(fd, zbuf, l, next); next += l; }
    sc(SYS_fsync, fd, 0, 0, 0);
    if (get64(tr) != n || get64(tr + 8) != h) fail("stream trailer doesn't match its records", 0);
    out(2, "restored chunks "); outnum(2, n); out(2, " digest "); outhex(2, h); out(2, "\n");
    return 0;
}

void cmain(long *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    crc_init();
    int rc = 2;
    if (argc == 3 && streq(argv[1], "trim")) rc = cmd_trim(argv[2]);
    else if (argc == 3 && streq(argv[1], "digest")) rc = cmd_digest(argv[2]);
    else if (argc == 3 && streq(argv[1], "dump")) rc = cmd_dump(argv[2]);
    else if (argc == 3 && streq(argv[1], "restore")) rc = cmd_restore(argv[2]);
    else out(2, "usage: r8s-sparse trim <mnt> | digest <dev> | dump <dev> | restore <dev>\n");
    die_exit(rc);
}
