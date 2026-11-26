/* The official application (EP085_Main_R on the SP-16, EP113_Main_R on the DJS-1000: unit.c) owns the LCD, the panel
 * link and audio: the menu asks it to quit first thing (SIGTERM), does its own start-up while it shuts down, and only waits for it right before it needs the screen.
 * Polling /proc every 10 ms instead of the shell's 1-second sleeps is most of the start-up time saved. */
#include "bm.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>



/* Sends sig (0 = none) to every live (not zombie) process whose comm is the official app's; returns how many. */
static int each_official(int sig) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char path[64], buf[256];
        snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        ssize_t r = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (r <= 0) continue;
        buf[r] = 0;
        /* "pid (comm) S ..." - comm may hold spaces or parentheses: take the last ')' */
        char *open_p = strchr(buf, '('), *close_p = strrchr(buf, ')');
        if (!open_p || !close_p || close_p < open_p) continue;
        *close_p = 0;
        if (strcmp(open_p + 1, unit()->official_comm) != 0) continue;
        if (close_p[1] == ' ' && close_p[2] == 'Z') continue;
        n++;
        if (sig) kill((pid_t)atoi(e->d_name), sig);
    }
    closedir(d);
    return n;
}

int official_signal(int sig) { return each_official(sig); }
int official_running(void) { return each_official(0) > 0; }

int official_wait_gone(int timeout_ms) {
    double end = bm_now() + timeout_ms / 1000.0;
    while (official_running()) {
        if (bm_now() >= end) return 0;
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return 1;
}
