/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <nos/nlk_caps.h>
#include <nos/keyboard_engine.h>
#include <kernel/mm.h>
#include <kernel/irq.h>
#include <kernel/printk.h>
#include <uapi/nos/errno.h>

static unsigned int allocated_blocks;
static int allocations_until_failure = -1;
static struct nlk_kb_named_light_sync_beacon sent_beacon;
static uint16_t sent_length;
void *kmalloc(size_t n, gfp_t flags)
{
    void *p;
    (void)flags;
    if (allocations_until_failure == 0) {
        return NULL;
    }
    if (allocations_until_failure > 0) {
        allocations_until_failure--;
    }
    p = malloc(n);
    if (p != NULL) {
        allocated_blocks++;
    }
    return p;
}
void *kzalloc(size_t n, gfp_t flags)
{
    void *p = kmalloc(n, flags);
    if (p != NULL) {
        memset(p, 0, n);
    }
    return p;
}
void kfree(void *p)
{
    if (p != NULL) {
        allocated_blocks--;
    }
    free(p);
}
addr_t disable_irq_save(void)
{
    return 0;
}
void enable_irq_save(addr_t flags)
{
    (void)flags;
}
int pr_log(bool tag, enum log_level level, const char *fmt, ...)
{
    (void)tag;
    (void)level;
    (void)fmt;
    return 0;
}
int nlk_node_register_cap(struct nlk_node *node, struct nlk_capability *cap)
{
    (void)node;
    (void)cap;
    return 0;
}
int nlk_node_send_event(struct nlk_node *node, nlk_addr_t dst, uint8_t cap, uint8_t op,
                        const void *data, uint16_t len, bool ack)
{
    (void)node;
    (void)dst;
    (void)cap;
    (void)ack;
    assert(op == NLK_KB_EVT_LIGHT_SYNC_BEACON);
    assert(len <= sizeof(sent_beacon));
    sent_length = len;
    memcpy(&sent_beacon, data, len);
    return 0;
}

static void test_effect_entry_budget(void)
{
    /* Names belong to immutable definitions; firmware needs no copied labels. */
    assert(sizeof(struct nlk_kb_effect_entry) <= 5 * sizeof(void *));
}

static void render_effect(const struct nlk_kb_light_cfg *cfg, uint8_t x, uint8_t y, uint32_t phase,
                          uint8_t rgb[3], void *priv)
{
    (void)cfg;
    (void)x;
    (void)y;
    (void)phase;
    rgb[0] = *(uint8_t *)priv;
}

static void test_effect_registry(struct nlk_keyboard_ctx *keyboard)
{
    static const char first_name[] = "probe";
    static const char second_name[] = "second";
    uint8_t value = 42, rgb[3] = {0};
    unsigned int initial_blocks = allocated_blocks;
    const struct nlk_kb_effect_entry *first, *second;
    struct nlk_kb_named_light_sync_beacon incoming = {0};
    struct nlk_pkt_hdr header = {.cmd_op = NLK_KB_EVT_LIGHT_SYNC_BEACON};

    assert(nlk_cap_keyboard_find_light_effect(keyboard, 0) == NULL);
    assert(nlk_cap_keyboard_register_light_effect(keyboard, first_name, "Probe", render_effect,
                                                  &value) == 0);
    assert(nlk_cap_keyboard_register_light_effect(keyboard, second_name, "Second", NULL, NULL) ==
           0);
    first = nlk_cap_keyboard_find_light_effect(keyboard, 0);
    second = nlk_cap_keyboard_find_light_effect(keyboard, 1);
    assert(first != NULL && second != NULL);
    assert(nlk_kb_effect_name(first) == first_name);
    assert(nlk_kb_effect_name(second) == second_name);
    assert(nlk_kb_effect_id(first) == 0 && nlk_kb_effect_id(second) == 1);
    assert(nlk_cap_keyboard_find_light_effect(keyboard, 2) == NULL);
    assert(nlk_cap_keyboard_register_light_effect(keyboard, second_name, "Updated", render_effect,
                                                  &value) == 0);
    assert(keyboard->light_effect_count == 2 && allocated_blocks == initial_blocks + 2);
    assert(second == nlk_cap_keyboard_find_light_effect(keyboard, 1));
    second->render(&keyboard->light_cfg, 0, 0, 0, rgb, second->priv);
    assert(rgb[0] == value);
    keyboard->light_cfg.effect_id = 0;
    assert(nlk_cap_keyboard_broadcast_light_beacon(keyboard, 123) == 0);
    assert(sent_length == sizeof(sent_beacon));
    assert(strcmp(sent_beacon.effect_name, first_name) == 0);
    assert(sent_beacon.beacon.master_frame_tick == 123);
    /* Resolve received names locally even when the peer's numeric IDs differ. */
    incoming.beacon.effect_id = 99;
    strcpy(incoming.effect_name, second_name);
    assert(keyboard->cap.ops->on_event(keyboard->node, &keyboard->cap, &header, &incoming,
                                       sizeof(incoming)) == 0);
    assert(keyboard->light_cfg.effect_id == 1);
    assert(nlk_cap_keyboard_register_light_effect(keyboard, "", NULL, NULL, NULL) == -EINVAL);
    assert(nlk_cap_keyboard_register_light_effect(keyboard, "0123456789012345", NULL, NULL, NULL) ==
           -EINVAL);
}

static int handle_key(const struct key_action_context *context, void *priv)
{
    unsigned int *calls = priv;
    (*calls)++;
    assert(context->action_slot == 0);
    return context->event_type ? 5 : KEY_ACTION_PASS;
}

static void test_unified_rpc_requires_service(void)
{
    struct nlk_keyboard_ctx keyboard;
    struct nlk_pkt_hdr header = {.cmd_op = NLK_KB_V2_INFO};
    uint16_t length = 0;

    assert(nlk_cap_keyboard_init_sized(NULL, &keyboard, 1, 1, 1, 1, NULL, NULL) == 0);
    /* No legacy RPC handler may appear when the host platform macro is defined. */
    assert(keyboard.cap.cap_type == 0x0100U && keyboard.cap.cap_ver == 0x20U);
    assert(keyboard.cap.ops->on_rpc(NULL, &keyboard.cap, &header, NULL, 0, NULL, &length) ==
           -EAGAIN);
    nlk_cap_keyboard_deinit(&keyboard);
}

static bool report_has_key(const struct nlk_keyboard_ctx *keyboard, uint8_t key)
{
    for (size_t i = 0; i < sizeof(keyboard->last_report.keys); i++) {
        if (keyboard->last_report.keys[i] == key) {
            return true;
        }
    }
    return false;
}

static void test_allocation_failures(void)
{
    struct nlk_keyboard_ctx keyboard;
    unsigned int before = allocated_blocks;

    for (int i = 0; i < 5; i++) {
        allocations_until_failure = i;
        assert(nlk_cap_keyboard_init_sized(NULL, &keyboard, 1, 2, 2, 2, NULL, NULL) == -ENOMEM);
        assert(allocated_blocks == before);
    }
    allocations_until_failure = -1;
    assert(nlk_cap_keyboard_init_sized(NULL, &keyboard, 1, 2, 2, 2, NULL, NULL) == 0);
    assert(keyboard.keymap[1] - keyboard.keymap[0] == 2);
    assert(keyboard.action_chains[1] - keyboard.action_chains[0] == 2);
    struct nlk_kb_desc oversized = keyboard.desc;
    oversized.key_count++;
    assert(nlk_cap_keyboard_configure(&keyboard, &oversized) == -EINVAL);
    nlk_cap_keyboard_deinit(&keyboard);
    assert(allocated_blocks == before);
}

struct report_probe {
    unsigned int hid_count;
    unsigned int nkro_count;
    struct nlk_kb_hid_report hid;
    struct nlk_kb_nkro_report nkro;
};

static void capture_hid_report(const struct nlk_kb_hid_report *report, void *priv)
{
    struct report_probe *probe = priv;

    probe->hid_count++;
    probe->hid = *report;
}

static void capture_nkro_report(const struct nlk_kb_nkro_report *report, void *priv)
{
    struct report_probe *probe = priv;

    probe->nkro_count++;
    probe->nkro = *report;
}

static void test_hid_reports(void)
{
    struct nlk_keyboard_ctx keyboard;
    struct report_probe probe = {0};
    const uint8_t base[] = {4, 5, 6, 7, 8, 9, 10, 0xe0, KC_MEDIA_VOL_UP, 0xe8};
    const uint8_t fn[] = {20, 21, 22, 23, 24, 25, 26,
                          KC_MEDIA_BRIGHTNESS_UP, KC_MEDIA_BRIGHTNESS_DOWN, 0xe8};
    uint32_t bitmap = 1U << 5;

    assert(nlk_cap_keyboard_init_sized(NULL, &keyboard, 1, 10, 10, 2, capture_hid_report, &probe) ==
           0);
    keyboard.nkro_cb = capture_nkro_report;
    assert(nlk_cap_keyboard_load_layout(&keyboard, 0, base, sizeof(base)) == 0);
    assert(nlk_cap_keyboard_load_layout(&keyboard, 1, fn, sizeof(fn)) == 0);
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(keyboard.last_report.keys[0] == 9);
    bitmap |= 1U;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(keyboard.last_report.keys[0] == 9 && keyboard.last_report.keys[1] == 4);
    bitmap = 0x1ffU;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(keyboard.last_report.report_id == 1 && keyboard.last_nkro.report_id == 2);
    assert(keyboard.last_report.modifiers == 1 && keyboard.last_nkro.modifiers == 1);
    assert(keyboard.consumer_report == 0x00E9U);
    for (uint8_t key = 4; key <= 10; key++) {
        assert((keyboard.last_nkro.bitmap[key >> 3] & (1U << (key & 7U))) != 0);
    }
    assert(!report_has_key(&keyboard, KC_MEDIA_VOL_UP));
    /* Fn preserves a held media key, then the next press uses the selected layer. */
    bitmap = (1U << 8) | (1U << 9);
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(keyboard.active_layer == 1 && keyboard.consumer_report == 0x00E9U);
    bitmap = (1U << 7) | (1U << 9);
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(keyboard.consumer_report == 0x006FU);
    bitmap = 1U << 9;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(keyboard.consumer_report == 0U);
    bitmap = (1U << 8) | (1U << 9);
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(keyboard.consumer_report == 0x0070U);
    bitmap = 1U << 9;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    bitmap |= 1U;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(report_has_key(&keyboard, 20));
    /* Replacing a configuration suppresses held keys until their release. */
    nlk_cap_keyboard_release_held_keys(&keyboard);
    assert(!report_has_key(&keyboard, 20));
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(keyboard.last_report.keys[0] == 0);
    bitmap = 0;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    bitmap = 1;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 10) == 0);
    assert(keyboard.active_layer == 0 && report_has_key(&keyboard, 4));
    assert(probe.hid_count == probe.nkro_count && probe.hid_count >= 10);
    assert(memcmp(&probe.hid, &keyboard.last_report, sizeof(probe.hid)) == 0);
    assert(memcmp(&probe.nkro, &keyboard.last_nkro, sizeof(probe.nkro)) == 0);
    nlk_cap_keyboard_deinit(&keyboard);
}

struct action_probe {
    unsigned int presses;
    unsigned int releases;
    int result;
};

static int probe_action(const struct key_action_context *context, void *priv)
{
    struct action_probe *probe = priv;

    if (context->event_type) {
        probe->presses++;
        return probe->result;
    }
    probe->releases++;
    return KEY_ACTION_PASS;
}

static void test_action_chains(void)
{
    struct nlk_keyboard_ctx keyboard;
    struct action_probe remap = {.result = 5}, pass = {.result = KEY_ACTION_PASS};
    struct action_probe drop = {.result = KEY_ACTION_DROP};
    uint32_t bitmap = 1;

    assert(nlk_cap_keyboard_init_sized(NULL, &keyboard, 1, 1, 1, 1, NULL, NULL) == 0);
    keyboard.keymap[0][0] = 4;
    assert(nlk_cap_keyboard_register_action(&keyboard, "remap", "", probe_action, &remap) == 0);
    assert(nlk_cap_keyboard_register_action(&keyboard, "pass", "", probe_action, &pass) == 0);
    assert(nlk_cap_keyboard_register_action(&keyboard, "drop", "", probe_action, &drop) == 0);
    assert(nlk_cap_keyboard_bind_key_action(&keyboard, 0, 0, "remap", false) == 0);
    assert(nlk_cap_keyboard_bind_key_action(&keyboard, 0, 0, "pass", true) == 0);
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 1) == 0);
    assert(report_has_key(&keyboard, 5));
    assert(remap.presses == 1 && pass.presses == 1);
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 1) == 0);
    assert(remap.presses == 1);
    bitmap = 0;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 1) == 0);
    assert(remap.releases == 1 && pass.releases == 1);
    assert(nlk_cap_keyboard_bind_key_action(&keyboard, 0, 0, "remap", false) == 0);
    assert(nlk_cap_keyboard_bind_key_action(&keyboard, 0, 0, "drop", true) == 0);
    bitmap = 1;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 1) == 0);
    assert(keyboard.last_report.keys[0] == 0 && remap.presses == 2);
    bitmap = 0;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 1) == 0);
    assert(drop.releases == 1);
    assert(nlk_cap_keyboard_register_action(&keyboard, "chord", "", nlk_cap_keyboard_chord_action,
                                            NULL) == 0);
    assert(nlk_cap_keyboard_bind_key_action(&keyboard, 0, 0, "chord", false) == 0);
    keyboard.keymap[0][0] = 8U << 8 | 4U;
    bitmap = 1;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 1) == 0);
    assert(report_has_key(&keyboard, 4) && keyboard.last_report.modifiers == 8);
    bitmap = 0;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 1) == 0);
    assert(keyboard.last_report.keys[0] == 0 && keyboard.last_report.modifiers == 0);
    nlk_cap_keyboard_deinit(&keyboard);
}

static void test_macro_playback(void)
{
    struct nlk_keyboard_ctx keyboard;
    const struct nlk_kb_macro_step chord[] = {
        {NLK_KB_MACRO_ACT_KEY_DOWN, 0xe0, 10},
        {NLK_KB_MACRO_ACT_KEY_DOWN, 4, 5},
        {NLK_KB_MACRO_ACT_KEY_UP, 4, 7},
        {NLK_KB_MACRO_ACT_KEY_UP, 0xe0, 0},
    };
    struct nlk_kb_macro_step tap = {NLK_KB_MACRO_ACT_KEY_TAP, 4, 0};
    uint32_t bitmap = 1;

    assert(nlk_cap_keyboard_init_sized(NULL, &keyboard, 1, 1, 1, 1, NULL, NULL) == 0);
    keyboard.keymap[0][0] = 5;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 1) == 0);
    assert(nlk_cap_keyboard_set_macro(&keyboard, 2, "Chord", chord, 4, 1) == 0);
    assert(nlk_cap_keyboard_macro_start(&keyboard, 2, 100) == 0);
    assert(nlk_cap_keyboard_macro_start(&keyboard, 2, 100) == -EBUSY);
    assert(nlk_cap_keyboard_tick(&keyboard, 109) == 0);
    assert(keyboard.last_report.modifiers == 0);
    assert(nlk_cap_keyboard_tick(&keyboard, 110) == 0);
    assert(keyboard.last_report.modifiers == 1 && report_has_key(&keyboard, 5));
    assert(nlk_cap_keyboard_tick(&keyboard, 115) == 0);
    assert(report_has_key(&keyboard, 4) && report_has_key(&keyboard, 5));
    assert(nlk_cap_keyboard_tick(&keyboard, 122) == 0);
    assert(!report_has_key(&keyboard, 4) && report_has_key(&keyboard, 5));
    assert(nlk_cap_keyboard_tick(&keyboard, 123) == 0);
    assert(keyboard.macro_playing == 0 && keyboard.last_report.modifiers == 0);
    assert(nlk_cap_keyboard_macro_start(&keyboard, 2, 200) == 0);
    assert(nlk_cap_keyboard_tick(&keyboard, 210) == 0);
    assert(nlk_cap_keyboard_macro_cancel(&keyboard) == 0);
    assert(keyboard.last_report.modifiers == 0 && report_has_key(&keyboard, 5));
    assert(nlk_cap_keyboard_set_macro(&keyboard, 1, "Repeat", &tap, 1, 2) == 0);
    assert(nlk_cap_keyboard_macro_start(&keyboard, 1, UINT32_MAX - 1) == 0);
    assert(nlk_cap_keyboard_tick(&keyboard, UINT32_MAX - 1) == 0);
    assert(report_has_key(&keyboard, 4));
    assert(nlk_cap_keyboard_tick(&keyboard, UINT32_MAX) == 0);
    assert(!report_has_key(&keyboard, 4));
    assert(nlk_cap_keyboard_tick(&keyboard, 0) == 0);
    assert(report_has_key(&keyboard, 4));
    assert(nlk_cap_keyboard_tick(&keyboard, 1) == 0);
    assert(!keyboard.macro_playing && !report_has_key(&keyboard, 4));
    tap.key_code = 0xe8;
    assert(nlk_cap_keyboard_set_macro(&keyboard, 1, "Invalid", &tap, 1, 1) == -EINVAL);
    assert(nlk_cap_keyboard_find_macro(&keyboard, 1)->steps[0].key_code == 4);
    assert(nlk_cap_keyboard_macro_start(&keyboard, 1, 300) == 0);
    assert(nlk_cap_keyboard_tick(&keyboard, 300) == 0);
    nlk_cap_keyboard_replace_macros(&keyboard, NULL, 0);
    assert(!keyboard.macro_playing && !report_has_key(&keyboard, 4));
    assert(report_has_key(&keyboard, 5) && keyboard.macro_count == 0);
    nlk_cap_keyboard_deinit(&keyboard);
}

static void capture_light_change(const struct nlk_kb_light_cfg *cfg,
                                 const struct nlk_kb_light_sync_beacon *beacon, uint8_t indicators,
                                 void *priv)
{
    unsigned int *count = priv;

    assert(cfg != NULL);
    (void)beacon;
    (void)indicators;
    (*count)++;
}

static bool mock_action_state(const struct nlk_keyboard_ctx *ctx, uint32_t trigger,
                              struct kb_action_state *out, void *priv)
{
    (void)priv;
    if (trigger == nlk_fnv1a_24("caps_state")) {
        out->active = (ctx->indicator_leds & NLK_KB_LED_CAPS) != 0U;
        out->value = ctx->indicator_leds;
        return true;
    }
    return false;
}

static void test_light_activation(void)
{
    struct nlk_keyboard_ctx keyboard;
    struct action_probe pass = {.result = KEY_ACTION_PASS};
    struct nlk_kb_light_layer layers[4] = {
        {.zone_id = NLK_KB_LIGHT_LAYER_OWN_CFG,
         .width = 1,
         .height = 1,
         .activation = NLK_KB_LIGHT_HOLD,
         .cfg = {.enable = 1}},
        {.zone_id = NLK_KB_LIGHT_LAYER_OWN_CFG,
         .width = 1,
         .height = 1,
         .activation = NLK_KB_LIGHT_TOGGLE,
         .cfg = {.enable = 1}},
        {.zone_id = NLK_KB_LIGHT_LAYER_OWN_CFG,
         .width = 1,
         .height = 1,
         .activation = NLK_KB_LIGHT_STATE,
         .cfg = {.enable = 1}},
        {.zone_id = NLK_KB_LIGHT_LAYER_OWN_CFG,
         .width = 1,
         .height = 1,
         .activation = NLK_KB_LIGHT_PULSE,
         .cfg = {.enable = 1}},
    };
    struct nlk_kb_light_zone_desc zone = {0};
    uint32_t bitmap = 1;
    uint32_t generation;
    unsigned int changes = 0;
    unsigned int previous_changes;

    assert(nlk_cap_keyboard_init_sized(NULL, &keyboard, 1, 1, 1, 1, NULL, NULL) == 0);
    assert(nlk_cap_keyboard_register_light_effect(&keyboard, "static", "", render_effect, NULL) ==
           0);
    assert(nlk_cap_keyboard_register_action(&keyboard, "pulse", "", probe_action, &pass) == 0);
    assert(nlk_cap_keyboard_bind_key_action(&keyboard, 0, 0, "pulse", false) == 0);
    assert(nlk_cap_keyboard_set_light_cb(&keyboard, capture_light_change, NULL, &changes) == 0);
    nlk_cap_keyboard_set_action_state_cb(&keyboard, mock_action_state, NULL);
    layers[0].trigger = nlk_fnv1a_24("pulse");
    layers[1].trigger = layers[0].trigger;
    layers[2].trigger = nlk_fnv1a_24("caps_state");
    layers[3].trigger = layers[0].trigger;
    assert(nlk_cap_keyboard_set_light_layers(&keyboard, layers, 4) == 0);
    assert(!keyboard.light_layer_active[0] && !keyboard.light_layer_active[1] &&
           !keyboard.light_layer_active[2] && !keyboard.light_layer_active[3]);
    nlk_cap_keyboard_set_indicators(&keyboard, NLK_KB_LED_CAPS);
    assert(keyboard.light_layer_active[2] && keyboard.light_runtime->triggers[2].value == NLK_KB_LED_CAPS);
    nlk_cap_keyboard_set_indicators(&keyboard, 0);
    assert(!keyboard.light_layer_active[2]);
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 1) == 0);
    assert(keyboard.light_layer_active[0] && keyboard.light_layer_active[1] &&
           keyboard.light_layer_active[3]);
    uint32_t seq1 = keyboard.light_runtime->triggers[3].seq;
    assert(seq1 != 0U);
    bitmap = 0;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 1) == 0);
    assert(!keyboard.light_layer_active[0] && keyboard.light_layer_active[1] &&
           keyboard.light_layer_active[3]);
    bitmap = 1;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &bitmap, 1) == 0);
    assert(keyboard.light_layer_active[0] && !keyboard.light_layer_active[1] &&
           keyboard.light_runtime->triggers[3].seq > seq1);
    nlk_cap_keyboard_release_held_keys(&keyboard);
    assert(!keyboard.light_layer_active[0]);
    assert(changes >= 5);
    previous_changes = changes;
    generation = keyboard.light_layer_generation;
    layers[0].width = 2;
    assert(nlk_cap_keyboard_set_light_layers(&keyboard, layers, 4) == -EINVAL);
    assert(keyboard.light_layer_generation == generation && keyboard.light_layers[0].width == 1);
    assert(changes == previous_changes);
    layers[0].width = 1;
    assert(nlk_cap_keyboard_register_light_zone(&keyboard, &zone, NULL) == 0);
    layers[0].zone_id = 0;
    assert(nlk_cap_keyboard_validate_light_layer(&keyboard, &layers[0]) == -EINVAL);
    assert(nlk_cap_keyboard_set_light_layers(&keyboard, NULL, 0) == 0);
    assert(keyboard.light_cfg.enable == 1 && keyboard.light_layer_count == 0);
    nlk_cap_keyboard_deinit(&keyboard);
}

int main(void)
{
    struct nlk_keyboard_ctx keyboard;
    struct nlk_node node = {0};
    static const char action_name[] = "runtime_probe";
    uint32_t keys = 1;
    unsigned int calls = 0;

    test_unified_rpc_requires_service();
    test_allocation_failures();
    test_hid_reports();
    test_action_chains();
    test_macro_playback();
    test_light_activation();
    test_effect_entry_budget();

    /* A production capability must leave room for the configuration documents. */
    assert(sizeof(keyboard) <= 2000);
    assert(nlk_cap_keyboard_init_sized(&node, &keyboard, 1, 1, 1, 1, NULL, NULL) == 0);
    assert(keyboard.cap.cap_type == 0x0100U);
    assert(keyboard.cap.cap_ver == 0x20U);
    assert(nlk_cap_keyboard_register_action(&keyboard, action_name, "Probe", handle_key, &calls) ==
           0);
    assert(keyboard.action_names[0] == action_name);
    assert(nlk_cap_keyboard_action_id(NULL, 0) == 0);
    assert(nlk_cap_keyboard_action_id(&keyboard, 0) == 1);
    assert(nlk_cap_keyboard_action_id(&keyboard, 1) == 0);
    assert(nlk_cap_keyboard_action_id(&keyboard, NLK_KB_MAX_ACTIONS) == 0);
    /* Metadata refresh preserves the handler and its callback context. */
    assert(nlk_cap_keyboard_register_action(&keyboard, action_name, "Updated", NULL, NULL) == 0);
    assert(keyboard.action_count == 1);
    assert(nlk_cap_keyboard_bind_key_action(&keyboard, 0, 0, action_name, false) == 0);
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &keys, 1) == 0);
    assert(calls == 1 && keyboard.last_report.keys[0] == 5);
    keys = 0;
    assert(nlk_cap_keyboard_update_local_keys(&keyboard, &keys, 1) == 0);
    assert(calls == 2 && keyboard.last_report.keys[0] == 0);
    /* IDs stay dense through duplicate registration and the capacity boundary. */
    static const char *const names[] = {
        "a01", "a02", "a03", "a04", "a05", "a06", "a07", "a08", "a09", "a10", "a11", "a12",
        "a13", "a14", "a15", "a16", "a17", "a18", "a19", "a20", "a21", "a22", "a23",
    };
    for (uint8_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        assert(nlk_cap_keyboard_register_action(&keyboard, names[i], NULL, NULL, NULL) == 0);
        assert(nlk_cap_keyboard_action_id(&keyboard, i + 1) == i + 2);
    }
    assert(keyboard.action_count == NLK_KB_MAX_ACTIONS);
    assert(nlk_cap_keyboard_register_action(&keyboard, "overflow", NULL, NULL, NULL) == -ENOSPC);
    assert(nlk_cap_keyboard_register_action(&keyboard, "", NULL, NULL, NULL) == -EINVAL);
    assert(nlk_cap_keyboard_register_action(&keyboard, "01234567890123456789012345678901", NULL,
                                            NULL, NULL) == -EINVAL);
    assert(nlk_cap_keyboard_bind_key_action(&keyboard, 0, 0, "a23", false) == 0);
    assert(keyboard.action_chains[0][0].action_ids[0] == NLK_KB_MAX_ACTIONS);
    test_effect_registry(&keyboard);
    nlk_cap_keyboard_deinit(&keyboard);
    assert(allocated_blocks == 0);
    printf("production keyboard capability tests passed; context=%zu bytes\n", sizeof(keyboard));
    return 0;
}
