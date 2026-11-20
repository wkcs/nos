#!/usr/bin/env python3
"""在 -fsigned-char 下验证 libs/string.c 的无符号字符比较与 UTF-8 字符串排序行为。"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

MM_H = """#ifndef NOS_STRING_TEST_MM_H
#define NOS_STRING_TEST_MM_H
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef uint8_t u8;
typedef uint64_t u64;
typedef unsigned int gfp_t;
#define GFP_KERNEL 1U
#define BUG_ON(cond) assert(!(cond))
#ifndef __visible
#define __visible
#endif
static inline void *kmalloc(size_t size, gfp_t flags)
{
    (void)flags;
    return malloc(size);
}
#endif
"""

ERRNO_H = """#ifndef NOS_STRING_TEST_ERRNO_H
#define NOS_STRING_TEST_ERRNO_H
#include <errno.h>
#endif
"""

TEST_C = r"""
#include <assert.h>
#include <stddef.h>

int nos_strcmp(const char *cs, const char *ct);
int nos_strncmp(const char *cs, const char *ct, size_t count);

int main(void)
{
    /* 在 -fsigned-char 下，"测试配置.nkc" 首字节 0xe6 不得被解释为负数 */
    assert(nos_strcmp("\xe6\xb5\x8b\xe8\xaf\x95\xe9\x85\x8d\xe7\xbd\xae.nkc", "") > 0);
    assert(nos_strcmp("\xe6\xb5\x8b\xe8\xaf\x95\xe9\x85\x8d\xe7\xbd\xae.nkc", "pass.nkc") > 0);
    assert(nos_strcmp("pass.nkc", "\xe6\xb5\x8b\xe8\xaf\x95\xe9\x85\x8d\xe7\xbd\xae.nkc") < 0);
    assert(nos_strcmp("", "\xe6\xb5\x8b\xe8\xaf\x95\xe9\x85\x8d\xe7\xbd\xae.nkc") < 0);
    assert(nos_strcmp("\x80", "\x7f") > 0);
    assert(nos_strcmp("\xff", "\x01") > 0);
    assert(nos_strcmp("\xff", "\xff") == 0);

    /* strncmp 需同步按 unsigned char 比较，且差值 > 127 不得截断为 signed char 负值 */
    assert(nos_strncmp("\xff", "\x01", 1) > 0);
    assert(nos_strncmp("\x01", "\xff", 1) < 0);
    assert(nos_strncmp("\xe6\xb5\x8b", "", 3) > 0);
    assert(nos_strncmp("abc", "abd", 2) == 0);
    assert(nos_strncmp("a", "b", 0) == 0);
    return 0;
}
"""

REMAP_MACROS = [
    "-Dstrcat=nos_strcat",
    "-Dstrncat=nos_strncat",
    "-Dstrlcat=nos_strlcat",
    "-Dstrlen=nos_strlen",
    "-Dmemcpy=nos_memcpy",
    "-Dmemset=nos_memset",
    "-Dmemmove=nos_memmove",
    "-Dmemscan=nos_memscan",
    "-Dstrstr=nos_strstr",
    "-Dstrnstr=nos_strnstr",
    "-Dmemchr=nos_memchr",
    "-Dstrncmp=nos_strncmp",
    "-Dstrcmp=nos_strcmp",
    "-Dstrchr=nos_strchr",
    "-Dstrpbrk=nos_strpbrk",
    "-Dstrcpy=nos_strcpy",
    "-Dstrncpy=nos_strncpy",
    "-Dstrspn=nos_strspn",
    "-Dstrnlen=nos_strnlen",
    "-Dstrcoll=nos_strcoll",
    "-Dstrerror=nos_strerror",
    "-Dkstrdup=nos_kstrdup",
]


def main():
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory(prefix="nos-string-cmp-") as tmpdir:
        tmp = Path(tmpdir)
        kdir = tmp / "kernel"
        kdir.mkdir()
        (kdir / "mm.h").write_text(MM_H, encoding="utf-8")
        (kdir / "errno.h").write_text(ERRNO_H, encoding="utf-8")
        test_c = tmp / "test_string_cmp.c"
        test_c.write_text(TEST_C, encoding="utf-8")
        str_obj = tmp / "string.o"
        exe = tmp / "test_string_cmp"

        subprocess.run(
            [
                cc, "-std=gnu11", "-fsigned-char", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0",
                "-Wall", "-Wextra", "-Werror", "-fno-builtin",
                *REMAP_MACROS,
                "-I", str(tmp),
                "-c", str(ROOT / "libs/string.c"),
                "-o", str(str_obj),
            ],
            check=True,
        )
        subprocess.run(
            [cc, "-std=gnu11", "-fsigned-char", "-Wall", "-Wextra", "-Werror",
             str(test_c), str(str_obj), "-o", str(exe)],
            check=True,
        )
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
