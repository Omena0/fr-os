/*
 * list.h — intrusive doubly-linked list.
 *
 * Circular, with a sentinel head. Circular means: no special case for the empty
 * list and no special case for a one-element list, and no branch on the first
 * insert. Intrusive means the link lives inside the object being tracked, so
 * there is no allocation and the kernel can put a list_head in a slab object
 * without a wrapper.
 *
 * Locking is the caller's responsibility. Every function here is correct with
 * interrupts disabled or under an external lock; none of them take one.
 */
#ifndef LIST_H
#define LIST_H

#include <types.h>

struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }

static inline void list_init(struct list_head *head)
{
    head->next = head;
    head->prev = head;
}

static inline bool list_empty(const struct list_head *head)
{
    return head->next == head;
}

static inline void __list_add(struct list_head *entry,
                  struct list_head *prev,
                  struct list_head *next)
{
    next->prev = entry;
    entry->next = next;
    entry->prev = prev;
    prev->next = entry;
}

/* Insert immediately after `pos`. */
static inline void list_add(struct list_head *entry, struct list_head *pos)
{
    __list_add(entry, pos, pos->next);
}

/* Insert immediately before `pos`, i.e. at the end of the list. */
static inline void list_add_tail(struct list_head *entry, struct list_head *pos)
{
    __list_add(entry, pos->prev, pos);
}

static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    prev->next = next;
}

static inline void list_del(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    entry->next = entry;
    entry->prev = entry;
}

static inline void list_del_init(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    list_init(entry);
}

/* Splice an entire list in after `pos`, leaving `head` empty. */
static inline void list_splice_init(struct list_head *head,
                    struct list_head *pos)
{
    if (list_empty(head))
        return;
    struct list_head *first = head->next;
    struct list_head *last = head->prev;
    struct list_head *prev = pos;

    first->prev = prev;
    prev->next = first;
    last->next = pos;
    pos->prev = last;
    list_init(head);
}

/* Move one entry from wherever it is to the tail of `dst`. */
static inline void list_move(struct list_head *entry, struct list_head *dst)
{
    list_del_init(entry);
    list_add_tail(entry, dst);
}

/* Move every entry from `src` onto the tail of `dst`. */
static inline void list_move_all(struct list_head *src, struct list_head *dst)
{
    list_splice_init(src, dst);
}

#define list_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

/*
 * Safe iteration: `pos` is saved before the body so the current element can be
 * freed inside the loop. Without this, list_del() during iteration would leave
 * the cursor pointing at freed memory.
 */
#define list_for_each_safe(pos, tmp, head) \
    for (pos = (head)->next, tmp = pos->next; pos != (head); \
         pos = tmp, tmp = pos->next)

#endif /* LIST_H */
