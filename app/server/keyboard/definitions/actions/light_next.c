/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <nos/keyboard_definitions.h>

static int execute(const struct key_action_context *context, const void *arguments, uint16_t length,
                   const struct kb_action_runtime *runtime)
{
    (void)arguments;
    (void)length;
    if (context->event_type == 1U) {
        (void)runtime->request(runtime->context, KB_ACTION_REQUEST_LIGHT_NEXT, NULL);
    }
    return KEY_ACTION_DROP;
}

const struct kb_action_definition kb_action_light_next = {
    .id = "light_next",
    .description = "Next light config",
    .execute = execute,
};
