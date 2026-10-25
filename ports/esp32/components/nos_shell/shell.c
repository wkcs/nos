/* Copyright (C) 2026 NOS contributors. Shared nshell editor/parser, MCU commands. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include "nos/esp_shell.h"
#include "nos/shell.h"
#include "nos/arch.h"
#include "nos/user.h"
#include "nos/driver/esp_idf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static struct nos_shell_editor editor;
static int output_error;
static void print(const char *format, ...) __attribute__((format(printf, 1, 2)));

static int write_all(const char *text, size_t n) {
    unsigned stalls = 0;
    while (n) {
        intptr_t rc = nos_write_nowait(1, text, n);
        if (rc == -NOS_EAGAIN && stalls++ < 100) {
            vTaskDelay(1);
            continue;
        }
        if (rc <= 0 || (size_t)rc > n)
            return rc < 0 ? (int)rc : -NOS_EIO;
        text += rc;
        n -= (size_t)rc;
    }
    return 0;
}
static void emit(void *context, const char *text, size_t n) {
    (void)context;
    while (n && !output_error) {
        const char *lf = memchr(text, '\n', n);
        size_t count = lf ? (size_t)(lf - text) : n;
        if (count)
            output_error = write_all(text, count);
        if (lf && !output_error)
            output_error = write_all("\r\n", 2);
        size_t consumed = count + !!lf;
        text += consumed;
        n -= consumed;
    }
}
static void print(const char *format, ...) {
    char buffer[NOS_SHELL_LINE_BYTES + 128];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof(buffer)) {
        emit(NULL, "nshell: output too long\n", 24);
        return;
    }
    emit(NULL, buffer, n);
}
static void version(void) {
    print("NOS arch=%s abi=%u target=%s scheduler=NOS\n", ARCH_NAME, NOS_ABI_VERSION,
          CONFIG_IDF_TARGET);
}
static void stats(void) {
    struct nos_object_stats s;
    intptr_t rc = nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&s, 0, 0);
    if (rc < 0)
        print("stats: error=%ld\n", (long)rc);
    else
        print("tasks=%" PRIu32 " mutexes=%" PRIu32 " endpoints=%" PRIu32 " free_pages=%" PRIu32
              "\n",
              s.tasks, s.mutexes, s.endpoints, s.free_pages);
    print("sdk_heap_free=%u sdk_heap_min=%u\n",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
          (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}
static void devices(void) {
    struct nos_service_info *items = calloc(32, sizeof(*items));
    if (!items) {
        print("devices: no memory\n");
        return;
    }
    size_t count = 0;
    int rc = nos_idf_devices_snapshot(items, 32, &count);
    if (rc)
        print("devices: error=%d\n", rc);
    else {
        print("devices=%u\n", (unsigned)count);
        for (size_t i = 0; i < count; i++)
            print("%s kind=%u state=%s owned=%u\n", items[i].name, (unsigned)items[i].kind,
                  items[i].state == NOS_SERVICE_RUNNING   ? "running"
                  : items[i].state == NOS_SERVICE_FAULTED ? "faulted"
                                                          : "stopped",
                  (unsigned)items[i].resources_owned);
    }
    free(items);
}
static void processes(void) {
    TaskStatus_t *items = calloc(NOS_TASKS, sizeof(*items));
    char(*names)[configMAX_TASK_NAME_LEN] = calloc(NOS_TASKS, sizeof(*names));
    if (!items || !names) {
        free(items);
        free(names);
        print("ps: no memory\n");
        return;
    }
    vTaskSuspendAll();
    unsigned count = uxTaskGetSystemState(items, NOS_TASKS, NULL);
    for (unsigned i = 0; i < count; i++)
        snprintf(names[i], sizeof(names[i]), "%s", items[i].pcTaskName);
    xTaskResumeAll();
    print("tasks=%u capacity=%u\n", count, NOS_TASKS);
    for (unsigned i = 0; i < count; i++) {
        print("id=%u name=%s priority=%u state=%u ", (unsigned)items[i].xTaskNumber, names[i],
              (unsigned)items[i].uxCurrentPriority, (unsigned)items[i].eCurrentState);
        if (items[i].xTaskNumber == NOS_IDLE_FIRST)
            print("stack_free=unavailable\n"); /* Native idle has no SDK stack-fill marker. */
        else
            print("stack_free=%u\n", (unsigned)items[i].usStackHighWaterMark);
    }
    free(names);
    free(items);
}
static void command(char *line) {
    char *words[NOS_SHELL_WORDS + 1];
    int count = nos_shell_parse(line, words, NOS_SHELL_WORDS);
    if (count < 0) {
        print("parse: %s\n", count == NOS_SHELL_PARSE_QUOTE    ? "unmatched quote"
                             : count == NOS_SHELL_PARSE_ESCAPE ? "trailing escape"
                                                               : "too many arguments");
        return;
    }
    if (!count)
        return;
    const char *name = words[0];
    if (nos_esp_shell_files(count, words, emit, NULL))
        return;
    if (!strcmp(name, "run") || !strcmp(name, "mount")) {
        print("%s: unsupported; ELF/persistent block backend is not bound\n", name);
        return;
    }
    if (!strcmp(name, "echo")) {
        for (int i = 1; i < count; i++)
            print("%s%s", i == 1 ? "" : " ", words[i]);
        print("\n");
        return;
    }
    if (count != 1) {
        print("%s: unexpected arguments\n", name);
        return;
    }
    if (!strcmp(name, "help"))
        print("nshell commands: help version uptime stats mem ps devices echo history clear\n"
              "files: pwd cd ls cat write append touch mkdir rm rmdir mv stat\n");
    else if (!strcmp(name, "version"))
        version();
    else if (!strcmp(name, "uptime"))
        print("uptime_us=%" PRId64 "\n", esp_timer_get_time());
    else if (!strcmp(name, "stats") || !strcmp(name, "mem"))
        stats();
    else if (!strcmp(name, "ps"))
        processes();
    else if (!strcmp(name, "devices"))
        devices();
    else if (!strcmp(name, "history")) {
        for (unsigned offset = editor.history_count; offset; offset--) {
            unsigned i = (editor.history_next + NOS_SHELL_HISTORY - offset) % NOS_SHELL_HISTORY;
            print("%s\n", editor.history[i]);
        }
    } else if (!strcmp(name, "clear"))
        print("\033[2J\033[H");
    else
        print("nshell: unknown command: %s\n", name);
}
void nos_idf_shell_run(void) {
    nos_shell_editor_init(&editor);
    print("[nos] init: target=%s native_task=%ld\n", CONFIG_IDF_TARGET,
          (long)nos_syscall(NOS_SYS_GETPID, 0, 0, 0));
    print("[nos] scheduler: online, cpu=0, hz=%u\n", (unsigned)configTICK_RATE_HZ);
    version();
    stats();
    devices();
    print("[nos] console: UART0 managed, 115200 baud\n");
    print("[nos] nshell: starting (ESP shared-firmware profile)\n");
    print("NOS SHELL READY\n" NOS_SHELL_PROMPT);
    for (;;) {
        unsigned char ch;
        intptr_t rc = nos_read_nowait(0, &ch, 1);
        if (rc != 1 || output_error) {
            output_error = 0;
            vTaskDelay(1);
            continue;
        }
        enum nos_shell_event event = nos_shell_editor_feed(&editor, ch, emit, NULL);
        if (event == NOS_SHELL_EVENT_LINE)
            command(editor.line);
        else if (event == NOS_SHELL_EVENT_TOO_LONG)
            print("command too long\n");
        if (event != NOS_SHELL_EVENT_NONE)
            print(NOS_SHELL_PROMPT);
    }
}
