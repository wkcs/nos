/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <nos/keyboard_definitions.h>

/* The action engine publishes this binding's press/release to lighting layers. */
static int execute(const struct key_action_context *context, const void *arguments, uint16_t length,
                   const struct kb_action_runtime *runtime)
{
    (void)context;
    (void)arguments;
    (void)length;
    (void)runtime;
    return KEY_ACTION_PASS;
}

/* 上报当前 Win Lock 开关状态 */
static bool query_state(const struct nlk_keyboard_ctx *keyboard, struct kb_action_state *out)
{
    if (keyboard == NULL || out == NULL) {
        return false;
    }
    out->active = keyboard->params.win_lock != 0U;
    out->value = keyboard->params.win_lock;
    return true;
}

const struct kb_action_definition kb_action_win_lock = {
    .id = "win_lock",
    .description = "Windows-key indicator trigger",
    .execute = execute,
    .query_state = query_state,
};
