#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "desktop.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SETTINGS "/usr/bin/gsettings"
#define SCHEMA "org.gnome.settings-daemon.plugins.media-keys"
#define STATE_DIRECTORY "/var/lib/m720-playpause"
enum { SETTING_SIZE = 4096 };

static int desktop_error(const char *message)
{
    fprintf(stderr, "m720-playpause: %s\n", message);
    return -1;
}

static int user_command(const struct passwd *user, char *const arguments[],
                        char *output, size_t capacity)
{
    int pipes[2];
    if (pipe(pipes) < 0)
        return desktop_error("cannot create settings pipe");
    pid_t pid = fork();
    if (pid < 0) {
        close(pipes[0]);
        close(pipes[1]);
        return desktop_error("cannot start settings command");
    }
    if (pid == 0) {
        close(pipes[0]);
        int output_fd = dup2(pipes[1], STDOUT_FILENO);
        if (output_fd < 0)
            _exit(126);
        close(pipes[1]);
        if (geteuid() != user->pw_uid &&
            (setgroups(0, NULL) < 0 || setgid(user->pw_gid) < 0 || setuid(user->pw_uid) < 0))
            _exit(126);
        char runtime[64], bus[96];
        snprintf(runtime, sizeof(runtime), "/run/user/%lu", (unsigned long)user->pw_uid);
        snprintf(bus, sizeof(bus), "unix:path=%s/bus", runtime);
        if (setenv("HOME", user->pw_dir, 1) < 0 ||
            setenv("XDG_RUNTIME_DIR", runtime, 1) < 0 ||
            setenv("DBUS_SESSION_BUS_ADDRESS", bus, 1) < 0)
            _exit(126);
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("DCONF_PROFILE");
        unsetenv("GSETTINGS_BACKEND");
        unsetenv("GSETTINGS_SCHEMA_DIR");
        alarm(15);
        execv(arguments[0], arguments);
        close(output_fd);
        _exit(127);
    }
    close(pipes[1]);
    size_t used = 0;
    bool overflow = false, failed = false;
    char buffer[512];
    for (;;) {
        ssize_t count = read(pipes[0], buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            failed = true;
            break;
        }
        if (count == 0)
            break;
        if ((size_t)count >= capacity - used) {
            overflow = true;
            continue;
        }
        memcpy(output + used, buffer, (size_t)count);
        used += (size_t)count;
    }
    close(pipes[0]);
    output[used] = '\0';
    int status;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || failed || overflow)
        return desktop_error("desktop settings command failed; run setup in your logged-in GNOME session");
    while (used > 0 && isspace((unsigned char)output[used - 1]))
        output[--used] = '\0';
    return 0;
}

static int read_setting(const struct passwd *user, char output[SETTING_SIZE])
{
    char *const arguments[] = { SETTINGS, "get", SCHEMA, "play", NULL };
    return user_command(user, arguments, output, SETTING_SIZE);
}

static int set_setting(const struct passwd *user, char *value)
{
    char output[SETTING_SIZE];
    char *const arguments[] = { SETTINGS, "set", SCHEMA, "play", value, NULL };
    return user_command(user, arguments, output, sizeof(output));
}

static int binding_lists(const char *original, char *without, char *with, size_t capacity)
{
    const char *p = original;
    while (isspace((unsigned char)*p)) ++p;
    if (strncmp(p, "@as ", 4) == 0) p += 4;
    while (isspace((unsigned char)*p)) ++p;
    if (*p++ != '[' || capacity < 16)
        return desktop_error("unexpected GNOME shortcut format");
    size_t used = 1;
    without[0] = '[';
    bool first = true;
    for (;;) {
        while (isspace((unsigned char)*p)) ++p;
        if (*p == ']') { ++p; break; }
        const char *begin = p;
        char quote = *p++;
        if (quote != '\'' && quote != '"')
            return desktop_error("invalid GNOME shortcut list");
        char value[SETTING_SIZE];
        size_t n = 0;
        while (*p != quote) {
            if (*p == '\0' || n + 1 >= sizeof(value))
                return desktop_error("unterminated or oversized shortcut");
            if (*p == '\\') {
                ++p;
                if (*p == '\0') return desktop_error("invalid shortcut escape");
            }
            value[n++] = *p++;
        }
        value[n] = '\0';
        ++p;
        if (strcmp(value, "0xca") != 0 && strcmp(value, "0xCA") != 0 && *value != '\0') {
            size_t length = (size_t)(p - begin);
            if (used + length + 16 >= capacity)
                return desktop_error("shortcut list too large");
            if (!first) { without[used++] = ','; without[used++] = ' '; }
            memcpy(without + used, begin, length);
            used += length;
            first = false;
        }
        while (isspace((unsigned char)*p)) ++p;
        if (*p == ']') { ++p; break; }
        if (*p++ != ',') return desktop_error("invalid shortcut separator");
    }
    while (isspace((unsigned char)*p)) ++p;
    if (*p != '\0') return desktop_error("trailing data in shortcut list");
    without[used] = ']';
    without[used + 1] = '\0';
    memcpy(with, without, used);
    snprintf(with + used, capacity - used, "%s'0xca']", first ? "" : ", ");
    return 0;
}

static int state_directory(void)
{
    if (mkdir(STATE_DIRECTORY, 0700) < 0 && errno != EEXIST)
        return desktop_error("cannot create backup directory");
    struct stat st;
    if (lstat(STATE_DIRECTORY, &st) < 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != 0 || (st.st_mode & 0077) != 0)
        return desktop_error("backup directory must be a root-owned private directory");
    return 0;
}

static int backup_setting(const struct passwd *user, const char *value)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), STATE_DIRECTORY "/gnome-%lu-play.txt", (unsigned long)user->pw_uid);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (errno != EEXIST)
            return desktop_error("cannot save original desktop binding");
        fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) return desktop_error("cannot open desktop binding backup");
        struct stat st;
        char existing[SETTING_SIZE], without[SETTING_SIZE], with[SETTING_SIZE];
        ssize_t size = read(fd, existing, sizeof(existing) - 1);
        bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 &&
                     (st.st_mode & 0077) == 0 && size > 0 && st.st_size == size;
        close(fd);
        if (!valid) return desktop_error("invalid desktop binding backup; original file preserved");
        existing[size] = '\0';
        return binding_lists(existing, without, with, sizeof(with));
    }
    size_t length = strlen(value), used = 0;
    int result = 0;
    while (used < length) {
        ssize_t size = write(fd, value + used, length - used);
        if (size < 0 && errno == EINTR) continue;
        if (size <= 0) { result = -1; break; }
        used += (size_t)size;
    }
    if (fsync(fd) < 0) result = -1;
    if (close(fd) < 0) result = -1;
    int directory = open(STATE_DIRECTORY, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) result = -1;
    else { if (fsync(directory) < 0) result = -1; close(directory); }
    return result == 0 ? 0 : desktop_error("desktop binding backup failed");
}

static int configure_desktop(const struct passwd *user, const char *device)
{
    char original[SETTING_SIZE], without[SETTING_SIZE], wanted[SETTING_SIZE], actual[SETTING_SIZE];
    if (read_setting(user, original) < 0 || binding_lists(original, without, wanted, sizeof(wanted)) < 0)
        return -1;
    char *const writable[] = { SETTINGS, "writable", SCHEMA, "play", NULL };
    if (user_command(user, writable, actual, sizeof(actual)) < 0 || strcmp(actual, "true") != 0)
        return desktop_error("GNOME Play/Pause binding is not writable");
    if (backup_setting(user, original) < 0 || setup_mouse(device, STATE_DIRECTORY) < 0)
        return -1;
    if (read_setting(user, actual) < 0 || strcmp(actual, original) != 0)
        return desktop_error("desktop binding changed during setup; mouse configured, rerun setup");
    sigset_t blocked, previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) < 0)
        return desktop_error("cannot protect shortcut refresh from interruption");
    /* Re-register the raw-keycode grab after input-device changes or suspend/resume. */
    int result = set_setting(user, without);
    if (result == 0) {
        struct timespec delay = { .tv_sec = 1 };
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
        result = set_setting(user, wanted);
    }
    if (result < 0 || read_setting(user, actual) < 0 || strcmp(actual, wanted) != 0) {
        if (set_setting(user, original) < 0)
            fprintf(stderr, "Restore GNOME play binding from " STATE_DIRECTORY "/gnome-%lu-play.txt\n",
                    (unsigned long)user->pw_uid);
        result = desktop_error("desktop binding verification failed; attempted to restore its previous value");
    }
    sigprocmask(SIG_SETMASK, &previous, NULL);
    if (result == 0) {
        puts("GNOME Play/Pause configured and refreshed for F24 (0xca). Other shortcuts preserved.");
        puts("Setup complete. Press the thumb button to test playback; no background process remains.");
    }
    return result;
}

int desktop_setup(const char *device)
{
    if (geteuid() != 0) {
        char executable[PATH_MAX];
        ssize_t size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
        if (size < 0 || (size_t)size >= sizeof(executable) - 1)
            return desktop_error("cannot locate executable");
        executable[size] = '\0';
        char *arguments[] = { "/usr/bin/sudo", "--", executable, "setup", NULL, NULL, NULL };
        if (device != NULL) { arguments[4] = "--device"; arguments[5] = (char *)device; }
        fflush(NULL);
        execv(arguments[0], arguments);
        return desktop_error("cannot start sudo; run setup from a terminal");
    }
    const char *uid_text = getenv("SUDO_UID");
    char *end = NULL;
    errno = 0;
    unsigned long uid = uid_text ? strtoul(uid_text, &end, 10) : 0;
    if (uid == 0 || uid > UINT32_MAX || errno != 0 || end == uid_text || end == NULL || *end != '\0')
        return desktop_error("run setup from your desktop user account (sudo is supported)");
    struct passwd *user = getpwuid((uid_t)uid);
    if (user == NULL)
        return desktop_error("cannot identify the desktop user");
    if (state_directory() < 0) return -1;
    int lock = open(STATE_DIRECTORY "/setup.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0) return desktop_error("cannot open setup lock");
    if (flock(lock, LOCK_EX | LOCK_NB) < 0) {
        close(lock);
        return desktop_error("another setup is in progress");
    }
    int result = configure_desktop(user, device);
    close(lock);
    return result;
}
