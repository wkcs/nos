/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <string.h>
#include "../../app/shell/printf_cmd.h"

struct output { char data[1024]; size_t length; int fail; };
static int capture(void *context, const char *data, size_t length)
{
    struct output *out = context;
    if (out->fail) return -1;
    if (length > 2) length = 2;
    assert(length <= sizeof(out->data) - out->length);
    memcpy(out->data + out->length, data, length);
    out->length += length;
    return (int)length;
}
int main(void)
{
    char buffer[512];
    struct output out = {0};
    char *args[] = { "one", "7", "two", "8" };
    char *binary[] = { "a\\0000b\\cignored" };
    char *numbers[] = { "0x20", "-2", "bad" };
    assert(shell_printf_execute("%s:%03d\\n", 4, args, capture, &out, buffer, sizeof(buffer)) == 0);
    assert(out.length == 16 && memcmp(out.data, "one:007\ntwo:008\n", 16) == 0);
    out.length = 0;
    assert(shell_printf_execute("%b", 1, binary, capture, &out, buffer, sizeof(buffer)) == 0);
    assert(out.length == 3 && memcmp(out.data, "a\0b", 3) == 0);
    out.length = 0;
    assert(shell_printf_execute("%x %d %d", 3, numbers, capture, &out, buffer, sizeof(buffer)) == 1);
    assert(out.length == 7 && memcmp(out.data, "20 -2 0", 7) == 0);
    out.length = 0;
    assert(shell_printf_execute("%s %d", 0, NULL, capture, &out, buffer, sizeof(buffer)) == 0);
    assert(out.length == 2 && memcmp(out.data, " 0", 2) == 0);
    assert(shell_printf_execute("%999s", 1, args, capture, &out, buffer, sizeof(buffer)) == 1);
    out.fail = 1;
    assert(shell_printf_execute("hello", 0, NULL, capture, &out, buffer, sizeof(buffer)) == 1);
    return 0;
}
