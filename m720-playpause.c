#define _POSIX_C_SOURCE 200809L
#include "desktop.h"
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { REPORT_SIZE = 20, PAYLOAD_SIZE = 16, BACKUP_SIZE = 36, TIMEOUT_MS = 2000 };
enum { THUMB_CID = 0x00d0, M720_VENDOR = 0x046d, M720_PRODUCT = 0xb015 };

struct mapping {
    uint8_t action[4];
    uint8_t status;
};

struct mouse {
    int fd;
    uint8_t sequence, persistent, host_feature, controls, host;
    uint16_t capabilities;
    bool supports_f24;
    char identity[18];
    struct mapping original;
};

static const uint8_t playpause[4] = { 0x01, 0x00, 0x73, 0x00 };
static const uint8_t legacy_playpause[4] = { 0x01, 0x00, 0xe8, 0x00 };

static bool descriptor_supports_usage(const uint8_t *data, size_t length, uint32_t usage)
{
    struct hid_globals { uint32_t page, minimum, maximum; } global = { 0 }, stack[8];
    size_t depth = 0;
    uint32_t usage_min = UINT32_MAX, usage_max = 0;
    bool found = false;
    for (size_t offset = 0; offset < length;) {
        uint8_t prefix = data[offset++];
        if (prefix == 0xfe)
            return false;
        size_t size = (prefix & 3u) == 3 ? 4 : prefix & 3u;
        if (size > length - offset)
            return false;
        uint32_t value = 0;
        for (size_t i = 0; i < size; ++i)
            value |= (uint32_t)data[offset + i] << (8 * i);
        offset += size;
        unsigned int type = (prefix >> 2) & 3u, tag = prefix >> 4;
        if (type == 1) {
            switch (tag) {
            case 0: global.page = value; break;
            case 1: global.minimum = value; break;
            case 2: global.maximum = value; break;
            case 10:
                if (depth == sizeof(stack) / sizeof(stack[0]))
                    return false;
                stack[depth++] = global;
                break;
            case 11:
                if (depth == 0)
                    return false;
                global = stack[--depth];
                break;
            default: break;
            }
        } else if (type == 2) {
            if (tag == 1)
                usage_min = value;
            if (tag == 2)
                usage_max = value;
        } else if (type == 0) {
            if (tag == 8 && (value & 3u) == 0 && global.page == 7 &&
                global.minimum == 0 && global.maximum >= usage &&
                usage_min <= usage && usage_max >= usage)
                found = true;
            usage_min = UINT32_MAX;
            usage_max = 0;
        }
    }
    return found && depth == 0;
}

static int fail(const char *message)
{
    fprintf(stderr, "m720-playpause: %s\n", message);
    return -1;
}

static int system_error(const char *operation)
{
    fprintf(stderr, "m720-playpause: %s: %s\n", operation, strerror(errno));
    return -1;
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((unsigned int)p[0] << 8 | p[1]);
}

static int64_t milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int exchange(struct mouse *m, uint8_t feature, uint8_t function,
                    const uint8_t *parameters, size_t count,
                    uint8_t *reply, size_t minimum)
{
    uint8_t request[REPORT_SIZE] = { 0x11, 0xff, feature, 0 };
    if (count > PAYLOAD_SIZE || minimum > PAYLOAD_SIZE || function > 15)
        return fail("invalid internal request");
    m->sequence = (uint8_t)(m->sequence % 15 + 1);
    request[3] = (uint8_t)((function << 4) | m->sequence);
    if (count != 0)
        memcpy(request + 4, parameters, count);

    ssize_t written;
    do {
        written = write(m->fd, request, sizeof(request));
    } while (written < 0 && errno == EINTR);
    if (written < 0)
        return system_error("send HID++ request");
    if (written != REPORT_SIZE)
        return fail("short HID++ write; outcome unknown");
    int64_t start = milliseconds();
    if (start < 0)
        return system_error("read monotonic clock");

    for (;;) {
        int64_t now = milliseconds();
        if (now < 0)
            return system_error("read monotonic clock");
        if (now - start >= TIMEOUT_MS)
            return fail("HID++ timeout; wake the mouse and retry inspection");
        struct pollfd pfd = { .fd = m->fd, .events = POLLIN };
        int ready = poll(&pfd, 1, (int)(TIMEOUT_MS - (now - start)));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            return system_error("poll HID device");
        if (ready == 0)
            continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return fail("mouse disconnected or HID device unavailable");
        uint8_t packet[64];
        ssize_t size = read(m->fd, packet, sizeof(packet));
        if (size < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (size < 0)
            return system_error("read HID++ response");
        if (size == 0)
            return fail("HID device closed");
        if (!((size == 7 && packet[0] == 0x10) ||
              (size == REPORT_SIZE && packet[0] == 0x11)))
            continue;
        /* Direct Bluetooth devices may reply with device index 0 instead of 255. */
        if (packet[1] != 0xff && packet[1] != 0x00)
            continue;
        if ((packet[2] == 0xff || packet[2] == 0x8f) &&
            packet[3] == feature && packet[4] == request[3]) {
            fprintf(stderr, "m720-playpause: HID++ error 0x%02x on feature index "
                    "0x%02x function %u\n", packet[5], feature, function);
            return -1;
        }
        if (packet[2] != feature || packet[3] != request[3])
            continue;
        size_t payload = (size_t)size - 4;
        if (payload < minimum)
            return fail("truncated matching HID++ response");
        memset(reply, 0, PAYLOAD_SIZE);
        memcpy(reply, packet + 4, payload);
        return 0;
    }
}

static int feature_index(struct mouse *m, uint16_t id, uint8_t *index)
{
    uint8_t parameters[2] = { (uint8_t)(id >> 8), (uint8_t)id };
    uint8_t reply[PAYLOAD_SIZE];
    if (exchange(m, 0, 0, parameters, sizeof(parameters), reply, 3) < 0)
        return -1;
    if (reply[0] == 0) {
        fprintf(stderr, "m720-playpause: required HID++ feature 0x%04x is absent\n", id);
        return -1;
    }
    *index = reply[0];
    return 0;
}

static int current_host(struct mouse *m, uint8_t *host)
{
    uint8_t reply[PAYLOAD_SIZE];
    if (exchange(m, m->host_feature, 0, NULL, 0, reply, 2) < 0)
        return -1;
    if (reply[0] == 0 || reply[0] > 3 || reply[1] >= reply[0])
        return fail("invalid host-channel response");
    *host = reply[1];
    return 0;
}

static int read_mapping(struct mouse *m, struct mapping *mapping)
{
    uint8_t parameters[3] = { 0x00, THUMB_CID, m->host };
    uint8_t reply[PAYLOAD_SIZE];
    if (exchange(m, m->persistent, 3, parameters, sizeof(parameters), reply, 8) < 0)
        return -1;
    if (be16(reply) != THUMB_CID || reply[2] != m->host || reply[7] > 1) {
        fprintf(stderr, "m720-playpause: received CID=0x%04x host=%u status=0x%02x; "
                "expected CID=0x00d0 host=%u status=0 or 1\n",
                be16(reply), reply[2], reply[7], m->host);
        return fail("unexpected mapping identity, host or status");
    }
    memcpy(mapping->action, reply + 3, sizeof(mapping->action));
    mapping->status = reply[7];
    return 0;
}

static bool same_mapping(const struct mapping *a, const struct mapping *b)
{
    return a->status == b->status && memcmp(a->action, b->action, 4) == 0;
}

static int check_reporting(struct mouse *m)
{
    uint8_t parameters[2] = { 0x00, THUMB_CID };
    uint8_t reply[PAYLOAD_SIZE];
    if (exchange(m, m->controls, 2, parameters, sizeof(parameters), reply, 5) < 0)
        return -1;
    if (be16(reply) != THUMB_CID)
        return fail("unexpected control-reporting response");
    if ((reply[2] & 0x55) != 0 || (be16(reply + 3) != 0 && be16(reply + 3) != THUMB_CID))
        return fail("thumb button is diverted or remapped by another tool; restore its regular reporting first");
    return 0;
}

static int inspect(struct mouse *m)
{
    uint8_t reply[PAYLOAD_SIZE], ping[3] = { 0, 0, 0xa5 };
    if (exchange(m, 0, 1, ping, sizeof(ping), reply, 3) < 0)
        return -1;
    if (reply[0] < 2 || reply[2] != ping[2])
        return fail("HID++ 2.0 or newer with matching ping required");
    printf("HID++ %u.%u; Bluetooth M720 (046d:b015)\n", reply[0], reply[1]);
    if (feature_index(m, 0x1c00, &m->persistent) < 0 ||
        feature_index(m, 0x1814, &m->host_feature) < 0 ||
        feature_index(m, 0x1b04, &m->controls) < 0 ||
        current_host(m, &m->host) < 0)
        return -1;
    if (exchange(m, m->persistent, 0, NULL, 0, reply, 2) < 0)
        return -1;
    m->capabilities = be16(reply);
    printf("Host channel: %u; persistent capabilities: 0x%04x\n",
           m->host + 1u, m->capabilities);
    if (!(m->capabilities & 0x0001))
        return fail("persistent keyboard actions are unsupported");
    if (exchange(m, m->persistent, 1, NULL, 0, reply, 1) < 0)
        return -1;
    uint8_t count = reply[0];
    if (count == 0 || count > 32)
        return fail("unexpected persistent-control count");
    bool found = false;
    for (uint8_t index = 0; index < count; ++index) {
        uint8_t parameters[2] = { index, m->host };
        if (exchange(m, m->persistent, 2, parameters, sizeof(parameters), reply, 2) < 0)
            return -1;
        if (be16(reply) == THUMB_CID)
            found = true;
    }
    if (!found)
        return fail("thumb control 0x00d0 is not persistently remappable");
    if (read_mapping(m, &m->original) < 0)
        return -1;
    printf("Thumb mapping: action=%u usage=0x%04x modifiers=0x%02x (%s)\n",
           m->original.action[0], be16(m->original.action + 1), m->original.action[3],
           m->original.status ? "custom" : "default");
    printf("F24 advertised by HID descriptor: %s\n", m->supports_f24 ? "yes" : "no");
    puts("Proposed mapping: F24 (keyboard usage 0x0073); bind F24 to desktop Play/Pause.");
    return check_reporting(m);
}

static bool valid_identity(const char *identity)
{
    if (strnlen(identity, 18) != 17)
        return false;
    for (size_t i = 0; i < 17; ++i) {
        char c = identity[i];
        if (i % 3 == 2 ? c != ':' : !((c >= '0' && c <= '9') ||
                                     (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

static uint32_t checksum(const uint8_t *data, size_t count)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < count; ++i)
        hash = (hash ^ data[i]) * UINT32_C(16777619);
    return hash;
}

static void encode_backup(const struct mouse *m, uint8_t data[BACKUP_SIZE])
{
    memset(data, 0, BACKUP_SIZE);
    memcpy(data, "M720PP01", 8);
    memcpy(data + 8, m->identity, 18);
    data[26] = m->host;
    memcpy(data + 27, m->original.action, 4);
    data[31] = m->original.status;
    uint32_t hash = checksum(data, 32);
    for (size_t i = 0; i < 4; ++i)
        data[32 + i] = (uint8_t)(hash >> (24 - 8 * i));
}

static int decode_backup(const struct mouse *m, const uint8_t data[BACKUP_SIZE],
                         struct mapping *saved)
{
    uint32_t expected = (uint32_t)be16(data + 32) << 16 | be16(data + 34);
    if (memcmp(data, "M720PP01", 8) != 0 || checksum(data, 32) != expected)
        return fail("invalid or damaged backup");
    if (memcmp(data + 8, m->identity, 18) != 0 || data[26] != m->host)
        return fail("backup belongs to a different mouse or host channel");
    if (data[27] > 9 || data[31] > 1)
        return fail("unsupported backup action or status");
    memcpy(saved->action, data + 27, 4);
    saved->status = data[31];
    return 0;
}

static int save_backup(const struct mouse *m, const char *path)
{
    char directory[PATH_MAX];
    if (strlen(path) >= sizeof(directory))
        return fail("backup path too long");
    memcpy(directory, path, strlen(path) + 1);
    char *slash = strrchr(directory, '/');
    const char *name = path;
    if (slash != NULL) {
        name = strrchr(path, '/') + 1;
        if (slash == directory)
            slash[1] = '\0';
        else
            *slash = '\0';
    } else {
        strcpy(directory, ".");
    }
    int parent = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0)
        return system_error("open backup directory");
    int fd = openat(parent, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        int result = system_error("create backup (must be a new file)");
        close(parent);
        return result;
    }
    uint8_t data[BACKUP_SIZE];
    encode_backup(m, data);
    size_t offset = 0;
    int result = 0;
    while (offset < sizeof(data)) {
        ssize_t size = write(fd, data + offset, sizeof(data) - offset);
        if (size < 0 && errno == EINTR)
            continue;
        if (size <= 0) {
            result = fail("backup write failed; mouse unchanged");
            break;
        }
        offset += (size_t)size;
    }
    if (result == 0 && fsync(fd) < 0)
        result = system_error("sync backup");
    if (close(fd) < 0)
        result = system_error("close backup");
    if (result == 0 && fsync(parent) < 0)
        result = system_error("sync backup directory");
    close(parent);
    if (result == 0)
        printf("Original mapping saved to %s\n", path);
    return result;
}

static int load_backup(const struct mouse *m, const char *path, struct mapping *saved)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return system_error("open backup");
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size != BACKUP_SIZE) {
        close(fd);
        return fail("backup must be a regular 36-byte file");
    }
    uint8_t data[BACKUP_SIZE];
    size_t offset = 0;
    while (offset < sizeof(data)) {
        ssize_t size = read(fd, data + offset, sizeof(data) - offset);
        if (size < 0 && errno == EINTR)
            continue;
        if (size <= 0) {
            close(fd);
            return fail("cannot read complete backup");
        }
        offset += (size_t)size;
    }
    close(fd);
    return decode_backup(m, data, saved);
}

static bool tool_mapping(const struct mapping *mapping)
{
    return memcmp(mapping->action, playpause, 4) == 0 ||
           memcmp(mapping->action, legacy_playpause, 4) == 0;
}

static int import_candidate(const struct mouse *m, const char *path, struct mapping *saved)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return 1;
    struct stat st;
    uint8_t data[BACKUP_SIZE];
    bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == BACKUP_SIZE &&
                 read(fd, data, sizeof(data)) == BACKUP_SIZE;
    close(fd);
    if (!valid || memcmp(data + 8, m->identity, 18) != 0 || data[26] != m->host)
        return 1;
    if (decode_backup(m, data, saved) < 0)
        return -1;
    return tool_mapping(saved) ? 1 : 0;
}

static int automatic_backup(struct mouse *m, const char *directory, char path[PATH_MAX])
{
    char identity[18];
    memcpy(identity, m->identity, sizeof(identity));
    for (size_t i = 0; i < 17; ++i)
        if (identity[i] == ':') identity[i] = '-';
    int length = snprintf(path, PATH_MAX, "%s/mouse-%s-channel-%u.m720-backup",
                          directory, identity, m->host + 1u);
    if (length < 0 || length >= PATH_MAX)
        return fail("automatic backup path too long");
    struct mapping saved;
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (load_backup(m, path, &saved) < 0)
            return -1;
        if (!tool_mapping(&m->original) && !same_mapping(&saved, &m->original))
            return fail("mouse mapping differs from its saved original; refusing to overwrite another change");
        printf("Using original backup: %s\n", path);
        return 0;
    }
    if (errno != ENOENT)
        return system_error("inspect automatic backup path");
    glob_t files = { 0 };
    int result = glob("*.m720-backup", 0, NULL, &files);
    if (result != 0 && result != GLOB_NOMATCH) {
        globfree(&files);
        return fail("cannot search for existing mouse backups");
    }
    bool found = false;
    if (files.gl_pathc > 128) {
        globfree(&files);
        return fail("too many backup candidates in the current directory");
    }
    for (size_t i = 0; i < files.gl_pathc; ++i) {
        struct mapping candidate;
        int status = import_candidate(m, files.gl_pathv[i], &candidate);
        if (status < 0 || (status == 0 && found && !same_mapping(&saved, &candidate))) {
            globfree(&files);
            return fail("damaged or conflicting original backups; specify a backup explicitly");
        }
        if (status == 0) { saved = candidate; found = true; }
    }
    globfree(&files);
    if (!found && tool_mapping(&m->original)) {
        if (memcmp(m->original.action, legacy_playpause, 4) == 0)
            return fail("legacy mapping requires the original backup; run setup from its directory");
        puts("Already mapped to F24. Original backup not found here; existing backups are unchanged.");
        return 0;
    }
    if (found && !tool_mapping(&m->original) && !same_mapping(&saved, &m->original))
        return fail("existing backup differs from current mouse mapping; refusing to overwrite another change");
    struct mouse snapshot = *m;
    if (found) snapshot.original = saved;
    if (save_backup(&snapshot, path) < 0)
        return -1;
    return 0;
}

static int change_mapping(struct mouse *m, const struct mapping *target)
{
    uint8_t host, reply[PAYLOAD_SIZE];
    struct mapping before, after;
    if (current_host(m, &host) < 0 || host != m->host)
        return fail("host channel changed; no mapping written");
    if (check_reporting(m) < 0 || read_mapping(m, &before) < 0)
        return -1;
    if (!same_mapping(&before, &m->original))
        return fail("mapping changed during inspection; no mapping written");
    uint8_t parameters[7] = { 0, THUMB_CID, m->host, 0, 0, 0, 0 };
    memcpy(parameters + 3, target->action, 4);
    /* An untouched default must be restored with ResetCid, not saved as a custom action. */
    uint8_t function = target->status ? 4 : 5;
    size_t count = target->status ? sizeof(parameters) : 3;
    if (exchange(m, m->persistent, function, parameters, count, reply, 0) < 0 ||
        read_mapping(m, &after) < 0) {
        return fail("write outcome uncertain; keep the backup and inspect before retrying");
    }
    if (!same_mapping(&after, target))
        return fail("mapping read-back differs; keep the backup and use restore");
    puts("Mapping read-back verified. No background process is needed by this tool.");
    return 0;
}

static int run_command(struct mouse *m, const char *command, const char *backup)
{
    if (inspect(m) < 0)
        return -1;
    if (strcmp(command, "inspect") == 0) {
        puts("Inspection only: no settings changed.");
        return 0;
    }
    struct mapping target = { .action = { 0 }, .status = 1 };
    if (strcmp(command, "apply") == 0) {
        if (!m->supports_f24)
            return fail("HID descriptor does not advertise F24; no mapping written");
        memcpy(target.action, playpause, 4);
        if (memcmp(m->original.action, playpause, 4) == 0) {
            puts("Already mapped to F24; nothing changed, no backup created.");
            return 0;
        }
        if (m->original.action[0] > 9)
            return fail("original action cannot be safely backed up");
        if (memcmp(m->original.action, legacy_playpause, 4) == 0) {
            struct mapping saved;
            if (load_backup(m, backup, &saved) < 0)
                return fail("migrating the old 0xe8 mapping requires its original backup");
            if (memcmp(saved.action, legacy_playpause, 4) == 0 ||
                memcmp(saved.action, playpause, 4) == 0)
                return fail("migration backup does not contain the original assignment");
            puts("Migrating the old 0xe8 mapping; preserving the existing original backup.");
        } else if (save_backup(m, backup) < 0) {
            return -1;
        }
    } else {
        if (load_backup(m, backup, &target) < 0)
            return -1;
        if (same_mapping(&m->original, &target)) {
            puts("Original mapping is already restored; nothing changed.");
            return 0;
        }
        if (memcmp(m->original.action, playpause, 4) != 0 &&
            memcmp(m->original.action, legacy_playpause, 4) != 0)
            return fail("current mapping is neither this tool's mapping nor the backup; refusing to overwrite another change");
    }
    return change_mapping(m, &target);
}

static int discover(char path[PATH_MAX])
{
    glob_t matches = { 0 };
    int result = glob("/sys/class/hidraw/hidraw*/device/uevent", 0, NULL, &matches);
    if (result != 0) {
        globfree(&matches);
        return fail("no HID devices visible; connect the M720 via Bluetooth");
    }
    unsigned int found = 0;
    for (size_t i = 0; i < matches.gl_pathc; ++i) {
        FILE *file = fopen(matches.gl_pathv[i], "r");
        if (file == NULL)
            continue;
        char line[256];
        while (fgets(line, sizeof(line), file) != NULL) {
            unsigned int bus, vendor, product, number;
            if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &vendor, &product) == 3 &&
                bus == BUS_BLUETOOTH && vendor == M720_VENDOR && product == M720_PRODUCT &&
                sscanf(matches.gl_pathv[i], "/sys/class/hidraw/hidraw%u/", &number) == 1) {
                snprintf(path, PATH_MAX, "/dev/hidraw%u", number);
                ++found;
            }
        }
        fclose(file);
    }
    globfree(&matches);
    if (found != 1)
        return fail(found ? "multiple M720 devices; choose --device /dev/hidrawN" :
                            "no Bluetooth M720 found (USB receivers are not supported)");
    return 0;
}

static int open_mouse(struct mouse *m, const char *path)
{
    const char *suffix = path + (strlen(path) >= 11 ? 11 : strlen(path));
    if (strncmp(path, "/dev/hidraw", 11) != 0 || *suffix == '\0' ||
        strspn(suffix, "0123456789") != strlen(suffix))
        return fail("device path must be /dev/hidrawN");
    m->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (m->fd < 0)
        return system_error("open M720; device access may require sudo");
    struct hidraw_devinfo info = { 0 };
    struct stat st;
    if (fstat(m->fd, &st) < 0 || !S_ISCHR(st.st_mode) ||
        ioctl(m->fd, HIDIOCGRAWINFO, &info) < 0 ||
        info.bustype != BUS_BLUETOOTH || (uint16_t)info.vendor != M720_VENDOR ||
        (uint16_t)info.product != M720_PRODUCT)
        return fail("device is not a Bluetooth Logitech M720 (046d:b015)");
    char identity[128] = { 0 };
    if (ioctl(m->fd, HIDIOCGRAWUNIQ(sizeof(identity)), identity) < 0 || !valid_identity(identity))
        return fail("cannot obtain a Bluetooth identity for safe backup/restore");
    memcpy(m->identity, identity, sizeof(m->identity));
    int descriptor_size = 0;
    struct hidraw_report_descriptor descriptor = { 0 };
    if (ioctl(m->fd, HIDIOCGRDESCSIZE, &descriptor_size) < 0 ||
        descriptor_size <= 0 || descriptor_size > HID_MAX_DESCRIPTOR_SIZE)
        return fail("cannot read HID descriptor size");
    descriptor.size = (uint32_t)descriptor_size;
    if (ioctl(m->fd, HIDIOCGRDESC, &descriptor) < 0 || descriptor.size > HID_MAX_DESCRIPTOR_SIZE)
        return fail("cannot read HID descriptor");
    m->supports_f24 = descriptor_supports_usage(descriptor.value, descriptor.size, 0x73);
    if (flock(m->fd, LOCK_EX | LOCK_NB) < 0)
        return fail("another instance has locked the device");
    printf("Device: %s\n", path);
    return 0;
}

int setup_mouse(const char *device, const char *state_directory)
{
    char discovered[PATH_MAX], backup[PATH_MAX];
    if (device == NULL) {
        if (discover(discovered) < 0) return -1;
        device = discovered;
    }
    struct mouse m = { .fd = -1 };
    int result = open_mouse(&m, device);
    if (result == 0) result = inspect(&m);
    if (result == 0 && !m.supports_f24)
        result = fail("HID descriptor does not advertise F24; no mapping written");
    if (result == 0) result = automatic_backup(&m, state_directory, backup);
    if (result == 0) {
        if (memcmp(m.original.action, playpause, 4) == 0) {
            puts("Mouse already mapped correctly; no device write needed.");
        } else {
            struct mapping target = { .status = 1 };
            memcpy(target.action, playpause, 4);
            result = change_mapping(&m, &target);
        }
    }
    if (m.fd >= 0) close(m.fd);
    return result;
}

static void usage(void)
{
    puts("Usage: m720-playpause [setup|inspect|apply|restore] [--device /dev/hidrawN]\n"
         "                     [--backup PATH]\n\n"
         "setup    Configure the connected mouse and refresh GNOME Play/Pause (default).\n"
         "inspect  Read capabilities and thumb mapping; no settings changed.\n"
         "apply    Save a NEW backup, then persistently map the hidden thumb button to F24.\n"
         "restore  Restore the mapping from that backup for this mouse and channel.\n\n"
         "apply and restore require --backup PATH. Bluetooth M720 only.\n"
         "Targets thumb CID 0x00d0, not the scroll-wheel/middle button.\n"
         "Setup requests sudo and keeps per-mouse/channel backups in /var/lib/m720-playpause.\n"
         "Setup preserves other GNOME shortcuts and the keyboard media bindings.\n"
         "Migrating the old 0xe8 mapping reuses its existing original backup.\n"
         "Close Solaar, LogiOps, and other device configuration tools first.\n"
         "No installation, daemon, network access, or downloaded dependencies.");
}

int main(int argc, char **argv)
{
    const char *command = "setup", *backup = NULL, *device = NULL;
    int index = 1;
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage();
        return EXIT_SUCCESS;
    }
    if (index < argc && argv[index][0] != '-')
        command = argv[index++];
    if (strcmp(command, "setup") != 0 && strcmp(command, "inspect") != 0 &&
        strcmp(command, "apply") != 0 && strcmp(command, "restore") != 0) {
        usage();
        return EXIT_FAILURE;
    }
    while (index < argc) {
        if (strcmp(argv[index], "--device") == 0 && index + 1 < argc && device == NULL)
            device = argv[++index];
        else if (strcmp(argv[index], "--backup") == 0 && index + 1 < argc && backup == NULL)
            backup = argv[++index];
        else {
            usage();
            return EXIT_FAILURE;
        }
        ++index;
    }
    bool mutation = strcmp(command, "apply") == 0 || strcmp(command, "restore") == 0;
    if ((mutation && (backup == NULL || *backup == '\0')) || (!mutation && backup != NULL)) {
        usage();
        return EXIT_FAILURE;
    }
    if (strcmp(command, "setup") == 0)
        return desktop_setup(device) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    char discovered[PATH_MAX];
    if (device == NULL) {
        if (discover(discovered) < 0)
            return EXIT_FAILURE;
        device = discovered;
    }
    struct mouse m = { .fd = -1 };
    int result = open_mouse(&m, device);
    if (result == 0)
        result = run_command(&m, command, backup);
    if (m.fd >= 0)
        close(m.fd);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
