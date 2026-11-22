/* Encrypted programs on the stick: the cipher of the unit's USB autoexec hook (the one autoexec.bin uses), so the unit's
 * own key opens them and nothing else is needed on the stick.
 *
 * An encrypted program is the executable, zero-padded to a multiple of 512 bytes, encrypted with AES-256-CBC, each
 * 512-byte sector on its own (IV = little-endian sector number + 12 zero bytes). There is no header: it is recognised
 * by its first sector decrypting to an ELF header (tools/encrypt-app.py makes one). The trailing zeros are never
 * loaded (the program headers say what is).
 *
 * The key is the first line of the unit's key file (BM_KEY), at most 31 bytes, zero-padded to 32. It is read once,
 * kept only as round keys in memory, and never logged or written anywhere.
 *
 * Reading the headers of an encrypted program decrypts only the sectors they are in; running one decrypts it whole into
 * RAM (bootmenu --decrypt, used by the launcher). The AES is table-driven (4 KB of tables, built once): about 25 cycles
 * a byte on the A9, so a 12 MB program decrypts in well under a second, faster than the stick reads it. */
#include "bm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SECTOR 512

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

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

/* ---- the key ------------------------------------------------------------------------------------------------- */
static Aes key_aes;
static int key_state; /* 0 = not tried, 1 = loaded, -1 = not available */

int crypt_key_load(const char *path) {
    if (key_state) return key_state > 0;
    uint8_t raw[64], key[32];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t r = fd >= 0 ? read(fd, raw, sizeof raw) : -1;
    if (fd >= 0) close(fd);
    size_t k = 0;
    while (r > 0 && k < (size_t)r && k < 31 && raw[k] != '\n' && raw[k] != 0) k++;
    if (k && raw[k - 1] == '\r') k--;
    if (k == 0) {
        key_state = -1;
        bm_log("crypt: no key at %s: encrypted programs are not listed", path);
        return 0;
    }
    memset(key, 0, sizeof key);
    memcpy(key, raw, k);
    aes_init(&key_aes, key);
    memset(key, 0, sizeof key);
    memset(raw, 0, sizeof raw);
    key_state = 1;
    return 1;
}

/* ---- reading ------------------------------------------------------------------------------------------------- */
ssize_t crypt_pread(int fd, void *dst, size_t len, off_t off) {
    if (key_state <= 0 || off < 0) return -1;
    uint8_t stack[4096], *buf = stack;
    off_t first = off / SECTOR * SECTOR;
    size_t span = (size_t)((off + (off_t)len + SECTOR - 1) / SECTOR * SECTOR - first);
    if (span > sizeof stack && !(buf = malloc(span))) return -1;
    size_t got = 0;
    while (got < span) {
        ssize_t r = pread(fd, buf + got, span - got, first + (off_t)got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    got = got / SECTOR * SECTOR; /* an encrypted file is whole sectors; a partial one is not part of it */
    decrypt_sectors(&key_aes, buf, got / SECTOR, (uint32_t)(first / SECTOR));
    ssize_t out = 0;
    if ((off_t)got > off - first) {
        out = (ssize_t)(got - (size_t)(off - first));
        if ((size_t)out > len) out = (ssize_t)len;
        memcpy(dst, buf + (off - first), (size_t)out);
    }
    if (buf != stack) free(buf);
    return out;
}

int crypt_decrypt_file(const char *path, int out_fd) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        bm_log("crypt: cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    static uint8_t buf[64 * 1024];
    off_t pos = 0;
    int rc = 0;
    for (;;) {
        ssize_t r = crypt_pread(fd, buf, sizeof buf, pos);
        if (r < 0) { rc = -1; break; }
        if (r == 0) break;
        for (ssize_t w = 0; w < r;) {
            ssize_t n = write(out_fd, buf + w, (size_t)(r - w));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                bm_log("crypt: write failed: %s", strerror(errno));
                close(fd);
                return -1;
            }
            w += n;
        }
        pos += r;
    }
    close(fd);
    if (rc == 0) bm_log("crypt: %s: %lld bytes decrypted", path, (long long)pos);
    return rc;
}
