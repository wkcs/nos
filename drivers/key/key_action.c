#include <wk/task.h>
#include <wk/err.h>
#include <wk/delay.h>
#include <init/init.h>
#include <string.h>
#include <cmd/cmd.h>
#include "keyboard.h"

struct key_action_info {
    uint8_t type;
    void (*action)(void);
};

void key_action_led(void)
{
    static bool led_is_on = true;
    uint8_t argc = 1;
    char *argv[1];

    led_is_on = !led_is_on;
    argv[0] = led_is_on ? "on" : "off";

    //pr_info("led %s\r\n", argv[0]);
    cmd_run("led", argc, argv);
}

void key_action_caps(void)
{
}

void key_action_macro(void)
{
}

static struct key_action_info key_action[] = {
    { KA_LED, key_action_led },
    { KA_CAPS, key_action_caps },
    { KA_MACRO, key_action_macro},
    {},
};

int key_action_run(enum key_action action)
{
    struct key_action_info *info = key_action;

    for (; info->type != KA_NO && info->action != NULL; info++) {
        if (info->type == action)
            info->action();
    }

    return 0;
}
