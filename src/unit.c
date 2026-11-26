/* Which unit the menu runs on: the TORAIZ SP-16 or the DJS-1000.
 *
 * Both run the same board, screen, GPU and autoexec hook (with the same key), so one autoexec.bin serves both. The
 * unit is the one whose official application is installed (/root/pdj/EP113_Main_R is the DJS-1000's,
 * /root/pdj/EP085_Main_R the SP-16's). --unit or BOOTMENU_UNIT (sp16, djs1000) overrides that, for tests and for a
 * development PC. With neither application installed the menu takes the SP-16, as before the DJS-1000 was known.
 *
 * The release tables: the official application's size in each release. The sizes are all different (across both
 * units too), so one stat() tells the installed version, and the model of a copy on the stick. */
#include "bm.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* PioneerSP16FirmwareResearch/docs/firmware-reference.md, "Application binary identities" */
static const Release SP16_RELEASES[] = {
    {10724172, "1.20"}, {11172172, "1.30"}, {11171172, "1.31"},
    {11676132, "1.40"}, {11993612, "1.50"}, {12000396, "1.60"},
};
/* measured on the DJS-1000's 1.22 update (earlier releases are not listed yet: they show as an unknown version) */
static const Release DJS1000_RELEASES[] = {
    {12225388, "1.22"},
};

#define N(a) (int)(sizeof a / sizeof a[0])
static const Unit UNITS[] = {
    {UNIT_SP16, "sp16", "TORAIZ SP-16", "EP085_Main_R", "/root/pdj/EP085_Main_R", "Pioneer DJ - Toraiz SP-16",
     SP16_RELEASES, N(SP16_RELEASES), 0},
    {UNIT_DJS1000, "djs1000", "DJS-1000", "EP113_Main_R", "/root/pdj/EP113_Main_R", "Pioneer DJ - DJS-1000",
     DJS1000_RELEASES, N(DJS1000_RELEASES), 1},
};

static const Unit *current;

static const Unit *by_key(const char *key) {
    for (int i = 0; i < N(UNITS); i++)
        if (key && !strcmp(key, UNITS[i].key)) return &UNITS[i];
    return NULL;
}

int unit_choose(const char *key) {
    const Unit *u = by_key(key);
    if (!u) return -1;
    current = u;
    return 0;
}

const Unit *unit(void) {
    if (current) return current;
    const char *env = getenv("BOOTMENU_UNIT");
    if (env && *env && (current = by_key(env))) return current;
    current = &UNITS[0];
    for (int i = N(UNITS) - 1; i >= 0; i--)
        if (access(UNITS[i].official_exe, F_OK) == 0) {
            current = &UNITS[i];
            break;
        }
    return current;
}

const char *unit_release(long long bytes, const Unit **u) {
    for (int k = 0; k < N(UNITS); k++)
        for (int i = 0; i < UNITS[k].n_releases; i++)
            if (UNITS[k].releases[i].bytes == bytes) {
                if (u) *u = &UNITS[k];
                return UNITS[k].releases[i].version;
            }
    if (u) *u = NULL;
    return NULL;
}
