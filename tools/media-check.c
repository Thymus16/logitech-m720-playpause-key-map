#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct watched_key { unsigned int code; const char *name; };
static const struct watched_key keys[] = {
    { KEY_F24, "F24" }, { KEY_PLAYPAUSE, "PLAYPAUSE" },
    { KEY_PLAY, "PLAY" }, { KEY_PAUSECD, "PAUSECD" },
    { KEY_NEXTSONG, "NEXT" }, { KEY_PREVIOUSSONG, "PREVIOUS" },
    { KEY_LEFTCTRL, "LCTRL" }, { KEY_RIGHTCTRL, "RCTRL" },
    { KEY_LEFTALT, "LALT" }, { KEY_RIGHTALT, "RALT" },
    { KEY_LEFTSHIFT, "LSHIFT" }, { KEY_RIGHTSHIFT, "RSHIFT" },
    { KEY_LEFTMETA, "LMETA" }, { KEY_RIGHTMETA, "RMETA" }
};

static void show_state(int fd, const char *name)
{
    unsigned char pressed[(KEY_MAX + 8) / 8] = { 0 };
    if (ioctl(fd, EVIOCGKEY(sizeof(pressed)), pressed) < 0) {
        perror("EVIOCGKEY");
        return;
    }
    printf("%s held:", name);
    bool any = false;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        unsigned int code = keys[i].code;
        if (pressed[code / 8] & (1u << (code % 8))) {
            printf(" %s", keys[i].name);
            any = true;
        }
    }
    puts(any ? "" : " none of the monitored keys");
}

int main(void)
{
    struct pollfd devices[8];
    char names[8][128];
    nfds_t count = 0;
    glob_t matches = { 0 };
    if (glob("/dev/input/event*", 0, NULL, &matches) != 0) {
        globfree(&matches);
        fputs("No input devices visible.\n", stderr);
        return 1;
    }
    for (size_t i = 0; i < matches.gl_pathc && count < 8; ++i) {
        int fd = open(matches.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        struct input_id id;
        if (ioctl(fd, EVIOCGID, &id) < 0 || id.vendor != 0x046d ||
            (id.product != 0xb015 && id.product != 0xb34d)) {
            close(fd);
            continue;
        }
        memset(names[count], 0, sizeof(names[count]));
        if (ioctl(fd, EVIOCGNAME(sizeof(names[count]) - 1), names[count]) < 0)
            snprintf(names[count], sizeof(names[count]), "%04x:%04x", id.vendor, id.product);
        devices[count] = (struct pollfd){ .fd = fd, .events = POLLIN };
        printf("Watching %s: %s\n", matches.gl_pathv[i], names[count]);
        show_state(fd, names[count]);
        ++count;
    }
    globfree(&matches);
    if (count == 0) {
        fputs("No accessible Bluetooth M720/K850. Run with sudo.\n", stderr);
        return 1;
    }
    puts("For 25 seconds, tap/release the mouse thumb button and keyboard Play/Pause.");
    puts("Read-only: no input grab or injection; only F24/media/modifier events printed.");
    fflush(stdout);
    struct timespec start, now;
    int status = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &start) < 0) {
        perror("clock_gettime");
        status = 1;
    }
    while (status == 0) {
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
            perror("clock_gettime");
            status = 1;
            break;
        }
        if (now.tv_sec - start.tv_sec >= 25)
            break;
        int ready = poll(devices, count, 500);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0) {
            perror("poll");
            status = 1;
            break;
        }
        for (nfds_t i = 0; i < count; ++i) {
            if (devices[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                printf("Disconnected: %s; rerun after reconnecting.\n", names[i]);
                close(devices[i].fd);
                devices[i].fd = -1;
                continue;
            }
            if (!(devices[i].revents & POLLIN))
                continue;
            struct input_event events[32];
            ssize_t size = read(devices[i].fd, events, sizeof(events));
            if (size <= 0)
                continue;
            for (size_t e = 0; e < (size_t)size / sizeof(events[0]); ++e) {
                if (events[e].type == EV_SYN && events[e].code == SYN_DROPPED)
                    puts("Input events lost; repeat this check.");
                if (events[e].type != EV_KEY)
                    continue;
                for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) {
                    if (events[e].code == keys[k].code)
                        printf("%s: %s %s\n", names[i], keys[k].name,
                               events[e].value == 0 ? "UP" : events[e].value == 1 ? "DOWN" : "REPEAT");
                }
            }
            fflush(stdout);
        }
    }
    for (nfds_t i = 0; i < count; ++i) {
        if (devices[i].fd >= 0) {
            show_state(devices[i].fd, names[i]);
            close(devices[i].fd);
        }
    }
    return status;
}
