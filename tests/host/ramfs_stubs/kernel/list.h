#pragma once
#include <stddef.h>
struct list_head { struct list_head *next, *prev; };
#define INIT_LIST_HEAD(h) ((h)->next = (h)->prev = (h))
static inline void list_add_tail(struct list_head *e, struct list_head *h) {
    e->next = h; e->prev = h->prev; h->prev->next = e; h->prev = e;
}
static inline void list_del(struct list_head *e) {
    e->prev->next = e->next; e->next->prev = e->prev;
    e->next = e->prev = e;
}
static inline int list_empty(const struct list_head *h) {
    return h->next == h;
}
#define list_for_each_entry(p,h,m) \
    for (struct list_head *_pos = (h)->next; \
         _pos != (h) && ((p) = (void *)((char *)_pos - offsetof(__typeof__(*(p)), m)), 1); \
         _pos = _pos->next)
