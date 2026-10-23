/* Copyright (C) 2026 NOS contributors. Loopback physical-profile interoperability fixture. */
#define _POSIX_C_SOURCE 200809L
#include "nlp/capabilities.h"
#include "nlp/auth.h"
#include "terminal_posix.h"
#include <sys/stat.h>
extern const struct nlp_crypto nlp_openssl_crypto;
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
struct server {
    struct nlp_endpoint endpoint;
    struct nlp_stream stream;
    struct nlp_keyboard keyboard;
    struct nlp_log log;
    struct nlp_terminal terminal;
    uint8_t tx[4096];
    size_t used, sent;
};
static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static int transmit(void *context, const uint8_t *bytes, size_t size)
{
    struct server *s = context;
    size_t written;
    if (sizeof(s->tx) - s->used < size + 2)
        return -NLP_BUSY;
    int result = nlp_stream_wrap(NLP_FRAMING_TCP, bytes, size, s->tx + s->used,
                                 sizeof(s->tx) - s->used, &written);
    if (!result)
        s->used += written;
    return result;
}
static int read_key(const char *path, uint8_t key[32], int private_key)
{
    uint8_t bytes[33];
    struct stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int good = !fstat(fd, &st) && S_ISREG(st.st_mode) && (!private_key || !(st.st_mode & 077)) &&
               read(fd, bytes, 33) == 32;
    close(fd);
    if (good)
        memcpy(key, bytes, 32);
    nlp_secure_clear(bytes, sizeof(bytes));
    return good ? 0 : -1;
}
int main(int argc, char **argv)
{
    struct server s;
    int listener, fd, enable = 1;
    unsigned port;
    int secure = argc >= 5 && !strcmp(argv[1], "--secure-loopback");
    uint8_t key[32], pin[32];
    struct nlp_auth auth;
    int shell = secure && argc == 6 && !strcmp(argv[5], "--shell");
    struct nlp_posix_terminal terminal;
    struct nlp_terminal_ops terminal_ops;
    nlp_posix_terminal_init(&terminal, &terminal_ops);
    struct sockaddr_in address;
    if ((secure && argc != 5 && !shell) ||
        (!secure && (argc != 3 || strcmp(argv[1], "--physical-loopback"))) ||
        !(port = (unsigned)strtoul(argv[2], NULL, 10)) || port > 65535) {
        fprintf(stderr,
                "usage: %s --physical-loopback PORT\nThis fixture binds 127.0.0.1 only; it is not "
                "an authenticated network service.\n",
                argv[0]);
        return 2;
    }
    if (secure && (read_key(argv[3], key, 1) || read_key(argv[4], pin, 0))) {
        fprintf(stderr, "invalid raw key/pin file or private key permissions\n");
        return 2;
    }
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
        return 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 1)) {
        perror("listen");
        return 1;
    }
    printf("NLP %s fixture 127.0.0.1:%u\n", secure ? "Noise authenticated" : "physical-profile",
           port);
    fflush(stdout);
    while ((fd = accept(listener, NULL, NULL)) >= 0) {
        struct nlp_endpoint_config config;
        memset(&s, 0, sizeof(s));
        memset(&config, 0, sizeof(config));
        /* Stable fixture UID, fresh process/connection boot from OS CSPRNG. */
        memcpy(config.uid, "NOS-NLP-FIXTURE01", 16);
        memcpy(config.fabric, config.uid, 16);
        int random_fd = open("/dev/urandom", O_RDONLY);
        if (random_fd < 0 || read(random_fd, &config.boot_id, 8) != 8 || !config.boot_id)
            return 1;
        close(random_fd);
        /* Each fixture lifetime creates a distinct fabric, not a reused epoch. */
        nlp_put64(config.fabric + 8, config.boot_id);
        config.epoch = 1;
        config.node = 1;
        config.transport = 2;
        config.physical = !secure;
        if (secure) {
            if (nlp_auth_init(&auth, &nlp_openssl_crypto, key, pin, config.uid))
                return 1;
            config.auth = &auth;
        }
        config.permissions =
            NLP_PERMISSION_DISCOVER | NLP_PERMISSION_OBSERVE_INPUT | NLP_PERMISSION_LOG_READ;
        if (shell)
            config.permissions |= NLP_PERMISSION_SHELL;
        config.send = transmit;
        config.context = &s;
        if (nlp_endpoint_init(&s.endpoint, &config) ||
            nlp_keyboard_register(&s.keyboard, &s.endpoint, 1) ||
            nlp_log_register(&s.log, &s.endpoint, 2))
            return 1;
        if (shell && nlp_terminal_register(&s.terminal, &s.endpoint, 3, &terminal_ops))
            return 1;
        nlp_stream_init(&s.stream, NLP_FRAMING_TCP);
        nlp_log_write(&s.log, 2, 1, "NLP fixture ready", 17, 0);
        fcntl(fd, F_SETFL, O_NONBLOCK);
        for (;;) {
            uint8_t input[512], nkro[33] = {0};
            uint32_t now = now_ms();
            struct pollfd p = {fd, POLLIN | (s.used > s.sent ? POLLOUT : 0), 0};
            nlp_endpoint_poll(&s.endpoint, now);
            nlp_keyboard_update(&s.keyboard, nkro, 0, (uint64_t)now * 1000);
            nlp_keyboard_poll(&s.keyboard);
            nlp_log_poll(&s.log);
            nlp_posix_terminal_poll(&terminal);
            if (shell)
                nlp_terminal_poll(&s.terminal);
            if (poll(&p, 1, 5) < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (p.revents & (POLLERR | POLLHUP | POLLNVAL))
                break;
            if (p.revents & POLLOUT) {
                ssize_t count = send(fd, s.tx + s.sent, s.used - s.sent, MSG_NOSIGNAL);
                if (count < 0 && errno != EAGAIN && errno != EINTR)
                    break;
                if (count > 0)
                    s.sent += (size_t)count;
                if (s.sent == s.used)
                    s.sent = s.used = 0;
            }
            if (p.revents & POLLIN) {
                ssize_t count = recv(fd, input, sizeof(input), 0);
                if (!count)
                    break;
                if (count < 0) {
                    if (errno == EAGAIN || errno == EINTR)
                        continue;
                    break;
                }
                size_t consumed;
                if (nlp_stream_feed(&s.stream, input, (size_t)count, &consumed, now,
                                    nlp_endpoint_receive, &s.endpoint) ||
                    consumed != (size_t)count)
                    break;
            }
        }
        nlp_endpoint_disconnect(&s.endpoint);
        close(fd);
    }
    close(listener);
    return 0;
}
