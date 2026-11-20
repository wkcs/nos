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

/* 同步主机上报的真实 Caps Lock LED 状态，避免本地翻转与系统状态反相 */
static bool query_state(const struct nlk_keyboard_ctx *keyboard, struct kb_action_state *out)
{
    if (keyboard == NULL || out == NULL) {
        return false;
    }
    out->active = (keyboard->indicator_leds & NLK_KB_LED_CAPS) != 0U;
    out->value = keyboard->indicator_leds;
    return true;
}

const struct kb_action_definition kb_action_caps_lock = {
    .id = "caps_lock",
    .description = "Caps Lock indicator trigger",
    .execute = execute,
    .query_state = query_state,
};
