/* The LCD backlight. The official application's screen saver dims it (IScreenSaverManager writes level 10 to
 * /dev/backlight) and only the official application itself brings it back, on its next touch or at its next start.
 * Stopped while the screen saver is on, it leaves the LCD dark for the menu and for every app started after it, so the
 * menu restores it once the official application is gone, the way the official application does when it starts:
 *
 *   LCD BRIGHTNESS (UTILITY, setting id 5, 1..5, default 3) -> 0x13, 0x2c, 0x5b, 0x9e, 0xf8
 *   ioctl(open("/dev/backlight", O_RDWR), 0x40047000, &level)      level: int, 0..255 (the kernel's own driver)
 *
 * The setting comes from the official application's settings file: an 8-byte header (magic 0x2a7b9e8f, version), then
 * 0x8c-byte records, int32 id at +0 and int32 value at +0x88, little-endian (EP085_Main_R 1.60: setter FUN_00562be8,
 * screen saver FUN_00563e9c, start-up FUN_00566b80, loader FUN_0056824c). A missing file or an out-of-range value
 * means 3, as there. */
#include "bm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BL_IOC_SET 0x40047000UL /* _IOW('p', 0, int) */
#define SETTINGS_MAGIC 0x2a7b9e8fu
#define SETTINGS_RECORD 0x8c
#define SETTING_LCD_BRIGHTNESS 5

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int backlight_setting(const char *settings) {
    int step = 3;
    FILE *f = fopen(settings, "rb");
    if (!f) return step;
    uint8_t rec[SETTINGS_RECORD];
    if (fread(rec, 1, 8, f) == 8 && le32(rec) == SETTINGS_MAGIC)
        while (fread(rec, 1, sizeof rec, f) == sizeof rec)
            if ((int32_t)le32(rec) == SETTING_LCD_BRIGHTNESS) {
                int32_t v = (int32_t)le32(rec + 0x88);
                if (v >= 1 && v <= 5) step = v;
                break;
            }
    fclose(f);
    return step;
}

int backlight_level(int step) {
    static const int level[] = {0x13, 0x2c, 0x5b, 0x9e, 0xf8};
    return step >= 1 && step <= 5 ? level[step - 1] : level[2];
}

int backlight_wake(const char *settings, char *info, size_t infolen) {
    int step = backlight_setting(settings), level = backlight_level(step);
    int fd = open(BM_BACKLIGHT, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        snprintf(info, infolen, "backlight: LCD BRIGHTNESS %d -> level %d, but no " BM_BACKLIGHT " (%s)", step, level,
                 strerror(errno));
        return -1;
    }
    int rc = ioctl(fd, BL_IOC_SET, &level);
    int err = errno;
    close(fd);
    if (rc != 0) {
        snprintf(info, infolen, "backlight: level %d FAILED: %s", level, strerror(err));
        return -1;
    }
    snprintf(info, infolen, "backlight: LCD BRIGHTNESS %d -> level %d", step, level);
    return 0;
}
