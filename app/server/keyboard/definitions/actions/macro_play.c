/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <nos/keyboard_definitions.h>
#include <nos/keyboard_document.h>
#include <nos/keyboard_object_store.h>

static const struct kb_parameter_definition parameters[] = {
    {.id = 1, .type = KB_PARAMETER_MACRO_REF, .maximum = KB_STORE_NAME_MAX, .name = "target"},
};

static int execute(const struct key_action_context *context, const void *arguments, uint16_t length,
                   const struct kb_action_runtime *runtime)
{
    if (context->event_type == 1U) {
        struct kb_doc_reader reader;
        char target[KB_STORE_NAME_SIZE];

        kb_doc_reader_init(&reader, arguments, length);
        kb_doc_string(&reader, target, sizeof(target));
        if (!reader.error && !reader.left) {
            (void)runtime->request(runtime->context, KB_ACTION_REQUEST_MACRO_PLAY, target);
        }
    }
    return KEY_ACTION_DROP;
}

/* 上报宏播放中状态与当前步序号 */
static bool query_state(const struct nlk_keyboard_ctx *keyboard, struct kb_action_state *out)
{
    if (keyboard == NULL || out == NULL) {
        return false;
    }
    out->active = keyboard->macro_playing != 0U;
    out->value = keyboard->macro_play_index;
    return true;
}

const struct kb_action_definition kb_action_macro_play = {
    .id = "macro_play",
    .description = "Play keyboard macro",
    .parameters = parameters,
    .parameter_count = 1,
    .execute = execute,
    .query_state = query_state,
};
