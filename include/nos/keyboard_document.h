/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Bounded, endian-independent keyboard document and NLK wire primitives.
 */
#ifndef NOS_KEYBOARD_DOCUMENT_H
#define NOS_KEYBOARD_DOCUMENT_H
#include <stddef.h>
#include <stdint.h>
struct kb_doc_reader {
    const uint8_t *data;
    size_t left;
    int error;
};
struct kb_doc_writer {
    uint8_t *data;
    size_t size, used;
    size_t page_offset, page_size;
    int error;
};
void kb_doc_reader_init(struct kb_doc_reader *r, const void *data, size_t size);
uint64_t kb_doc_uint(struct kb_doc_reader *r, unsigned int bytes);
int kb_doc_string(struct kb_doc_reader *r, char *out, size_t capacity);
const uint8_t *kb_doc_bytes(struct kb_doc_reader *r, size_t length);
int kb_doc_next(struct kb_doc_reader *r, uint16_t *tag, struct kb_doc_reader *value);
/* A NULL buffer performs bounded size measurement without allocation. */
void kb_doc_writer_init(struct kb_doc_writer *w, void *data, size_t capacity);
/* Emit only the requested byte window while tracking the full encoded length. */
void kb_doc_page_init(struct kb_doc_writer *w, void *data, size_t size, size_t offset,
                      size_t total_limit);
void kb_doc_patch_uint(struct kb_doc_writer *w, size_t offset, uint64_t value,
                       unsigned int bytes);
void kb_doc_put(struct kb_doc_writer *w, uint64_t value, unsigned int bytes);
void kb_doc_write(struct kb_doc_writer *w, const void *data, size_t length);
void kb_doc_str(struct kb_doc_writer *w, const char *value);
size_t kb_doc_record(struct kb_doc_writer *w, uint16_t tag);
void kb_doc_end(struct kb_doc_writer *w, size_t record);
int kb_doc_splice(void *body, size_t *length, size_t capacity, size_t offset, size_t remove,
                  const void *insert, size_t insert_length);
#endif
