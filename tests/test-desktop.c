#include "../desktop.c"
#include <assert.h>

int setup_mouse(const char *device, const char *directory)
{
    (void)device;
    (void)directory;
    abort();
}

int main(void)
{
    static const struct { const char *input, *without, *with; } cases[] = {
        { "['']", "[]", "['0xca']" },
        { "@as []", "[]", "['0xca']" },
        { "['0xca']", "[]", "['0xca']" },
        { "['0xCA', '0xca']", "[]", "['0xca']" },
        { "['<Control>p', '0xca', '<Alt>F8']", "['<Control>p', '<Alt>F8']", "['<Control>p', '<Alt>F8', '0xca']" },
        { "[\"<Super>p\", 'F24']", "[\"<Super>p\", 'F24']", "[\"<Super>p\", 'F24', '0xca']" },
        { "['quote\\\'key']", "['quote\\\'key']", "['quote\\\'key', '0xca']" }
    };
    char without[SETTING_SIZE], with[SETTING_SIZE];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        assert(binding_lists(cases[i].input, without, with, sizeof(with)) == 0);
        assert(strcmp(without, cases[i].without) == 0);
        assert(strcmp(with, cases[i].with) == 0);
        char next_without[SETTING_SIZE], next_with[SETTING_SIZE];
        assert(binding_lists(with, next_without, next_with, sizeof(next_with)) == 0);
        assert(strcmp(with, next_with) == 0);
    }
    const char *bad[] = { "", "true", "['x'", "[123]", "['x' 'y']", "[] junk", "['bad\\" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(binding_lists(bad[i], without, with, sizeof(with)) < 0);
    assert(binding_lists("['<Control>long']", without, with, 16) < 0);
    struct passwd *user = getpwuid(getuid());
    assert(user != NULL);
    char *const arguments[] = { "/usr/bin/printf", "%s", "['<Control>p', '0xca']", NULL };
    assert(user_command(user, arguments, with, sizeof(with)) == 0);
    assert(strcmp(with, "['<Control>p', '0xca']") == 0);
    char *const missing[] = { "/nonexistent/m720-test-command", NULL };
    assert(user_command(user, missing, with, sizeof(with)) < 0);
    assert(user_command(user, arguments, with, 8) < 0);
    puts("Desktop tests passed: additive/idempotent bindings, malformed input, bounded subprocess output.");
    return 0;
}
