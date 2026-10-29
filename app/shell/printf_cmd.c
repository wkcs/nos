/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "printf_cmd.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int emit(shell_printf_write_fn write, void *context, const char *data, size_t length)
{
    while (length != 0) {
        int n = write(context, data, length);
        if (n <= 0 || (size_t)n > length) return -1;
        data += n;
        length -= (size_t)n;
    }
    return 0;
}

/* Decode the format and %b escape sets without treating a NUL as a terminator. */
static int escaped(const char **source, char *value, int binary)
{
    const char *p = *source;
    unsigned int n = 0;
    int count = 0;
    char c = *p++;
    if (c == '\0') {
        *value = '\\';
        return 0;
    }
    if (c == 'c' && binary) return 1;
    switch (c) {
    case 'a': *value = '\a'; break;
    case 'b': *value = '\b'; break;
    case 'f': *value = '\f'; break;
    case 'n': *value = '\n'; break;
    case 'r': *value = '\r'; break;
    case 't': *value = '\t'; break;
    case 'v': *value = '\v'; break;
    case '\\': *value = '\\'; break;
    default:
        if (binary && c == '0') {
            while (count < 3 && *p >= '0' && *p <= '7') {
                n = n * 8 + (unsigned int)(*p++ - '0');
                count++;
            }
            *value = (char)n;
        } else if (!binary && c >= '0' && c <= '7') {
            n = (unsigned int)(c - '0');
            while (count++ < 2 && *p >= '0' && *p <= '7') n = n * 8 + (unsigned int)(*p++ - '0');
            *value = (char)n;
        } else {
            *value = c;
            *source = p;
            return 2;
        }
    }
    *source = p;
    return 0;
}

static int parse_number(const char *text, unsigned long long *out, int signed_number)
{
    unsigned long long value = 0, limit;
    int negative = 0, base = 10, digits = 0;
    if (*text == '\'' || *text == '"') {
        *out = (unsigned char)text[1];
        return text[1] == '\0' || text[2] != '\0' ? -1 : 0;
    }
    while (*text == ' ' || *text == '\t') text++;
    if (*text == '+' || *text == '-') negative = *text++ == '-';
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { base = 16; text += 2; }
    else if (text[0] == '0') base = 8;
    limit = signed_number ? (unsigned long long)LLONG_MAX + (unsigned int)negative : ULLONG_MAX;
    while (*text != '\0') {
        int digit;
        if (*text >= '0' && *text <= '9') digit = *text - '0';
        else if (*text >= 'a' && *text <= 'f') digit = *text - 'a' + 10;
        else if (*text >= 'A' && *text <= 'F') digit = *text - 'A' + 10;
        else break;
        if (digit >= base || value > (limit - (unsigned int)digit) / (unsigned int)base) return -1;
        value = value * (unsigned int)base + (unsigned int)digit;
        text++;
        digits = 1;
    }
    while (*text == ' ' || *text == '\t') text++;
    if (!digits || *text != '\0') return -1;
    *out = negative ? 0ULL - value : value;
    return 0;
}

int shell_printf_execute(const char *format, int argc, char *const argv[],
                         shell_printf_write_fn write, void *context,
                         char *buffer, size_t buffer_size)
{
    int arg = 0, status = 0;
    if (format == NULL || write == NULL || buffer == NULL || buffer_size < 512) return 1;
    do {
        const char *p = format;
        int before = arg;
        while (*p != '\0') {
            if (*p == '\\') {
                char c;
                int rc;
                p++;
                rc = escaped(&p, &c, 0);
                if (rc < 0) return 1;
                if (rc == 2 && emit(write, context, "\\", 1) != 0) return 1;
                if (emit(write, context, &c, 1) != 0) return 1;
            } else if (*p != '%') {
                const char *start = p;
                while (*p != '\0' && *p != '%' && *p != '\\') p++;
                if (emit(write, context, start, (size_t)(p - start)) != 0) return 1;
            } else {
                char spec[32];
                size_t used = 0;
                int number = 0, n, precision = -1;
                char conversion;
                const char *value;
                p++;
                if (*p == '%') { if (emit(write, context, "%", 1) != 0) return 1; p++; continue; }
                spec[used++] = '%';
                while (*p != '\0' && strchr("-+ #0", *p) != NULL) {
                    if (used >= sizeof(spec) - 8) return 1;
                    spec[used++] = *p++;
                }
                while (*p >= '0' && *p <= '9') {
                    number = number * 10 + (*p - '0');
                    if (number > 256 || used >= sizeof(spec) - 8) return 1;
                    spec[used++] = *p++;
                }
                if (*p == '.') {
                    spec[used++] = *p++;
                    precision = 0;
                    while (*p >= '0' && *p <= '9') {
                        precision = precision * 10 + (*p - '0');
                        if (precision > 256 || used >= sizeof(spec) - 8) return 1;
                        spec[used++] = *p++;
                    }
                }
                conversion = *p;
                if (conversion == '\0' || strchr("sbcdiouxX", conversion) == NULL) return 1;
                p++;
                value = arg < argc ? argv[arg++] : "";
                if (conversion == 'b') {
                    if (used != 1 || precision >= 0) return 1;
                    while (*value != '\0') {
                        char c = *value++;
                        if (c == '\\') {
                            int rc = escaped(&value, &c, 1);
                            if (rc == 1) return status;
                            if (rc < 0) return 1;
                            if (rc == 2 && emit(write, context, "\\", 1) != 0) return 1;
                        }
                        if (emit(write, context, &c, 1) != 0) return 1;
                    }
                    continue;
                }
                if (strchr("diouxX", conversion) != NULL) {
                    unsigned long long v = 0;
                    if (*value != '\0' && parse_number(value, &v, conversion == 'd' || conversion == 'i') != 0) status = 1;
                    spec[used++] = 'l'; spec[used++] = 'l'; spec[used++] = conversion; spec[used] = '\0';
                    if (conversion == 'd' || conversion == 'i') n = snprintf(buffer, buffer_size, spec, (long long)v);
                    else n = snprintf(buffer, buffer_size, spec, v);
                } else {
                    spec[used++] = conversion; spec[used] = '\0';
                    if (conversion == 's') n = snprintf(buffer, buffer_size, spec, value);
                    else n = snprintf(buffer, buffer_size, spec, (unsigned char)*value);
                }
                if (n < 0 || (size_t)n >= buffer_size || emit(write, context, buffer, (size_t)n) != 0) return 1;
            }
        }
        if (arg == before) break;
    } while (arg < argc);
    return status;
}
