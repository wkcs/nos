/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
#include <stddef.h>
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define INIT_LIST_HEAD(h) ((h)->next = (h)->prev = (h))
static inline void list_add_tail(struct list_head *e, struct list_head *h) { e->next = h; e->prev = h->prev; h->prev->next = e; h->prev = e; }
static inline void list_del(struct list_head *e) { e->prev->next = e->next; e->next->prev = e->prev; INIT_LIST_HEAD(e); }
static inline void list_del_init(struct list_head *e) { list_del(e); }
static inline int list_empty(const struct list_head *h) { return h->next == h; }
#define list_entry(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define list_first_entry(h,t,m) list_entry((h)->next,t,m)
#define list_for_each_entry(p,h,m) for (p = list_entry((h)->next, __typeof__(*p), m); &p->m != (h); p = list_entry(p->m.next, __typeof__(*p), m))
