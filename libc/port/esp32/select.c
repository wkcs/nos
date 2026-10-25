/* Copyright (C) 2026 NOS contributors. Readiness for NOS and SDK descriptors. */
#include "private.h"
#include <poll.h>
#include <sys/select.h>
#include "nos/poll_abi.h"
#include "esp_vfs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* IDF's poll bit assignments differ from the NOS wire ABI. */
static int native_events(int events) {
    return ((events & (POLLIN | POLLRDNORM)) ? NOS_POLLIN : 0) |
           ((events & POLLOUT) ? NOS_POLLOUT : 0) | ((events & POLLPRI) ? NOS_POLLPRI : 0);
}
static int libc_events(int events) {
    return ((events & NOS_POLLIN) ? POLLIN | POLLRDNORM : 0) |
           ((events & NOS_POLLOUT) ? POLLOUT : 0) | ((events & NOS_POLLPRI) ? POLLPRI : 0) |
           ((events & NOS_POLLERR) ? POLLERR : 0) | ((events & NOS_POLLHUP) ? POLLHUP : 0) |
           ((events & NOS_POLLNVAL) ? POLLNVAL : 0);
}
static int snapshot(struct pollfd *fds, nfds_t count) {
    struct nos_pollfd native[NOS_FD_LIMIT];
    for (unsigned i = 0; i < NOS_FD_LIMIT; i++)
        native[i] = (struct nos_pollfd){.fd = -1};
    for (nfds_t i = 0; i < count; i++) {
        fds[i].revents = 0;
        if (fds[i].fd >= 0 && fds[i].fd < (int)NOS_FD_LIMIT) {
            native[fds[i].fd].fd = fds[i].fd;
            native[fds[i].fd].events |= native_events(fds[i].events);
        }
    }
    int rc = (int)nos_libc_result(__getreent(), nos_poll(native, NOS_FD_LIMIT, 0));
    if (rc < 0)
        return -1;
    int ready = 0;
    for (nfds_t i = 0; i < count; i++) {
        int fd = fds[i].fd;
        if (fd < 0)
            continue;
        if (fd < (int)NOS_FD_LIMIT) {
            fds[i].revents =
                libc_events(native[fd].revents) & (fds[i].events | POLLERR | POLLHUP | POLLNVAL);
        } else if (fd >= FD_SETSIZE) {
            fds[i].revents = POLLNVAL;
        } else {
            fd_set read_set, write_set, except_set;
            FD_ZERO(&read_set);
            FD_ZERO(&write_set);
            FD_ZERO(&except_set);
            if (fds[i].events & (POLLIN | POLLRDNORM))
                FD_SET(fd, &read_set);
            if (fds[i].events & POLLOUT)
                FD_SET(fd, &write_set);
            if (fds[i].events & POLLPRI)
                FD_SET(fd, &except_set);
            struct timeval zero = {0};
            rc = esp_vfs_select(fd + 1, &read_set, &write_set, &except_set, &zero);
            if (rc < 0) {
                if (errno != EBADF)
                    return -1;
                fds[i].revents = POLLNVAL;
            } else {
                if (FD_ISSET(fd, &read_set))
                    fds[i].revents |= fds[i].events & (POLLIN | POLLRDNORM);
                if (FD_ISSET(fd, &write_set))
                    fds[i].revents |= POLLOUT;
                if (FD_ISSET(fd, &except_set))
                    fds[i].revents |= POLLPRI;
            }
        }
        ready += fds[i].revents != 0;
    }
    return ready;
}
static int wait_until(struct pollfd *fds, nfds_t count, int64_t deadline) {
    for (;;) {
        int rc = snapshot(fds, count);
        if (rc || esp_timer_get_time() >= deadline)
            return rc;
        vTaskDelay(1);
    }
}
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout_ms) {
    int context = nos_libc_context(__getreent());
    if (context != 1)
        return (int)nos_libc_result(__getreent(), context < 0 ? context : -NOS_ENOSYS);
    if (count > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }
    int rc = nos_user_check(fds, count * sizeof(*fds), NOS_PROT_READ | NOS_PROT_WRITE);
    if (rc)
        return (int)nos_libc_result(__getreent(), rc);
    int64_t deadline =
        timeout_ms < 0 ? INT64_MAX : esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    return wait_until(fds, count, deadline);
}
int __wrap_select(int nfds, fd_set *read_set, fd_set *write_set, fd_set *except_set,
                  struct timeval *timeout) {
    if (nfds < 0 || nfds > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }
    int native = 0;
    for (int fd = 0; fd < nfds && fd < (int)NOS_FD_LIMIT; fd++)
        native |= (read_set && FD_ISSET(fd, read_set)) || (write_set && FD_ISSET(fd, write_set)) ||
                  (except_set && FD_ISSET(fd, except_set));
    if (!native)
        return esp_vfs_select(nfds, read_set, write_set, except_set, timeout);
    if (timeout && (timeout->tv_sec < 0 || timeout->tv_usec < 0 || timeout->tv_usec >= 1000000)) {
        errno = EINVAL;
        return -1;
    }
    int context = nos_libc_context(__getreent());
    if (context != 1)
        return (int)nos_libc_result(__getreent(), context < 0 ? context : -NOS_ENOSYS);
    int64_t now = esp_timer_get_time(), deadline = INT64_MAX;
    if (timeout) {
        if ((uint64_t)timeout->tv_sec > (uint64_t)(INT64_MAX - now - timeout->tv_usec) / 1000000) {
            errno = EINVAL;
            return -1;
        }
        deadline = now + (int64_t)timeout->tv_sec * 1000000 + timeout->tv_usec;
    }
    struct pollfd *fds = calloc(nfds ? nfds : 1, sizeof(*fds));
    if (!fds)
        return -1;
    for (int fd = 0; fd < nfds; fd++) {
        fds[fd].fd = -1;
        if (read_set && FD_ISSET(fd, read_set))
            fds[fd].events |= POLLIN;
        if (write_set && FD_ISSET(fd, write_set))
            fds[fd].events |= POLLOUT;
        if (except_set && FD_ISSET(fd, except_set))
            fds[fd].events |= POLLPRI;
        if (fds[fd].events)
            fds[fd].fd = fd;
    }
    int rc = wait_until(fds, nfds, deadline), ready = 0;
    if (rc >= 0) {
        if (read_set)
            FD_ZERO(read_set);
        if (write_set)
            FD_ZERO(write_set);
        if (except_set)
            FD_ZERO(except_set);
        for (int fd = 0; fd < nfds; fd++) {
            if (fds[fd].revents & POLLNVAL) {
                errno = EBADF;
                rc = -1;
                break;
            }
            if (read_set && (fds[fd].revents & (POLLIN | POLLHUP))) {
                FD_SET(fd, read_set);
                ready++;
            }
            if (write_set && (fds[fd].revents & POLLOUT)) {
                FD_SET(fd, write_set);
                ready++;
            }
            if (except_set && (fds[fd].revents & POLLPRI)) {
                FD_SET(fd, except_set);
                ready++;
            }
        }
    }
    free(fds);
    return rc < 0 ? -1 : ready;
}
