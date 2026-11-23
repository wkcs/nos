/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS EL0 轻量级零 libc 运行库（LAZY_ALLOC VMO 堆分配器、字符串与格式化打印）
 */

#ifndef __NOS_USER_ULIBC_H__
#define __NOS_USER_ULIBC_H__

#include <nos/user_ai.h>

#define NOS_ULIBC_HEAP_BASE_VA  0x0000000000600000UL
#define NOS_ULIBC_HEAP_SIZE     (64UL * 1024UL)
#define NOS_ULIBC_ALIGN_UP(x)   (((x) + 15UL) & ~15UL)

struct ulibc_blk_hdr {
    size_t                size;
    uint32_t              is_free;
    uint32_t              magic;
    struct ulibc_blk_hdr *next;
};

#define NOS_ULIBC_BLK_MAGIC     0x5548424CU /* "UHBL" */

static inline struct ulibc_blk_hdr **ulibc_heap_head_slot(void)
{
    static struct ulibc_blk_hdr *g_heap_head = NULL;
    return &g_heap_head;
}

static inline void *umemset(void *dst, int c, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    uint8_t v = (uint8_t)c;

    for (size_t i = 0U; i < n; i++) {
        p[i] = v;
    }
    return dst;
}

static inline void *umemcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    for (size_t i = 0U; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

static inline int umemcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;

    for (size_t i = 0U; i < n; i++) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

static inline size_t ustrlen(const char *s)
{
    size_t n = 0U;

    if (s == NULL) {
        return 0U;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static inline int ustrcmp(const char *a, const char *b)
{
    if (a == b) {
        return 0;
    }
    if (a == NULL) {
        return -1;
    }
    if (b == NULL) {
        return 1;
    }
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static inline void uputs(const char *s)
{
    nos_user_puts(s);
}

static inline void uprint_u64(uint64_t val)
{
    char buf[24];
    size_t idx = 0U;

    if (val == 0ULL) {
        (void)nos_user_console_write("0", 1U);
        return;
    }
    while (val > 0ULL && idx < sizeof(buf)) {
        buf[idx++] = (char)('0' + (val % 10ULL));
        val /= 10ULL;
    }
    for (size_t i = 0U; i < idx / 2U; i++) {
        char tmp = buf[i];
        buf[i] = buf[idx - 1U - i];
        buf[idx - 1U - i] = tmp;
    }
    (void)nos_user_console_write(buf, idx);
}

static inline void uprint_i64(int64_t val)
{
    if (val < 0) {
        uint64_t mag;
        (void)nos_user_console_write("-", 1U);
        mag = (uint64_t)(-(val + 1)) + 1ULL;
        uprint_u64(mag);
        return;
    }
    uprint_u64((uint64_t)val);
}

static inline void uprint_hex64(uint64_t val)
{
    static const char hex[] = "0123456789abcdef";
    char buf[18];
    size_t pos = 0U;

    buf[0] = '0';
    buf[1] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4) {
        buf[2U + pos] = hex[(val >> shift) & 0xFULL];
        pos++;
    }
    (void)nos_user_console_write(buf, 18U);
}

static inline int ulibc_heap_init_once(void)
{
    struct ulibc_blk_hdr **head = ulibc_heap_head_slot();
    nos_handle_t heap_vmo = NOS_HANDLE_INVALID;
    struct ulibc_blk_hdr *first;
    int rc;

    if (*head != NULL) {
        return 0;
    }

    rc = nos_user_vmo_create(NOS_ULIBC_HEAP_SIZE,
                             NOS_VMO_ANON,
                             NOS_VMO_FLAG_LAZY_ALLOC,
                             &heap_vmo);
    if (rc != 0) {
        return rc;
    }
    rc = nos_user_vmo_map(heap_vmo,
                          NOS_HANDLE_INVALID,
                          NOS_ULIBC_HEAP_BASE_VA,
                          NOS_ULIBC_HEAP_SIZE,
                          NOS_USER_PROT_READ | NOS_USER_PROT_WRITE);
    if (rc != 0) {
        return rc;
    }

    /* 首次写块头将触发 LAZY_ALLOC 按需物理页提交 */
    first = (struct ulibc_blk_hdr *)(uintptr_t)NOS_ULIBC_HEAP_BASE_VA;
    first->size = NOS_ULIBC_HEAP_SIZE - NOS_ULIBC_ALIGN_UP(sizeof(*first));
    first->is_free = 1U;
    first->magic = NOS_ULIBC_BLK_MAGIC;
    first->next = NULL;
    *head = first;
    return 0;
}

static inline void *umalloc(size_t size)
{
    struct ulibc_blk_hdr **head = ulibc_heap_head_slot();
    struct ulibc_blk_hdr *cur;
    size_t hdr_sz = NOS_ULIBC_ALIGN_UP(sizeof(struct ulibc_blk_hdr));
    size_t need;

    if (size == 0U || ulibc_heap_init_once() != 0) {
        return NULL;
    }
    need = NOS_ULIBC_ALIGN_UP(size);

    for (cur = *head; cur != NULL; cur = cur->next) {
        if (cur->magic != NOS_ULIBC_BLK_MAGIC) {
            return NULL;
        }
        if (cur->is_free != 0U && cur->size >= need) {
            if (cur->size >= need + hdr_sz + 16U) {
                struct ulibc_blk_hdr *split =
                    (struct ulibc_blk_hdr *)((uint8_t *)cur + hdr_sz + need);
                split->size = cur->size - need - hdr_sz;
                split->is_free = 1U;
                split->magic = NOS_ULIBC_BLK_MAGIC;
                split->next = cur->next;
                cur->size = need;
                cur->next = split;
            }
            cur->is_free = 0U;
            return (void *)((uint8_t *)cur + hdr_sz);
        }
    }
    return NULL;
}

static inline void ufree(void *ptr)
{
    struct ulibc_blk_hdr **head = ulibc_heap_head_slot();
    struct ulibc_blk_hdr *blk;
    struct ulibc_blk_hdr *cur;
    size_t hdr_sz = NOS_ULIBC_ALIGN_UP(sizeof(struct ulibc_blk_hdr));

    if (ptr == NULL || *head == NULL) {
        return;
    }
    blk = (struct ulibc_blk_hdr *)((uint8_t *)ptr - hdr_sz);
    if (blk->magic != NOS_ULIBC_BLK_MAGIC) {
        return;
    }
    blk->is_free = 1U;

    /* 合并相邻空闲块 */
    cur = *head;
    while (cur != NULL && cur->next != NULL) {
        if (cur->is_free != 0U && cur->next->is_free != 0U &&
            ((uint8_t *)cur + hdr_sz + cur->size == (uint8_t *)cur->next)) {
            cur->size += hdr_sz + cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

static inline void *ucalloc(size_t nmemb, size_t size)
{
    size_t total;
    void *p;

    if (nmemb == 0U || size == 0U || nmemb > ((size_t)-1) / size) {
        return NULL;
    }
    total = nmemb * size;
    p = umalloc(total);
    if (p != NULL) {
        umemset(p, 0, total);
    }
    return p;
}

#endif /* __NOS_USER_ULIBC_H__ */
