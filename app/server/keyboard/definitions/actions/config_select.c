/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <nos/keyboard_definitions.h>
#include <nos/keyboard_document.h>

static const struct kb_parameter_definition parameters[] = {
    {.id = 1, .type = KB_PARAMETER_BASE_REF, .maximum = 23, .name = "target"},
};

static int execute(const struct key_action_context *context, const void *arguments, uint16_t length,
                   const struct kb_action_runtime *runtime)
{
    if (context->event_type == 1U) {
        struct kb_doc_reader reader;
        char target[24];

        kb_doc_reader_init(&reader, arguments, length);
        kb_doc_string(&reader, target, sizeof(target));
        if (!reader.error && !reader.left) {
            (void)runtime->request(runtime->context, KB_ACTION_REQUEST_SELECT_BASE, target);
        }
    }
    return KEY_ACTION_DROP;
}

const struct kb_action_definition kb_action_config_select = {
    .id = "config_select",
    .description = "Select base config",
    .parameters = parameters,
    .parameter_count = 1,
    .execute = execute,
};
