/* Copyright (C) 2026 NOS contributors */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "shell.h"

static uint32_t random_state = 0x51e11u;

static uint32_t next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void check_parse(const char *input, int expected, const char *const *expected_words)
{
    char line[NOS_SHELL_LINE_BYTES];
    char *words[NOS_SHELL_WORDS + 1];
    assert(strlen(input) < sizeof(line));
    strcpy(line, input);
    int count = nos_shell_parse(line, words, NOS_SHELL_WORDS);
    assert(count == expected);
    if (count < 0)
        return;
    assert(words[count] == NULL);
    for (int i = 0; i < count; i++)
        assert(strcmp(words[i], expected_words[i]) == 0);
}

static void parser_cases(void)
{
    const char *plain[] = {"write", "/a", "two words", "single words", "escaped value", "", "ab", "c\"d", "\\"};
    check_parse(" \twrite /a \"two words\" 'single words' escaped\\ value \"\" a\"\"b \"c\\\"d\" \\\\ ", 9, plain);
    const char *quoted[] = {"echo", "a\\b", "a b", "a'b"};
    check_parse("echo 'a\\b' a\\ b \"a'b\"", 4, quoted);
    check_parse("\t  ", 0, NULL);
    check_parse("echo 'missing", NOS_SHELL_PARSE_QUOTE, NULL);
    check_parse("echo \"missing", NOS_SHELL_PARSE_QUOTE, NULL);
    check_parse("echo trailing\\", NOS_SHELL_PARSE_ESCAPE, NULL);
    check_parse("a b c d e f g h i j", NOS_SHELL_PARSE_ARGS, NULL);
    puts("PASS shell_parser_quotes_escapes_empty_and_limits");
}

static void parser_fuzz(void)
{
    struct guarded {
        uint64_t before;
        char line[NOS_SHELL_LINE_BYTES];
        uint64_t after;
    } data;
    struct arguments {
        uint64_t before;
        char *words[NOS_SHELL_WORDS + 1];
        uint64_t after;
    } args;
    for (unsigned iteration = 0; iteration < 100000; iteration++) {
        data.before = data.after = args.before = args.after = UINT64_C(0xfedcba9876543210);
        unsigned length = next_random() % sizeof(data.line);
        static const char alphabet[] = " abcdef\t\\\"'0123";
        for (unsigned i = 0; i < length; i++)
            data.line[i] = alphabet[next_random() % (sizeof(alphabet) - 1)];
        data.line[length] = 0;
        unsigned capacity = next_random() % (NOS_SHELL_WORDS + 1);
        int count = nos_shell_parse(data.line, args.words, capacity);
        assert(count >= NOS_SHELL_PARSE_ARGS && count <= (int)capacity);
        if (count >= 0) {
            assert(args.words[count] == NULL);
            for (int i = 0; i < count; i++) {
                assert(args.words[i] >= data.line && args.words[i] <= data.line + length);
                assert(memchr(args.words[i], 0, (size_t)(data.line + length + 1 - args.words[i])));
            }
        }
        assert(data.before == UINT64_C(0xfedcba9876543210));
        assert(data.after == data.before && args.before == data.before && args.after == data.before);
    }
    puts("PASS shell_parser_bounded_fuzz_100000");
}

static void parser_roundtrip(void)
{
    for (unsigned iteration = 0; iteration < 10000; iteration++) {
        char line[NOS_SHELL_LINE_BYTES], expected[NOS_SHELL_WORDS][13];
        char *words[NOS_SHELL_WORDS + 1];
        unsigned used = 0, count = 1 + next_random() % NOS_SHELL_WORDS;
        for (unsigned argument = 0; argument < count; argument++) {
            if (argument) line[used++] = ' ';
            line[used++] = '"';
            unsigned length = next_random() % sizeof(expected[argument]);
            for (unsigned i = 0; i < length; i++) {
                static const char alphabet[] = " abc\\'\"";
                char byte = alphabet[next_random() % (sizeof(alphabet) - 1)];
                expected[argument][i] = byte;
                if (byte == '\\' || byte == '"') line[used++] = '\\';
                line[used++] = byte;
            }
            expected[argument][length] = 0;
            line[used++] = '"';
        }
        assert(used < sizeof(line));
        line[used] = 0;
        assert(nos_shell_parse(line, words, NOS_SHELL_WORDS) == (int)count);
        assert(words[count] == NULL);
        for (unsigned i = 0; i < count; i++)
            assert(strcmp(words[i], expected[i]) == 0);
    }
    puts("PASS shell_parser_quoted_roundtrip_10000");
}

static size_t emitted;

static void discard_output(void *context, const char *bytes, size_t count)
{
    assert(context == &emitted);
    for (size_t i = 0; i < count; i++)
        emitted += (unsigned char)bytes[i] + 1u;
}

static enum nos_shell_event feed(struct nos_shell_editor *editor, unsigned char character)
{
    enum nos_shell_event event = nos_shell_editor_feed(editor, character, discard_output, &emitted);
    assert(editor->length < NOS_SHELL_LINE_BYTES);
    assert(editor->cursor <= editor->length);
    assert(editor->history_count <= NOS_SHELL_HISTORY);
    assert(editor->line[editor->length] == 0);
    return event;
}

static void type(struct nos_shell_editor *editor, const char *bytes)
{
    while (*bytes)
        assert(feed(editor, (unsigned char)*bytes++) == NOS_SHELL_EVENT_NONE);
}

static void editor_cases(void)
{
    struct nos_shell_editor editor;
    nos_shell_editor_init(&editor);
    type(&editor, "abde\033[D\033[Dc");
    assert(strcmp(editor.line, "abcde") == 0);
    type(&editor, "\033[H>");
    type(&editor, "\033[F<");
    assert(strcmp(editor.line, ">abcde<") == 0);
    type(&editor, "\033[D\033[3~");
    assert(strcmp(editor.line, ">abcde") == 0);
    assert(feed(&editor, 127) == NOS_SHELL_EVENT_NONE);
    assert(strcmp(editor.line, ">abcd") == 0);
    assert(feed(&editor, '\r') == NOS_SHELL_EVENT_LINE);
    assert(strcmp(editor.line, ">abcd") == 0);
    assert(feed(&editor, '\n') == NOS_SHELL_EVENT_NONE);
    type(&editor, "draft\033[A");
    assert(strcmp(editor.line, ">abcd") == 0);
    type(&editor, "\033[B");
    assert(strcmp(editor.line, "draft") == 0);
    assert(feed(&editor, 3) == NOS_SHELL_EVENT_CANCEL);
    type(&editor, "echo\tseparated");
    assert(strcmp(editor.line, "echo separated") == 0);
    assert(feed(&editor, 3) == NOS_SHELL_EVENT_CANCEL);
    type(&editor, "discard\025clean");
    assert(strcmp(editor.line, "clean") == 0);
    assert(feed(&editor, '\n') == NOS_SHELL_EVENT_LINE);
    puts("PASS shell_editor_insert_delete_navigation_history_crlf_cancel");

    for (unsigned i = 0; i < NOS_SHELL_HISTORY + 3; i++) {
        char item[16];
        snprintf(item, sizeof(item), "item%u", i);
        type(&editor, item);
        assert(feed(&editor, '\n') == NOS_SHELL_EVENT_LINE);
    }
    assert(editor.history_count == NOS_SHELL_HISTORY);
    for (unsigned offset = 0; offset < NOS_SHELL_HISTORY; offset++) {
        char item[16];
        type(&editor, "\033[A");
        snprintf(item, sizeof(item), "item%u", NOS_SHELL_HISTORY + 2 - offset);
        assert(strcmp(editor.line, item) == 0);
    }
    type(&editor, "\033[A");
    assert(strcmp(editor.line, "item3") == 0);
    for (unsigned offset = 0; offset < NOS_SHELL_HISTORY + 1; offset++)
        type(&editor, "\033[B");
    assert(editor.line[0] == 0);
    for (unsigned i = 0; i < NOS_SHELL_LINE_BYTES + 20; i++)
        assert(feed(&editor, 'x') == NOS_SHELL_EVENT_NONE);
    assert(feed(&editor, '\n') == NOS_SHELL_EVENT_TOO_LONG);
    type(&editor, "recovered");
    assert(feed(&editor, '\n') == NOS_SHELL_EVENT_LINE);
    assert(strcmp(editor.line, "recovered") == 0);
    puts("PASS shell_editor_history_bound_overflow_and_recovery");
}

static void editor_fuzz(void)
{
    struct guarded {
        uint64_t before;
        struct nos_shell_editor editor;
        uint64_t after;
    } guarded = {.before = UINT64_C(0xfeedface12345678), .after = UINT64_C(0xfeedface12345678)};
    nos_shell_editor_init(&guarded.editor);
    for (unsigned i = 0; i < 100000; i++) {
        enum nos_shell_event event = feed(&guarded.editor, (unsigned char)next_random());
        assert(event >= NOS_SHELL_EVENT_NONE && event <= NOS_SHELL_EVENT_TOO_LONG);
        if (event == NOS_SHELL_EVENT_LINE) {
            char *words[NOS_SHELL_WORDS + 1];
            (void)nos_shell_parse(guarded.editor.line, words, NOS_SHELL_WORDS);
        }
        assert(guarded.before == UINT64_C(0xfeedface12345678));
        assert(guarded.after == guarded.before);
    }
    puts("PASS shell_editor_bounded_fuzz_100000");
}

int main(void)
{
    parser_cases();
    parser_fuzz();
    parser_roundtrip();
    editor_cases();
    editor_fuzz();
    return 0;
}
