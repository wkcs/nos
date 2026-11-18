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

const struct kb_action_definition kb_action_key_light = {
    .id = "key_light",
    .description = "Per-key lighting trigger",
    .execute = execute,
};
