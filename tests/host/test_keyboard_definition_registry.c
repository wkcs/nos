/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <string.h>
#include <nos/keyboard_definitions.h>

int main(void)
{
    size_t i;
    bool action_found = false, effect_found = false;
    struct key_action_context key = {0};
    uint8_t rgb[3];

    for (i = 0; i < kb_action_definition_count; i++) {
        const struct kb_action_definition *action = kb_action_definitions[i];
        if (!strcmp(action->id, "probe")) {
            action_found = true;
            assert(action->execute(&key, NULL, 0, NULL) == 42);
        }
    }
    for (i = 0; i < kb_effect_definition_count; i++) {
        const struct kb_effect_definition *effect = kb_effect_definitions[i];
        if (!strcmp(effect->id, "probe")) {
            effect_found = true;
            effect->render(NULL, 0, 0, 0, rgb, NULL);
            assert(rgb[0] == 42 && rgb[1] == 0 && rgb[2] == 0);
        }
    }
#ifdef TEST_DEFINITION_EXTENSION
    assert(action_found && effect_found);
    assert(kb_action_definition_count == 13 && kb_effect_definition_count == 10);
#else
    assert(!action_found && !effect_found);
    assert(kb_action_definition_count == 12 && kb_effect_definition_count == 9);
#endif
    return 0;
}
