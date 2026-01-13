#ifndef __NOS_KALLSYMS_H__
#define __NOS_KALLSYMS_H__

/* 
 * 打印符号信息
 * fmt: 格式化字符串，必须包含一个 %s 用于打印符号信息
 * addr: 地址
 */
void print_symbol(const char *fmt, unsigned long addr);

struct kallsym_entry {
    unsigned long addr;
    const char *name;
};

extern const struct kallsym_entry kallsyms_table[];
extern const unsigned int kallsyms_num;

const char *kallsyms_lookup(unsigned long addr, unsigned long *symbolsize,
                           unsigned long *offset, char **modname, char *namebuf);

#endif /* __NOS_KALLSYMS_H__ */
