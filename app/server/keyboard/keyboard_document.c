/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Never cast incoming bytes to structs: NLK and files share a little-endian ABI.
 */

#include <nos/keyboard_document.h>
#include <string.h>
#include <uapi/nos/errno.h>

void kb_doc_reader_init(struct kb_doc_reader *r, const void *data, size_t size)
{
    r->data = data;
    r->left = size;
    r->error = 0;
}

const uint8_t *kb_doc_bytes(struct kb_doc_reader *r, size_t length)
{
    const uint8_t *result = r->data;
    if (r->error || length > r->left) {
        r->error = -EINVAL;
        return NULL;
    }
    r->data += length;
    r->left -= length;
    return result;
}

uint64_t kb_doc_uint(struct kb_doc_reader *r, unsigned int bytes)
{
    uint64_t result = 0;
    const uint8_t *p;
    unsigned int i;
    if (bytes > 8) {
        r->error = -EINVAL;
        return 0;
    }
    p = kb_doc_bytes(r, bytes);
    if (p != NULL) {
        for (i = 0; i < bytes; i++) {
            result |= (uint64_t)p[i] << (8 * i);
        }
    }
    return result;
}

static int valid_utf8(const uint8_t *p, size_t length)
{
    size_t i = 0;
    while (i < length) {
        uint32_t code, minimum;
        unsigned int remaining;
        uint8_t first = p[i++];
        if (first < 0x80) {
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            remaining = 1;
            minimum = 0x80;
            code = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            remaining = 2;
            minimum = 0x800;
            code = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            remaining = 3;
            minimum = 0x10000;
            code = first & 7;
        } else {
            return 0;
        }
        if (remaining > length - i) {
            return 0;
        }
        while (remaining--) {
            if ((p[i] & 0xc0) != 0x80) {
                return 0;
            }
            code = (code << 6) | (p[i++] & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
            return 0;
        }
    }
    return 1;
}

int kb_doc_string(struct kb_doc_reader *r, char *out, size_t capacity)
{
    if (capacity != 0) {
        out[0] = 0;
    }
    size_t length = kb_doc_uint(r, 1);
    const uint8_t *p = kb_doc_bytes(r, length);
    if (p == NULL || length >= capacity || memchr(p, 0, length) != NULL || !valid_utf8(p, length)) {
        r->error = -EINVAL;
        return r->error;
    }
    memcpy(out, p, length);
    out[length] = 0;
    return 0;
}

int kb_doc_next(struct kb_doc_reader *r, uint16_t *tag, struct kb_doc_reader *value)
{
    size_t length;
    const uint8_t *p;
    if (r->error) {
        return r->error;
    }
    if (r->left == 0) {
        return 0;
    }
    *tag = kb_doc_uint(r, 2);
    length = kb_doc_uint(r, 2);
    p = kb_doc_bytes(r, length);
    if (p == NULL) {
        return -EINVAL;
    }
    kb_doc_reader_init(value, p, length);
    return 1;
}

void kb_doc_writer_init(struct kb_doc_writer *w, void *data, size_t capacity)
{
    w->data = data;
    w->size = capacity;
    w->used = 0;
    w->page_offset = 0;
    w->page_size = capacity;
    w->error = 0;
}

void kb_doc_page_init(struct kb_doc_writer *w, void *data, size_t size, size_t offset,
                      size_t total_limit)
{
    kb_doc_writer_init(w, data, total_limit);
    w->page_offset = offset;
    w->page_size = size;
}

static void kb_doc_copy(struct kb_doc_writer *w, size_t offset, const void *data, size_t length)
{
    size_t skip, start, count;

    if (w->data == NULL || length == 0 || offset + length <= w->page_offset) {
        return;
    }
    skip = offset < w->page_offset ? w->page_offset - offset : 0;
    start = offset + skip - w->page_offset;
    if (start >= w->page_size) {
        return;
    }
    count = length - skip;
    if (count > w->page_size - start) {
        count = w->page_size - start;
    }
    memcpy(w->data + start, (const uint8_t *)data + skip, count);
}

void kb_doc_write(struct kb_doc_writer *w, const void *data, size_t length)
{
    if (w->error || length > w->size - w->used) {
        w->error = -ENOSPC;
        return;
    }
    kb_doc_copy(w, w->used, data, length);
    w->used += length;
}

void kb_doc_patch_uint(struct kb_doc_writer *w, size_t offset, uint64_t value, unsigned int bytes)
{
    uint8_t encoded[sizeof(value)];
    unsigned int i;

    if (w->error) {
        return;
    }
    if (bytes > sizeof(encoded) || offset > w->used || bytes > w->used - offset) {
        w->error = -EINVAL;
        return;
    }
    for (i = 0; i < bytes; i++) {
        encoded[i] = value >> (8 * i);
    }
    kb_doc_copy(w, offset, encoded, bytes);
}

void kb_doc_put(struct kb_doc_writer *w, uint64_t value, unsigned int bytes)
{
    uint8_t encoded[sizeof(value)];
    unsigned int i;

    if (bytes > sizeof(encoded)) {
        w->error = -EINVAL;
        return;
    }
    for (i = 0; i < bytes; i++) {
        encoded[i] = value >> (8 * i);
    }
    kb_doc_write(w, encoded, bytes);
}

void kb_doc_str(struct kb_doc_writer *w, const char *value)
{
    size_t length = strlen(value);
    if (length > 255) {
        w->error = -EINVAL;
        return;
    }
    kb_doc_put(w, length, 1);
    kb_doc_write(w, value, length);
}
size_t kb_doc_record(struct kb_doc_writer *w, uint16_t tag)
{
    size_t start = w->used;
    kb_doc_put(w, tag, 2);
    kb_doc_put(w, 0, 2);
    return start;
}

void kb_doc_end(struct kb_doc_writer *w, size_t record)
{
    size_t length;
    if (w->error) {
        return;
    }
    length = w->used - record - 4;
    if (length > 65535) {
        w->error = -EINVAL;
        return;
    }
    kb_doc_patch_uint(w, record + sizeof(uint16_t), length, sizeof(uint16_t));
}

static int kb_doc_boundary(const uint8_t *body, size_t length, size_t offset)
{
    struct kb_doc_reader r, value;
    uint16_t tag;
    kb_doc_reader_init(&r, body, length);
    while (length - r.left < offset && kb_doc_next(&r, &tag, &value) > 0) {
    }
    return !r.error && length - r.left == offset;
}

int kb_doc_splice(void *body, size_t *length, size_t capacity, size_t offset, size_t remove,
                  const void *insert, size_t insert_length)
{
    uint8_t *p = body;
    if (*length > capacity || offset > *length || remove > *length - offset ||
        insert_length > capacity - (*length - remove) || !kb_doc_boundary(p, *length, offset) ||
        !kb_doc_boundary(p, *length, offset + remove) ||
        !kb_doc_boundary(insert, insert_length, insert_length)) {
        return -EINVAL;
    }
    memmove(p + offset + insert_length, p + offset + remove, *length - offset - remove);
    if (insert_length != 0) {
        memcpy(p + offset, insert, insert_length);
    }
    *length = *length - remove + insert_length;
    return 0;
}
