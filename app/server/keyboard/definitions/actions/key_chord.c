/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <nos/keyboard_definitions.h>

static const struct kb_parameter_definition parameters[] = {
    {.id = 1,
     .type = KB_PARAMETER_U8,
     .minimum = 1,
     .maximum = 255,
     .default_value = 1,
     .name = "modifiers"},
};

static int execute(const struct key_action_context *context, const void *arguments, uint16_t length,
                   const struct kb_action_runtime *runtime)
{
    struct key_action_context chord = *context;

    (void)runtime;
    if (length != 1U) {
        return KEY_ACTION_DROP;
    }
    chord.initial_code =
        (context->initial_code & 255U) | ((uint32_t)*(const uint8_t *)arguments << 8);
    return nlk_cap_keyboard_chord_action(&chord, NULL);
}

const struct kb_action_definition kb_action_key_chord = {
    .id = "key_chord",
    .description = "Add key modifiers",
    .parameters = parameters,
    .parameter_count = 1,
    .execute = execute,
};
