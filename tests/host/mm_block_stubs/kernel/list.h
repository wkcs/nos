/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
#include <stddef.h>
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD(n) struct list_head n = { &(n), &(n) }
#define INIT_LIST_HEAD(h) ((h)->next = (h)->prev = (h))
static inline void list_add_tail(struct list_head *e, struct list_head *h) { e->next = h; e->prev = h->prev; h->prev->next = e; h->prev = e; }
static inline void list_add(struct list_head *e, struct list_head *h) { list_add_tail(e, h->next); }
static inline void list_del(struct list_head *e) { e->prev->next = e->next; e->next->prev = e->prev; e->next = e->prev = e; }
static inline int list_empty(const struct list_head *h) { return h->next == h; }
#define list_entry(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define list_first_entry(h,t,m) list_entry((h)->next,t,m)
#define list_for_each(pos, head) for (pos = (head)->next; pos != (head); pos = pos->next)
#define list_for_each_safe(pos, n, head) for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)
#define list_for_each_entry(p,h,m) for (struct list_head *_pos = (h)->next; _pos != (h) && ((p) = (void *)((char *)_pos - offsetof(__typeof__(*(p)), m)), 1); _pos = _pos->next)
#define list_prev_entry(p,m) list_entry((p)->m.prev,__typeof__(*(p)),m)
#define list_next_entry(p,m) list_entry((p)->m.next,__typeof__(*(p)),m)
