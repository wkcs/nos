/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#pragma once
#include <stddef.h>
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD(n) struct list_head n = { &(n), &(n) }
#define INIT_LIST_HEAD(h) ((h)->next = (h)->prev = (h))
static inline void list_add_tail(struct list_head *e, struct list_head *h) { e->next = h; e->prev = h->prev; h->prev->next = e; h->prev = e; }
#define list_entry(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define list_first_entry(h,t,m) list_entry((h)->next,t,m)
#define list_for_each_entry(p,h,m) for (struct list_head *_p=(h)->next; _p!=(h) && ((p)=list_entry(_p,__typeof__(*(p)),m),1); _p=_p->next)
static inline void list_splice(struct list_head *list, struct list_head *head) { if (list->next != list) { struct list_head *first=list->next, *last=list->prev, *next=head->next; first->prev=head; head->next=first; last->next=next; next->prev=last; } }
static inline void list_cut_position(struct list_head *out, struct list_head *head, struct list_head *entry) { struct list_head *first=head->next, *next=entry->next; out->next=first; first->prev=out; out->prev=entry; entry->next=out; head->next=next; next->prev=head; }
