#define main program_main
#include "../m720-playpause.c"
#undef main
#include <assert.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>

enum scenario {
    NORMAL, NO_FEATURE, NO_KEYBOARD, NO_THUMB, DIVERTED, OTHER_REMAP,
    HOST_CHANGED, MAPPING_CHANGED, WRONG_MAPPING_HOST, BAD_READBACK,
    ERROR_REPLY, SHORT_REPLY, WRONG_PING, CUSTOM_ORIGINAL, ALREADY_APPLIED,
    RESTORED, LATER_CHANGE, SILENT, LOST_WRITE_REPLY, TOO_MANY_CONTROLS,
    LEGACY, NO_F24
};

static const struct mapping original = { .action = { 1, 0, 0x2b, 5 }, .status = 0 };
static const struct mapping custom = { .action = { 1, 0, 0x29, 0 }, .status = 1 };
static char test_directory[PATH_MAX];
static unsigned int tests_run;

static struct mouse test_mouse(void)
{
    struct mouse m = { .fd = -1, .host = 1, .original = original, .supports_f24 = true };
    memcpy(m.identity, "aa:bb:cc:dd:ee:ff", 18);
    return m;
}

static void send_packet(int fd, const uint8_t *packet, size_t size)
{
    assert(write(fd, packet, size) == (ssize_t)size);
}

static void emulate(int fd, enum scenario scenario, unsigned int expected_writes,
                    bool restoring, bool custom_original)
{
    alarm(10);
    struct mapping current = scenario == CUSTOM_ORIGINAL ? custom : original;
    if (restoring || scenario == ALREADY_APPLIED) {
        memcpy(current.action, playpause, 4);
        current.status = 1;
    }
    if (scenario == RESTORED)
        current = original;
    if (scenario == LATER_CHANGE)
        current = custom;
    if (scenario == LEGACY) {
        memcpy(current.action, legacy_playpause, 4);
        current.status = 1;
    }
    unsigned int writes = 0, host_reads = 0, mapping_reads = 0;
    for (;;) {
        uint8_t request[REPORT_SIZE], response[REPORT_SIZE] = { 0x11, 0 };
        ssize_t size = read(fd, request, sizeof(request));
        if (size == 0)
            break;
        assert(size == REPORT_SIZE && request[0] == 0x11 && request[1] == 0xff);
        uint8_t feature = request[2], function = request[3] >> 4;
        assert((request[3] & 15) != 0);
        response[1] = (request[3] & 1) ? 0xff : 0x00;
        memcpy(response + 2, request + 2, 2);
        uint8_t *p = response + 4;
        if (feature == 0 && function == 1) {
            p[0] = 4;
            p[1] = 5;
            p[2] = scenario == WRONG_PING ? 0 : request[6];
        } else if (feature == 0 && function == 0) {
            switch (be16(request + 4)) {
            case 0x1c00: p[0] = scenario == NO_FEATURE ? 0 : 12; break;
            case 0x1814: p[0] = 9; break;
            case 0x1b04: p[0] = 11; break;
            default: assert(false);
            }
        } else if (feature == 9 && function == 0) {
            ++host_reads;
            p[0] = 3;
            p[1] = scenario == HOST_CHANGED && host_reads > 1 ? 2 : 1;
        } else if (feature == 11 && function == 2) {
            assert(be16(request + 4) == THUMB_CID);
            p[1] = THUMB_CID;
            p[2] = scenario == DIVERTED ? 1 : 0;
            p[4] = scenario == OTHER_REMAP ? 0x53 : THUMB_CID;
        } else if (feature == 12 && function == 0) {
            p[1] = scenario == NO_KEYBOARD ? 0x22 : 0x23;
        } else if (feature == 12 && function == 1) {
            p[0] = scenario == TOO_MANY_CONTROLS ? 255 : 8;
        } else if (feature == 12 && function == 2) {
            static const uint8_t cids[8] = { 0x50, 0x51, 0x52, 0x53, 0x56, 0x5b, 0x5d, 0xd0 };
            assert(request[4] < 8 && request[5] == 1);
            p[1] = scenario == NO_THUMB ? 0x50 : cids[request[4]];
        } else if (feature == 12 && function == 3) {
            assert(be16(request + 4) == THUMB_CID && request[6] == 1);
            ++mapping_reads;
            p[1] = THUMB_CID;
            p[2] = scenario == WRONG_MAPPING_HOST ? 2 : 1;
            memcpy(p + 3, current.action, 4);
            p[7] = current.status;
            if (scenario == MAPPING_CHANGED && mapping_reads > 1)
                p[6] = 2;
        } else if (feature == 12 && (function == 4 || function == 5)) {
            ++writes;
            assert(writes <= expected_writes);
            assert(be16(request + 4) == THUMB_CID && request[6] == 1);
            if (restoring) {
                assert(function == (custom_original ? 4 : 5));
                if (custom_original)
                    assert(memcmp(request + 7, custom.action, 4) == 0);
            } else {
                static const uint8_t expected_f24[4] = { 0x01, 0x00, 0x73, 0x00 };
                assert(function == 4 && memcmp(request + 7, expected_f24, 4) == 0);
            }
            if (scenario != BAD_READBACK) {
                if (function == 5)
                    current = original;
                else {
                    memcpy(current.action, request + 7, 4);
                    current.status = 1;
                }
            }
        } else {
            assert(false);
        }
        if (scenario == SILENT || (scenario == LOST_WRITE_REPLY && writes > 0))
            continue;
        if (scenario == ERROR_REPLY) {
            response[2] = 0xff;
            response[3] = request[2];
            response[4] = request[3];
            response[5] = 2;
        }
        uint8_t unrelated[7] = { 0x10, 0xff, 0x44, 0, 0, 0, 0 };
        send_packet(fd, unrelated, sizeof(unrelated));
        uint8_t malformed[5] = { 0x11, 0, feature, request[3], 0 };
        send_packet(fd, malformed, sizeof(malformed));
        uint8_t stale[REPORT_SIZE];
        memcpy(stale, response, sizeof(stale));
        stale[3] ^= 1;
        send_packet(fd, stale, sizeof(stale));
        if (scenario == SHORT_REPLY && feature == 12 && function == 3) {
            response[0] = 0x10;
            send_packet(fd, response, 7);
        } else {
            send_packet(fd, response, sizeof(response));
        }
    }
    assert(writes == expected_writes);
    close(fd);
    _exit(0);
}

static void run_case(const char *name, enum scenario scenario, const char *command,
                     const char *backup, bool expect_success, unsigned int expected_writes,
                     bool custom_original)
{
    fflush(NULL);
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(sockets[0]);
        emulate(sockets[1], scenario, expected_writes, strcmp(command, "restore") == 0, custom_original);
    }
    close(sockets[1]);
    struct mouse m = test_mouse();
    m.supports_f24 = scenario != NO_F24;
    m.fd = sockets[0];
    int result = run_command(&m, command, backup);
    close(m.fd);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert((result == 0) == expect_success);
    printf("PASS: %s\n", name);
    ++tests_run;
}

static void backup_tests(void)
{
    struct mouse m = test_mouse();
    struct mapping decoded;
    uint8_t data[BACKUP_SIZE];
    encode_backup(&m, data);
    assert(decode_backup(&m, data, &decoded) == 0 && same_mapping(&decoded, &original));
    for (size_t i = 0; i < sizeof(data); ++i) {
        data[i] ^= 1;
        assert(decode_backup(&m, data, &decoded) < 0);
        data[i] ^= 1;
    }
    ++m.host;
    assert(decode_backup(&m, data, &decoded) < 0);
    --m.host;
    m.identity[0] = 'b';
    assert(decode_backup(&m, data, &decoded) < 0);
    m = test_mouse();
    assert(save_backup(&m, "original.m720-backup") == 0);
    assert(save_backup(&m, "original.m720-backup") < 0);
    assert(load_backup(&m, "original.m720-backup", &decoded) == 0 && same_mapping(&decoded, &original));
    struct stat st;
    assert(stat("original.m720-backup", &st) == 0 && (st.st_mode & 0777) == 0600);
    assert(symlink("original.m720-backup", "symlink.m720-backup") == 0);
    assert(load_backup(&m, "symlink.m720-backup", &decoded) < 0);
    assert(save_backup(&m, "symlink.m720-backup") < 0);
    assert(mkfifo("fifo.m720-backup", 0600) == 0);
    assert(load_backup(&m, "fifo.m720-backup", &decoded) < 0);
    m.original = custom;
    assert(save_backup(&m, "custom.m720-backup") == 0);
    m.host = 2;
    assert(save_backup(&m, "wrong-host.m720-backup") == 0);
    puts("PASS: backup roundtrip, every-byte corruption, device/host binding, no-clobber, symlink and FIFO rejection");
    ++tests_run;
}

static void cli_tests(void)
{
    char *help[] = { "m720-playpause", "--help", NULL };
    char *missing[] = { "m720-playpause", "apply", NULL };
    char *bad[] = { "m720-playpause", "erase", NULL };
    char *extra[] = { "m720-playpause", "inspect", "--backup", "x", NULL };
    assert(program_main(2, help) == EXIT_SUCCESS);
    assert(program_main(2, missing) == EXIT_FAILURE);
    assert(program_main(2, bad) == EXIT_FAILURE);
    assert(program_main(4, extra) == EXIT_FAILURE);
    struct mouse m = test_mouse();
    assert(open_mouse(&m, "/tmp/hidraw8") < 0);
    assert(open_mouse(&m, "/dev/hidraw") < 0);
    assert(open_mouse(&m, "/dev/hidraw8/../null") < 0);
    assert(valid_identity("aa:bb:cc:dd:ee:ff"));
    assert(!valid_identity(""));
    assert(!valid_identity("zz:bb:cc:dd:ee:ff"));
    puts("PASS: CLI rejects invalid/missing options before device access");
    ++tests_run;
}

static void descriptor_tests(void)
{
    uint8_t descriptor[] = {
        0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,
        0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x06,0x75,0x08,0x15,0x00,0x26,0xa4,
        0x00,0x05,0x07,0x19,0x00,0x2a,0xa4,0x00,0x81,0x00,0xc0
    };
    assert(descriptor_supports_usage(descriptor, sizeof(descriptor), 0x73));
    assert(!descriptor_supports_usage(descriptor, sizeof(descriptor), 0xe8));
    descriptor[31] = 0x65;
    assert(!descriptor_supports_usage(descriptor, sizeof(descriptor), 0x73));
    descriptor[31] = 0xa4;
    descriptor[38] = 0x65;
    assert(!descriptor_supports_usage(descriptor, sizeof(descriptor), 0x73));
    descriptor[38] = 0xa4;
    descriptor[34] = 0x0c;
    assert(!descriptor_supports_usage(descriptor, sizeof(descriptor), 0x73));
    descriptor[34] = 0x07;
    assert(!descriptor_supports_usage(descriptor, sizeof(descriptor) - 2, 0x73));
    const uint8_t pop[] = { 0xb4 };
    assert(!descriptor_supports_usage(pop, sizeof(pop), 0x73));
    puts("PASS: physical M720 descriptor permits F24 but excludes E8; malformed/range-limited descriptors refused");
    ++tests_run;
}

int main(void)
{
    strcpy(test_directory, "/tmp/m720-tests-XXXXXX");
    assert(mkdtemp(test_directory) != NULL);
    assert(chdir(test_directory) == 0);
    cli_tests();
    descriptor_tests();
    backup_tests();
    run_case("inspect sends no mutation", NORMAL, "inspect", NULL, true, 0, false);
    run_case("apply and verify keyboard F24", NORMAL, "apply", "apply.m720-backup", true, 1, false);
    run_case("restore default uses ResetCid", NORMAL, "restore", "original.m720-backup", true, 1, false);
    run_case("restore custom uses SetCid", NORMAL, "restore", "custom.m720-backup", true, 1, true);
    run_case("apply preserves custom original", CUSTOM_ORIGINAL, "apply", "apply-custom.m720-backup", true, 1, false);
    run_case("apply already mapped is no-op", ALREADY_APPLIED, "apply", "unused.m720-backup", true, 0, false);
    assert(access("unused.m720-backup", F_OK) < 0);
    run_case("existing backup prevents mutation", NORMAL, "apply", "original.m720-backup", false, 0, false);
    run_case("wrong backup host prevents restore", NORMAL, "restore", "wrong-host.m720-backup", false, 0, false);
    run_case("missing feature", NO_FEATURE, "inspect", NULL, false, 0, false);
    run_case("unsupported keyboard capability", NO_KEYBOARD, "apply", "unused.m720-backup", false, 0, false);
    run_case("missing persistent thumb CID", NO_THUMB, "apply", "unused.m720-backup", false, 0, false);
    run_case("diverted thumb is refused", DIVERTED, "apply", "unused.m720-backup", false, 0, false);
    run_case("other control remap is refused", OTHER_REMAP, "apply", "unused.m720-backup", false, 0, false);
    run_case("host race prevents mutation", HOST_CHANGED, "apply", "host-race.m720-backup", false, 0, false);
    run_case("mapping race prevents mutation", MAPPING_CHANGED, "apply", "mapping-race.m720-backup", false, 0, false);
    run_case("wrong host response", WRONG_MAPPING_HOST, "inspect", NULL, false, 0, false);
    run_case("readback mismatch is reported", BAD_READBACK, "apply", "readback.m720-backup", false, 1, false);
    run_case("HID++ error is reported", ERROR_REPLY, "inspect", NULL, false, 0, false);
    run_case("short matching response is rejected", SHORT_REPLY, "inspect", NULL, false, 0, false);
    run_case("wrong ping echo is rejected", WRONG_PING, "inspect", NULL, false, 0, false);
    run_case("restore already restored is no-op", RESTORED, "restore", "original.m720-backup", true, 0, false);
    run_case("restore protects later changes", LATER_CHANGE, "restore", "original.m720-backup", false, 0, false);
    run_case("control count is bounded", TOO_MANY_CONTROLS, "inspect", NULL, false, 0, false);
    run_case("silent device times out", SILENT, "inspect", NULL, false, 0, false);
    run_case("lost write reply is uncertain and never retried", LOST_WRITE_REPLY, "apply", "lost.m720-backup", false, 1, false);
    run_case("missing F24 support prevents write", NO_F24, "apply", "unused.m720-backup", false, 0, false);
    run_case("legacy migration preserves original backup", LEGACY, "apply", "original.m720-backup", true, 1, false);
    run_case("legacy restore remains supported", LEGACY, "restore", "original.m720-backup", true, 1, false);
    run_case("legacy migration needs original backup", LEGACY, "apply", "missing.m720-backup", false, 0, false);
    struct mouse m = test_mouse();
    struct mapping saved;
    assert(load_backup(&m, "original.m720-backup", &saved) == 0 && same_mapping(&saved, &original));
    glob_t files = { 0 };
    assert(glob("*.m720-backup", 0, NULL, &files) == 0);
    for (size_t i = 0; i < files.gl_pathc; ++i)
        assert(unlink(files.gl_pathv[i]) == 0);
    globfree(&files);
    assert(chdir("/") == 0 && rmdir(test_directory) == 0);
    printf("All %u test groups passed. No real HID device was opened.\n", tests_run);
    return 0;
}
