/* Copyright (C) 2026 NOS contributors */
#include "shell.h"

static unsigned text_length(const char *text)
{
    unsigned length = 0;
    while (text[length]) length++;
    return length;
}

static void text_copy(char *destination, const char *source)
{
    do { *destination++ = *source; } while (*source++);
}

static int text_equal(const char *left, const char *right)
{
    while (*left && *left == *right) { left++; right++; }
    return *left == *right;
}

static void redraw(struct nos_shell_editor *editor,
                   void (*emit)(void *, const char *, size_t), void *context)
{
    emit(context, "\r" NOS_SHELL_PROMPT, sizeof(NOS_SHELL_PROMPT));
    emit(context, editor->line, editor->length);
    emit(context, "\033[K", 3);
    for (unsigned index = editor->cursor; index < editor->length; index++)
        emit(context, "\033[D", 3);
}

static void erase_at_cursor(struct nos_shell_editor *editor)
{
    if (editor->cursor == editor->length) return;
    for (unsigned index = editor->cursor; index < editor->length; index++)
        editor->line[index] = editor->line[index + 1];
    editor->length--;
}

static void remember(struct nos_shell_editor *editor)
{
    if (!editor->length || editor->overflow) return;
    unsigned last = (editor->history_next + NOS_SHELL_HISTORY - 1) % NOS_SHELL_HISTORY;
    if (editor->history_count && text_equal(editor->line, editor->history[last])) return;
    text_copy(editor->history[editor->history_next], editor->line);
    editor->history_next = (editor->history_next + 1) % NOS_SHELL_HISTORY;
    if (editor->history_count < NOS_SHELL_HISTORY) editor->history_count++;
}

static void recall(struct nos_shell_editor *editor, unsigned older)
{
    if (older) {
        if (editor->history_offset == editor->history_count) return;
        if (!editor->history_offset) text_copy(editor->draft, editor->line);
        editor->history_offset++;
    } else {
        if (!editor->history_offset) return;
        editor->history_offset--;
    }
    const char *text = editor->draft;
    if (editor->history_offset) {
        unsigned index = (editor->history_next + NOS_SHELL_HISTORY - editor->history_offset) % NOS_SHELL_HISTORY;
        text = editor->history[index];
    }
    text_copy(editor->line, text);
    editor->cursor = editor->length = text_length(text);
}

void nos_shell_editor_init(struct nos_shell_editor *editor)
{
    unsigned char *bytes = (unsigned char *)editor;
    for (size_t index = 0; index < sizeof(*editor); index++) bytes[index] = 0;
}

enum nos_shell_event nos_shell_editor_feed(struct nos_shell_editor *editor,
    unsigned char character, void (*emit)(void *, const char *, size_t), void *context)
{
    if (editor->submitted) {
        editor->line[0] = 0;
        editor->length = editor->cursor = editor->overflow = editor->history_offset = 0;
        editor->submitted = 0;
    }
    if (character == '\n' && editor->saw_cr) {
        editor->saw_cr = 0;
        return NOS_SHELL_EVENT_NONE;
    }
    editor->saw_cr = 0;
    if (character == 3) {
        editor->escape_state = 0;
        editor->submitted = 1;
        emit(context, "^C\n", 3);
        return NOS_SHELL_EVENT_CANCEL;
    }
    if (character == '\r' || character == '\n') {
        editor->escape_state = 0;
        editor->saw_cr = character == '\r';
        remember(editor);
        editor->submitted = 1;
        emit(context, "\n", 1);
        return editor->overflow ? NOS_SHELL_EVENT_TOO_LONG : NOS_SHELL_EVENT_LINE;
    }
    if (character == 27) {
        editor->escape_state = 1;
        editor->escape_parameter = 0;
        return NOS_SHELL_EVENT_NONE;
    }
    if (editor->escape_state) {
        if (editor->escape_state == 1) {
            editor->escape_state = character == '[' || character == 'O' ? 2 : 0;
            return NOS_SHELL_EVENT_NONE;
        }
        if (character >= '0' && character <= '9') {
            if (editor->escape_parameter < 100)
                editor->escape_parameter = editor->escape_parameter * 10 + character - '0';
            return NOS_SHELL_EVENT_NONE;
        }
        /* Discard unsupported CSI parameters until their final byte. */
        if (character < 0x40 || character > 0x7e) return NOS_SHELL_EVENT_NONE;
        editor->escape_state = 0;
        switch (character) {
        case 'A': recall(editor, 1); break;
        case 'B': recall(editor, 0); break;
        case 'C': if (editor->cursor < editor->length) editor->cursor++; break;
        case 'D': if (editor->cursor) editor->cursor--; break;
        case 'H': editor->cursor = 0; break;
        case 'F': editor->cursor = editor->length; break;
        case '~':
            if (editor->escape_parameter == 1 || editor->escape_parameter == 7) editor->cursor = 0;
            else if (editor->escape_parameter == 4 || editor->escape_parameter == 8) editor->cursor = editor->length;
            else if (editor->escape_parameter == 3) erase_at_cursor(editor);
            break;
        default: return NOS_SHELL_EVENT_NONE;
        }
        redraw(editor, emit, context);
        return NOS_SHELL_EVENT_NONE;
    }
    /* A pasted tab separates words even without tab completion. */
    if (character == '\t') character = ' ';
    if (character == 1) editor->cursor = 0;
    else if (character == 5) editor->cursor = editor->length;
    else if (character == 21) {
        editor->line[0] = 0;
        editor->length = editor->cursor = editor->overflow = 0;
    } else if (character == 11) {
        editor->line[editor->cursor] = 0;
        editor->length = editor->cursor;
    } else if (character == 4) erase_at_cursor(editor);
    else if (character == 8 || character == 127) {
        if (!editor->cursor) return NOS_SHELL_EVENT_NONE;
        editor->cursor--;
        erase_at_cursor(editor);
    } else if (character >= 32 && character < 127) {
        if (editor->length + 1 == NOS_SHELL_LINE_BYTES) {
            editor->overflow = 1;
            return NOS_SHELL_EVENT_NONE;
        }
        for (unsigned index = editor->length + 1; index > editor->cursor; index--)
            editor->line[index] = editor->line[index - 1];
        editor->line[editor->cursor++] = (char)character;
        editor->length++;
        if (editor->cursor == editor->length) {
            char byte = (char)character;
            emit(context, &byte, 1);
            return NOS_SHELL_EVENT_NONE;
        }
    } else return NOS_SHELL_EVENT_NONE;
    redraw(editor, emit, context);
    return NOS_SHELL_EVENT_NONE;
}
