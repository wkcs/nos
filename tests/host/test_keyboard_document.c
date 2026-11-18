/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <string.h>
#include <nos/keyboard_document.h>
int main(void)
{
    uint8_t body[64] = {1,0,2,0,0,0, 5,0,1,0,7};
    uint8_t replacement[] = {1,0,3,0,1,'x',0};
    size_t length = 11;
    struct kb_doc_reader r;
    uint16_t tag;
    struct kb_doc_reader value;
    kb_doc_reader_init(&r, body, length);
    assert(kb_doc_next(&r, &tag, &value) == 1 && tag == 1 && value.left == 2);
    assert(kb_doc_next(&r, &tag, &value) == 1 && tag == 5);
    assert(kb_doc_next(&r, &tag, &value) == 0);
    assert(kb_doc_splice(body, &length, sizeof(body), 1, 0, replacement, sizeof(replacement)) < 0);
    assert(length == 11);
    assert(kb_doc_splice(body, &length, sizeof(body), 0, 6, replacement, sizeof(replacement)) == 0);
    assert(length == 12 && body[6] == 0 && body[7] == 5);
    replacement[2] = 33;
    assert(kb_doc_splice(body, &length, sizeof(body), 0, 0, replacement, sizeof(replacement)) < 0);
    assert(length == 12);
    body[2] = 0xff;
    kb_doc_reader_init(&r, body, length);
    assert(kb_doc_next(&r, &tag, &value) < 0);
    char text[8] = "unsafe";
    uint8_t malformed[] = {7, 'x'};
    kb_doc_reader_init(&r, malformed, sizeof(malformed));
    assert(kb_doc_string(&r, text, sizeof(text)) < 0 && text[0] == 0);
    uint8_t invalid_utf8[] = {3,0xed,0xa0,0x80};
    kb_doc_reader_init(&r, invalid_utf8, sizeof(invalid_utf8));
    assert(kb_doc_string(&r, text, sizeof(text)) < 0 && text[0] == 0);
    {
        struct kb_doc_writer measured, encoded;
        uint8_t expected[32];
        size_t rec;
        kb_doc_writer_init(&measured, NULL, sizeof(expected));
        rec=kb_doc_record(&measured,7);kb_doc_str(&measured,"sized");kb_doc_end(&measured,rec);
        assert(!measured.error && measured.used==10);
        kb_doc_writer_init(&encoded,expected,measured.used);
        rec=kb_doc_record(&encoded,7);kb_doc_str(&encoded,"sized");kb_doc_end(&encoded,rec);
        assert(!encoded.error && encoded.used==measured.used && expected[2]==6);
    }
    return 0;
}
