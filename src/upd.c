/* Official firmware update files (.UPD) on the stick, run live: nothing is installed.
 *
 * A .UPD is an iso9660 image encrypted with AES-256-CBC, each 512-byte sector on its own (IV = little-endian sector
 * number + 12 zero bytes), followed by a 20-byte trailer: "LIVESAMPLER", the version ("1.60\0"), a CRC32 of the
 * ciphertext (PioneerSP16FirmwareResearch/docs/firmware-format.md). It is the same cipher as autoexec.bin, so the key
 * is the unit's own: the file its USB autoexec hook uses (BM_UPD_KEY). The key is read when needed and is never
 * stored, logged or written anywhere.
 *
 * upd_probe reads only the trailer, so the menu lists a 95 MB update file and its version from 20 bytes.
 * upd_extract finds one file in the image and decrypts only the sectors it covers (images/pdj.tar.gz, which holds
 * EP085_Main_R, is about 5 % of an update). The AES is table-driven (4 KB of tables, built once): about 25 cycles a
 * byte on the A9, so the decryption costs less than reading the stick. */
#include "bm.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define SECTOR 512
#define BLOCK 2048            /* iso9660 logical block */
#define TRAILER 20
#define MARKER "LIVESAMPLER"
#define MARKER_LEN 11

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- trailer -------------------------------------------------------------------------------------------------- */
int upd_probe(const char *path, char *version, size_t vlen) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    uint8_t t[TRAILER];
    int ok = fstat(fd, &st) == 0 && st.st_size >= 32768 + BLOCK + TRAILER && (st.st_size - TRAILER) % SECTOR == 0 &&
             pread(fd, t, TRAILER, st.st_size - TRAILER) == TRAILER && !memcmp(t, MARKER, MARKER_LEN);
    close(fd);
    if (!ok) return 0;
    /* bytes 11..15: the version, NUL-terminated ("1.60\0") */
    size_t k = 0;
    while (k < 5 && t[MARKER_LEN + k] && (isdigit(t[MARKER_LEN + k]) || t[MARKER_LEN + k] == '.')) k++;
    if (k == 0 || (k < 5 && t[MARKER_LEN + k])) return 0;
    snprintf(version, vlen, "%.*s", (int)k, (const char *)t + MARKER_LEN);
    return 1;
}

/* ---- AES-256 decryption (FIPS-197, equivalent inverse cipher) ------------------------------------------------- */
static uint8_t SB[256], ISB[256];
static uint32_t TD[4][256]; /* InvSubBytes + InvMixColumns of one byte in row 0..3, column words little-endian */

static uint8_t xtime(uint8_t x) { return (uint8_t)(x << 1 ^ (x >> 7) * 0x1b); }
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (; b; b >>= 1, a = xtime(a))
        if (b & 1) p ^= a;
    return p;
}
static uint8_t rotl8(uint8_t x, int s) { return (uint8_t)(x << s | x >> (8 - s)); }
static uint32_t rotl32(uint32_t x, int s) { return x << s | x >> (32 - s); }

static void aes_tables(void) {
    static int ready;
    if (ready) return;
    uint8_t p = 1, q = 1;
    do { /* p walks the multiplicative group (times 3), q is its inverse (divided by 3) */
        p = (uint8_t)(p ^ xtime(p));
        q ^= (uint8_t)(q << 1);
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        SB[p] = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    SB[0] = 0x63;
    for (int i = 0; i < 256; i++) ISB[SB[i]] = (uint8_t)i;
    for (int i = 0; i < 256; i++) {
        uint8_t s = ISB[i];
        uint32_t w = (uint32_t)gmul(s, 14) | (uint32_t)gmul(s, 9) << 8 | (uint32_t)gmul(s, 13) << 16 |
                     (uint32_t)gmul(s, 11) << 24;
        for (int r = 0; r < 4; r++) TD[r][i] = rotl32(w, 8 * r);
    }
    ready = 1;
}

typedef struct {
    uint32_t dk[60]; /* decryption round keys, round 0 first */
} Aes;

static uint32_t sub_word(uint32_t w) {
    return (uint32_t)SB[w & 255] | (uint32_t)SB[w >> 8 & 255] << 8 | (uint32_t)SB[w >> 16 & 255] << 16 |
           (uint32_t)SB[w >> 24] << 24;
}

static void aes_init(Aes *a, const uint8_t key[32]) {
    aes_tables();
    uint32_t ek[60];
    for (int i = 0; i < 8; i++) ek[i] = le32(key + 4 * i);
    uint8_t rcon = 1;
    for (int i = 8; i < 60; i++) {
        uint32_t t = ek[i - 1];
        if (i % 8 == 0) {
            t = sub_word(t >> 8 | t << 24) ^ rcon;
            rcon = xtime(rcon);
        } else if (i % 8 == 4) {
            t = sub_word(t);
        }
        ek[i] = ek[i - 8] ^ t;
    }
    /* reversed order; the middle rounds through InvMixColumns (TD of SubBytes = InvMixColumns alone) */
    for (int r = 0; r <= 14; r++)
        for (int c = 0; c < 4; c++) {
            uint32_t w = ek[4 * (14 - r) + c];
            if (r > 0 && r < 14)
                w = TD[0][SB[w & 255]] ^ TD[1][SB[w >> 8 & 255]] ^ TD[2][SB[w >> 16 & 255]] ^ TD[3][SB[w >> 24]];
            a->dk[4 * r + c] = w;
        }
    memset(ek, 0, sizeof ek);
}

static void aes_decrypt_block(const Aes *a, const uint8_t in[16], uint8_t out[16]) {
    const uint32_t *k = a->dk;
    uint32_t s0 = le32(in) ^ k[0], s1 = le32(in + 4) ^ k[1], s2 = le32(in + 8) ^ k[2], s3 = le32(in + 12) ^ k[3];
    for (int r = 1; r < 14; r++) {
        k += 4;
        /* InvShiftRows: row r of column c comes from column c - r */
        uint32_t t0 = TD[0][s0 & 255] ^ TD[1][s3 >> 8 & 255] ^ TD[2][s2 >> 16 & 255] ^ TD[3][s1 >> 24] ^ k[0];
        uint32_t t1 = TD[0][s1 & 255] ^ TD[1][s0 >> 8 & 255] ^ TD[2][s3 >> 16 & 255] ^ TD[3][s2 >> 24] ^ k[1];
        uint32_t t2 = TD[0][s2 & 255] ^ TD[1][s1 >> 8 & 255] ^ TD[2][s0 >> 16 & 255] ^ TD[3][s3 >> 24] ^ k[2];
        uint32_t t3 = TD[0][s3 & 255] ^ TD[1][s2 >> 8 & 255] ^ TD[2][s1 >> 16 & 255] ^ TD[3][s0 >> 24] ^ k[3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    k += 4;
    const uint32_t s[4] = {s0, s1, s2, s3};
    for (int c = 0; c < 4; c++) {
        uint32_t w = (uint32_t)ISB[s[c] & 255] | (uint32_t)ISB[s[(c + 3) & 3] >> 8 & 255] << 8 |
                     (uint32_t)ISB[s[(c + 2) & 3] >> 16 & 255] << 16 | (uint32_t)ISB[s[(c + 1) & 3] >> 24] << 24;
        w ^= k[c];
        out[4 * c] = (uint8_t)w, out[4 * c + 1] = (uint8_t)(w >> 8), out[4 * c + 2] = (uint8_t)(w >> 16),
        out[4 * c + 3] = (uint8_t)(w >> 24);
    }
}

/* Decrypts whole sectors in place; `first` is the index of the first one in the file. */
static void decrypt_sectors(const Aes *a, uint8_t *buf, size_t n_sectors, uint32_t first) {
    for (size_t s = 0; s < n_sectors; s++) {
        uint8_t iv[16] = {0}, c[16], *p = buf + s * SECTOR;
        uint32_t idx = first + (uint32_t)s;
        iv[0] = (uint8_t)idx, iv[1] = (uint8_t)(idx >> 8), iv[2] = (uint8_t)(idx >> 16), iv[3] = (uint8_t)(idx >> 24);
        for (int b = 0; b < SECTOR; b += 16) {
            memcpy(c, p + b, 16);
            aes_decrypt_block(a, c, p + b);
            for (int i = 0; i < 16; i++) p[b + i] ^= iv[i];
            memcpy(iv, c, 16);
        }
    }
}

/* ---- the image ------------------------------------------------------------------------------------------------ */
typedef struct {
    int fd;
    off_t size; /* ciphertext bytes (the file without its trailer) */
    Aes aes;
} Upd;

/* Reads and decrypts [off, off + len) of the plain image; off and len are multiples of SECTOR. */
static int upd_read(Upd *u, off_t off, uint8_t *buf, size_t len) {
    if (off < 0 || off + (off_t)len > u->size) return -1;
    size_t got = 0;
    while (got < len) {
        ssize_t r = pread(u->fd, buf + got, len - got, off + (off_t)got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    decrypt_sectors(&u->aes, buf, len / SECTOR, (uint32_t)(off / SECTOR));
    return 0;
}

/* The name of a directory record: the Rock Ridge NM name when there is one, else the iso9660 name without ";1" and
 * a trailing dot. */
static void record_name(const uint8_t *rec, char *out, size_t n) {
    unsigned len = rec[0], nlen = rec[32];
    size_t k = 0;
    unsigned su = 33 + nlen + !(nlen & 1);
    int rr = 0;
    while (su + 4 <= len) {
        const uint8_t *e = rec + su;
        unsigned elen = e[2];
        if (elen < 4 || su + elen > len) break;
        if (e[0] == 'N' && e[1] == 'M' && elen > 5) {
            if (!rr) k = 0, rr = 1;
            for (unsigned i = 5; i < elen && k + 1 < n; i++) out[k++] = (char)e[i];
        }
        su += elen;
    }
    if (!rr) {
        for (unsigned i = 0; i < nlen && k + 1 < n && rec[33 + i] != ';'; i++) out[k++] = (char)rec[33 + i];
        if (k && out[k - 1] == '.') k--;
    }
    out[k] = 0;
}

/* Finds `name` in the directory at (extent, size); fills the entry's extent and size. 0 = found. */
static int dir_find(Upd *u, uint32_t extent, uint32_t size, const char *name, size_t name_len, uint32_t *f_extent,
                    uint32_t *f_size, int *is_dir) {
    uint8_t blk[BLOCK];
    for (uint32_t done = 0; done < size; done += BLOCK) {
        if (upd_read(u, (off_t)(extent + done / BLOCK) * BLOCK, blk, BLOCK) != 0) return -1;
        for (unsigned pos = 0; pos + 34 <= BLOCK;) {
            const uint8_t *rec = blk + pos;
            if (rec[0] == 0) break; /* the rest of this block is padding */
            if (rec[0] < 34 || pos + rec[0] > BLOCK) return -1;
            if (!(rec[32] == 1 && rec[33] <= 1)) { /* not . or .. */
                char n[256];
                record_name(rec, n, sizeof n);
                if (strlen(n) == name_len && !strncasecmp(n, name, name_len)) {
                    *f_extent = le32(rec + 2), *f_size = le32(rec + 10), *is_dir = (rec[25] & 2) != 0;
                    return 0;
                }
            }
            pos += rec[0];
        }
    }
    return -1;
}

static int upd_read_key(const char *path, uint8_t key[32]) {
    uint8_t raw[64];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, raw, sizeof raw);
    close(fd);
    if (r <= 0) return -1;
    /* the first line (up to LF or NUL), at most 31 bytes, zero-padded to 32: as the unit's losetup reads it */
    size_t k = 0;
    while (k < (size_t)r && k < 31 && raw[k] != '\n' && raw[k] != 0) k++;
    if (k && raw[k - 1] == '\r') k--;
    memset(key, 0, 32);
    memcpy(key, raw, k);
    memset(raw, 0, sizeof raw);
    return k ? 0 : -1;
}

int upd_extract(const char *upd_path, const char *key_path, const char *member, int out_fd) {
    static Upd u; /* the round keys stay off the stack */
    uint8_t key[32];
    if (upd_read_key(key_path, key) != 0) {
        bm_log("upd: cannot read the unit key %s", key_path);
        return -1;
    }
    aes_init(&u.aes, key);
    memset(key, 0, sizeof key);
    int rc = -1;
    struct stat st;
    u.fd = open(upd_path, O_RDONLY | O_CLOEXEC);
    if (u.fd < 0 || fstat(u.fd, &st) != 0) {
        bm_log("upd: cannot open %s: %s", upd_path, strerror(errno));
        goto out;
    }
    u.size = (st.st_size - TRAILER) / SECTOR * SECTOR;

    uint8_t pvd[BLOCK];
    if (upd_read(&u, 16 * BLOCK, pvd, BLOCK) != 0 || memcmp(pvd, "\001CD001\001", 7) != 0) {
        bm_log("upd: %s does not decrypt to an iso9660 image (another key, or not an SP-16 update)", upd_path);
        goto out;
    }
    if ((pvd[128] | pvd[129] << 8) != BLOCK) {
        bm_log("upd: unexpected iso9660 block size");
        goto out;
    }
    uint32_t extent = le32(pvd + 156 + 2), size = le32(pvd + 156 + 10);
    int is_dir = 1;
    for (const char *p = member; *p;) {
        while (*p == '/') p++;
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (!len) break;
        if (!is_dir || dir_find(&u, extent, size, p, len, &extent, &size, &is_dir) != 0) {
            bm_log("upd: %s: no %s in the image", upd_path, member);
            goto out;
        }
        p += len;
    }
    if (is_dir) {
        bm_log("upd: %s is a folder", member);
        goto out;
    }
    /* stream it out, 64 KB at a time */
    static uint8_t buf[64 * 1024];
    off_t pos = (off_t)extent * BLOCK, end = pos + size;
    while (pos < end) {
        size_t want = (size_t)(end - pos) < sizeof buf ? (size_t)(end - pos) : sizeof buf;
        size_t aligned = (want + SECTOR - 1) / SECTOR * SECTOR;
        if (upd_read(&u, pos, buf, aligned) != 0) {
            bm_log("upd: read error at %lld", (long long)pos);
            goto out;
        }
        for (size_t w = 0; w < want;) {
            ssize_t r = write(out_fd, buf + w, want - w);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) {
                bm_log("upd: write failed: %s", strerror(errno));
                goto out;
            }
            w += (size_t)r;
        }
        pos += (off_t)want;
    }
    bm_log("upd: %s: %s, %u bytes", upd_path, member, size);
    rc = 0;
out:
    memset(&u.aes, 0, sizeof u.aes);
    if (u.fd >= 0) close(u.fd);
    u.fd = -1;
    return rc;
}
