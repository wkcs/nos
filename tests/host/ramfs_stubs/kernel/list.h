#pragma once
#include <stddef.h>
struct list_head { struct list_head *next, *prev; };
#define INIT_LIST_HEAD(h) ((h)->next = (h)->prev = (h))
static inline void list_add_tail(struct list_head *e, struct list_head *h) {
    e->next = h; e->prev = h->prev; h->prev->next = e; h->prev = e;
}
#define list_for_each_entry(p,h,m) \
    for (struct list_head *_pos = (h)->next; \
         _pos != (h) && ((p) = (void *)((char *)_pos - offsetof(__typeof__(*(p)), m)), 1); \
         _pos = _pos->next)
