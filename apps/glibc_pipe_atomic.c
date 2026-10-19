/* Copyright (C) 2026 NOS contributors. Real pipe atomicity and iovec checks. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RECORD_BYTES 4096u
#define RECORD_COUNT 4u
#define LARGE_BYTES (3u * RECORD_BYTES + 37u)
#define DEADLINE_MS 10000
static int fail(const char *step)
{
    fprintf(stderr, "FAIL glibc_pipe_atomic %s errno=%d\n", step, errno);
    return 98;
}
static int nonblock(int fd, int enabled)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
}
static int readable(int fd, int milliseconds)
{
    struct pollfd item = {fd, POLLIN, 0};
    int rc;
    do rc = poll(&item, 1, milliseconds); while (rc < 0 && errno == EINTR);
    if (!rc && milliseconds == DEADLINE_MS) errno = ETIMEDOUT;
    return rc;
}
static int byte_in(int fd, char expected)
{
    char byte;
    return readable(fd, DEADLINE_MS) == 1 && read(fd, &byte, 1) == 1 && byte == expected ? 0 : -1;
}
static int wait_ok(pid_t child)
{
    int status;
    pid_t rc;
    do rc = waitpid(child, &status, 0); while (rc < 0 && errno == EINTR);
    return rc == child && WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
}
static int read_all(int fd, void *buffer, size_t bytes)
{
    size_t done = 0;
    while (done < bytes) {
        if (readable(fd, DEADLINE_MS) != 1) return -1;
        ssize_t n = read(fd, (char *)buffer + done, bytes - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}
static int drain_fill(int fd, size_t bytes)
{
    unsigned char buffer[RECORD_BYTES];
    while (bytes) {
        size_t n = bytes < sizeof(buffer) ? bytes : sizeof(buffer);
        if (read_all(fd, buffer, n)) return -1;
        for (size_t i = 0; i < n; i++) if (buffer[i] != 0x54) { errno = EILSEQ; return -1; }
        bytes -= n;
    }
    return 0;
}
static int empty(int fd)
{
    unsigned char byte;
    if (nonblock(fd, 1)) return -1;
    errno = 0;
    return read(fd, &byte, 1) == -1 && errno == EAGAIN ? 0 : -1;
}
/* Discover capacity, rather than assuming PIPE_BUF equals pipe capacity. */
static size_t fill(int fd)
{
    unsigned char buffer[RECORD_BYTES];
    memset(buffer, 0x54, sizeof(buffer));
    if (nonblock(fd, 1)) return 0;
    size_t total = 0;
    for (;;) {
        ssize_t n = write(fd, buffer, sizeof(buffer));
        if (n == -1 && errno == EAGAIN) return total;
        if (n != (ssize_t)sizeof(buffer) || total >= 1024u * 1024u) return 0;
        total += (size_t)n;
    }
}
static unsigned char pattern(unsigned writer, unsigned sequence, size_t offset)
{ return (unsigned char)(writer * 79u + sequence * 17u + offset * 13u); }
static void record_fill(unsigned char *buffer, unsigned writer, unsigned sequence)
{
    uint32_t header[] = {0x50495045u, writer, sequence, RECORD_BYTES};
    memcpy(buffer, header, sizeof(header));
    for (size_t i = sizeof(header); i < RECORD_BYTES; i++) buffer[i] = pattern(writer, sequence, i);
}
static int record_check(const unsigned char *buffer, unsigned expected[2])
{
    uint32_t h[4];
    memcpy(h, buffer, sizeof(h));
    if (h[0] != 0x50495045u || h[1] >= 2 || h[2] != expected[h[1]] || h[3] != RECORD_BYTES) return -1;
    for (size_t i = sizeof(h); i < RECORD_BYTES; i++)
        if (buffer[i] != pattern(h[1], h[2], i)) return -1;
    expected[h[1]]++;
    return 0;
}
/* Two independent address spaces synchronize before either writes. Only one
 * data pipe is used, so fork and descriptor pressure stay below NOS limits. */
static int process_records(unsigned mode)
{
    int data[2];
    if (pipe(data)) return fail("records_pipe");
    unsigned *start = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (start == MAP_FAILED) return fail("records_start_map");
    memset(start, 0, 4096);
    pid_t children[2];
    for (unsigned writer = 0; writer < 2; writer++) {
        children[writer] = fork();
        if (!children[writer]) {
            close(data[0]);
            unsigned char buffer[RECORD_BYTES];
            struct iovec *vectors = calloc(1024, sizeof(*vectors));
            if (!vectors) _exit(10);
            for (unsigned i = 0; i < 1024; i++) vectors[i] = (struct iovec){buffer + 4u * i, 4};
            __atomic_store_n(start + writer, 1, __ATOMIC_RELEASE);
            while (!__atomic_load_n(start + 2, __ATOMIC_ACQUIRE)) sched_yield();
            for (unsigned sequence = 0; sequence < RECORD_COUNT; sequence++) {
                record_fill(buffer, writer, sequence);
                struct iovec parts[] = {{buffer, 17}, {buffer + 17, 2027}, {buffer + 2044, 2052}};
                ssize_t n;
                do n = !mode ? write(data[1], buffer, sizeof(buffer)) :
                    mode == 1 ? writev(data[1], parts, 3) : writev(data[1], vectors, 1024);
                while (n < 0 && errno == EINTR);
                if (n != (ssize_t)sizeof(buffer)) _exit(11);
            }
            free(vectors); close(data[1]); _exit(0);
        }
        if (children[writer] < 0) return fail("records_fork");
    }
    struct timespec initial, now;
    if (clock_gettime(CLOCK_MONOTONIC, &initial)) return fail("records_clock");
    while (!__atomic_load_n(start, __ATOMIC_ACQUIRE) || !__atomic_load_n(start + 1, __ATOMIC_ACQUIRE)) {
        sched_yield();
        if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec - initial.tv_sec >= 10) return fail("records_ready_deadline");
    }
    __atomic_store_n(start + 2, 1, __ATOMIC_RELEASE);
    close(data[1]);
    unsigned char buffer[RECORD_BYTES];
    unsigned expected[2] = {0, 0};
    for (unsigned r = 0; r < 2u * RECORD_COUNT; r++) {
        /* Fragment the reader independently of the write boundaries. */
        for (size_t offset = 0; offset < sizeof(buffer);) {
            size_t bytes = sizeof(buffer) - offset < 511 ? sizeof(buffer) - offset : 511;
            if (read_all(data[0], buffer + offset, bytes)) return fail("records_read");
            offset += bytes;
        }
        if (record_check(buffer, expected)) return fail("records_interleaved_or_reordered");
    }
    if (expected[0] != RECORD_COUNT || expected[1] != RECORD_COUNT || wait_ok(children[0]) || wait_ok(children[1]))
        return fail("records_child_status");
    if (read(data[0], buffer, 1) != 0 || close(data[0]) || munmap(start, 4096)) return fail("records_eof_cleanup");
    return 0;
}
static int nonblocking_atomic(void)
{
    int data[2];
    if (pipe2(data, O_NONBLOCK)) return fail("nonblock_pipe");
    size_t capacity = fill(data[1]);
    if (capacity < RECORD_BYTES || drain_fill(data[0], 1)) return fail("nonblock_capacity");
    unsigned char bytes[RECORD_BYTES];
    memset(bytes, 0xa5, sizeof(bytes));
    struct iovec parts[] = {{bytes, 17}, {bytes + 17, RECORD_BYTES - 17}};
    errno = 0;
    if (write(data[1], bytes, sizeof(bytes)) != -1 || errno != EAGAIN) return fail("nonblock_scalar_partial_atomic");
    errno = 0;
    if (writev(data[1], parts, 2) != -1 || errno != EAGAIN) return fail("nonblock_vector_partial_atomic");
    if (drain_fill(data[0], capacity - 1) || empty(data[0]) || close(data[0]) || close(data[1]))
        return fail("nonblock_atomic_ghost_or_count");
    puts("PASS glibc_pipe_atomic_nonblock_no_partial");
    return 0;
}
static int large_nonblock(unsigned vectored)
{
    int data[2];
    if (pipe2(data, O_NONBLOCK)) return fail("large_nonblock_pipe");
    size_t capacity = fill(data[1]);
    if (capacity < RECORD_BYTES || drain_fill(data[0], capacity)) return fail("large_nonblock_capacity");
    unsigned char *original = malloc(capacity), buffer[RECORD_BYTES + 127];
    if (!original) return fail("large_nonblock_malloc");
    memset(original, 0x54, capacity); memset(buffer, 0xa5, sizeof(buffer));
    /* A 127-byte mergeable tail also exercises real Linux pipes, whose
     * available page slots are distinct from the nominal byte capacity. */
    ssize_t filled = write(data[1], original, capacity - 127);
    free(original);
    if (filled != (ssize_t)(capacity - 127)) return fail("large_nonblock_prepare_tail");
    struct iovec parts[66];
    for (unsigned i = 0; i < 65; i++) parts[i] = (struct iovec){buffer + 64u * i, 64};
    parts[65] = (struct iovec){buffer + 4160, sizeof(buffer) - 4160};
    /* Use individually small iovecs: the original sum, not each iov_len,
     * decides whether a nonblocking write may publish a partial prefix. */
    ssize_t n = vectored ? writev(data[1], parts, 66) : write(data[1], buffer, sizeof(buffer));
    if (n != 127) return fail("large_nonblock_missing_available_prefix");
    if (drain_fill(data[0], capacity - 127)) return fail("large_nonblock_original_corrupt");
    unsigned char prefix[127];
    if (read_all(data[0], prefix, (size_t)n)) return fail("large_nonblock_prefix_read");
    for (ssize_t i = 0; i < n; i++) if (prefix[i] != 0xa5) return fail("large_nonblock_prefix_corrupt");
    if (empty(data[0]) || close(data[0]) || close(data[1])) return fail("large_nonblock_ghost");
    return 0;
}

struct writer {
    int fd, notify, vectored;
    ssize_t result;
    int error;
    unsigned cleanup;
    unsigned char bytes[RECORD_BYTES];
};
static void writer_cleanup(void *argument)
{
    struct writer *w = argument;
    w->cleanup++;
    (void)write(w->notify, "K", 1);
}
static void *write_record(void *argument)
{
    struct writer *w = argument;
    pthread_cleanup_push(writer_cleanup, w);
    if (write(w->notify, "R", 1) != 1) { w->result = -2; w->error = errno; }
    else {
        struct iovec parts[] = {{w->bytes, 17}, {w->bytes + 17, RECORD_BYTES - 17}};
        w->result = w->vectored ? writev(w->fd, parts, 2) : write(w->fd, w->bytes, sizeof(w->bytes));
        w->error = errno;
    }
    (void)write(w->notify, "C", 1);
    pthread_cleanup_pop(0);
    return NULL;
}
static volatile sig_atomic_t signals_seen;
static void interrupt_writer(int signal_number)
{ (void)signal_number; signals_seen++; }
/* The ready handshake is followed by a timed observation of an empty
 * completion pipe while the data pipe is full. No guessed sleep is used to
 * infer that a write is blocked, and every later event has a 10s deadline. */
static int blocked_writer(unsigned mode)
{
    int data[2], events[2];
    if (pipe(data) || pipe(events)) return fail("blocked_pipes");
    size_t capacity = fill(data[1]);
    if (capacity < RECORD_BYTES || nonblock(data[1], 0)) return fail("blocked_fill");
    struct writer w = {.fd = data[1], .notify = events[1], .vectored = mode == 2};
    memset(w.bytes, 0xa5, sizeof(w.bytes));
    pthread_t thread;
    if (pthread_create(&thread, NULL, write_record, &w)) return fail("blocked_create");
    if (byte_in(events[0], 'R') || readable(events[0], 50) != 0) return fail("blocked_completed_when_full");
    void *result;
    if (!mode) {
        if (drain_fill(data[0], 1) || readable(events[0], 50) != 0) return fail("blocked_partial_space_committed");
        if (drain_fill(data[0], capacity - 1) || byte_in(events[0], 'C') || pthread_join(thread, &result) ||
            result || w.result != RECORD_BYTES || w.cleanup) return fail("blocked_whole_record_wakeup");
        unsigned char bytes[RECORD_BYTES];
        if (read_all(data[0], bytes, sizeof(bytes))) return fail("blocked_record_read");
        for (size_t i = 0; i < sizeof(bytes); i++) if (bytes[i] != 0xa5) return fail("blocked_record_corrupt");
    } else if (mode == 1) {
        /* A signal racing just before syscall entry is legitimate. Repeat
         * a directed signal until the still-full writer actually returns;
         * only -1/EINTR, with no published prefix, is accepted. */
        struct timespec initial, now;
        if (clock_gettime(CLOCK_MONOTONIC, &initial)) return fail("interrupt_clock");
        for (;;) {
            if (pthread_kill(thread, SIGUSR1)) return fail("interrupt_kill");
            int ready = readable(events[0], 50);
            if (ready < 0) return fail("interrupt_poll");
            if (ready) break;
            if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec - initial.tv_sec >= 10)
                return fail("interrupt_deadline");
        }
        if (byte_in(events[0], 'C') || pthread_join(thread, &result) || result ||
            w.result != -1 || w.error != EINTR || !signals_seen || w.cleanup)
            return fail("interrupt_write_result");
        if (drain_fill(data[0], capacity)) return fail("interrupt_changed_original");
    } else {
        if (pthread_cancel(thread) || byte_in(events[0], 'K') || pthread_join(thread, &result) ||
            result != PTHREAD_CANCELED || w.cleanup != 1) return fail("cancel_write_result");
        if (drain_fill(data[0], capacity)) return fail("cancel_changed_original");
    }
    if (empty(data[0]) || close(data[0]) || close(data[1]) || close(events[0]) || close(events[1]))
        return fail("blocked_ghost_or_cleanup");
    puts(!mode ? "PASS glibc_pipe_atomic_blocked_wakeup" : mode == 1 ?
        "PASS glibc_pipe_atomic_signal_eintr_no_ghost" : "PASS glibc_pipe_atomic_cancel_no_ghost");
    return 0;
}

/* No status pipe is needed for the descriptor race: a condition variable
 * reports readiness and completion, leaving the second pipe for the new OFD. */
struct pinned_writer {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    int fd, ready, done;
    ssize_t result;
    unsigned char bytes[RECORD_BYTES];
};
static void *write_pinned(void *argument)
{
    struct pinned_writer *w = argument;
    pthread_mutex_lock(&w->lock);
    w->ready = 1; pthread_cond_signal(&w->condition);
    pthread_mutex_unlock(&w->lock);
    ssize_t result = write(w->fd, w->bytes, sizeof(w->bytes));
    pthread_mutex_lock(&w->lock);
    w->result = result; w->done = 1; pthread_cond_signal(&w->condition);
    pthread_mutex_unlock(&w->lock);
    return NULL;
}
static int condition_until(struct pinned_writer *w, int done, long milliseconds)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline)) return -1;
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += milliseconds % 1000 * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_nsec -= 1000000000L; deadline.tv_sec++; }
    int rc = 0;
    while (!(done ? w->done : w->ready) && !rc) rc = pthread_cond_timedwait(&w->condition, &w->lock, &deadline);
    return rc;
}
static int close_reuse(void)
{
    int old[2], replacement[2];
    if (pipe(old) || pipe2(replacement, O_NONBLOCK)) return fail("pin_pipes");
    size_t capacity = fill(old[1]);
    if (capacity < RECORD_BYTES || nonblock(old[1], 0)) return fail("pin_fill");
    struct pinned_writer w = {.lock = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER, .fd = old[1]};
    memset(w.bytes, 0xa5, sizeof(w.bytes));
    pthread_t thread;
    if (pthread_create(&thread, NULL, write_pinned, &w)) return fail("pin_create");
    pthread_mutex_lock(&w.lock);
    if (condition_until(&w, 0, DEADLINE_MS) || condition_until(&w, 1, 50) != ETIMEDOUT || w.done)
        return fail("pin_not_blocked");
    pthread_mutex_unlock(&w.lock);
    int reused = old[1];
    if (close(reused) || dup2(replacement[1], reused) != reused) return fail("pin_close_reuse");
    if (drain_fill(old[0], capacity)) return fail("pin_original_corrupt");
    pthread_mutex_lock(&w.lock);
    if (condition_until(&w, 1, DEADLINE_MS)) return fail("pin_completion_deadline");
    pthread_mutex_unlock(&w.lock);
    if (pthread_join(thread, NULL) || w.result != RECORD_BYTES) return fail("pin_write_result");
    unsigned char bytes[RECORD_BYTES];
    if (read_all(old[0], bytes, sizeof(bytes))) return fail("pin_record_wrong_ofd");
    for (size_t i = 0; i < sizeof(bytes); i++) if (bytes[i] != 0xa5) return fail("pin_record_corrupt");
    if (read(old[0], bytes, 1) != 0 || empty(replacement[0])) return fail("pin_reused_fd_received_ghost");
    if (close(old[0]) || close(reused) || close(replacement[0]) || close(replacement[1]) ||
        pthread_cond_destroy(&w.condition) || pthread_mutex_destroy(&w.lock)) return fail("pin_cleanup");
    puts("PASS glibc_pipe_ofd_close_reuse_pin");
    return 0;
}
static int large_blocking(int vectored)
{
    int data[2];
    if (pipe(data)) return fail("large_pipe");
    pid_t child = fork();
    if (!child) {
        close(data[1]);
        unsigned char buffer[511]; size_t done = 0;
        for (;;) {
            ssize_t n = read(data[0], buffer, sizeof(buffer));
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) _exit(20);
            if (!n) break;
            for (ssize_t i = 0; i < n; i++) if (buffer[i] != (unsigned char)((done + (size_t)i) * 7u)) _exit(21);
            done += (size_t)n;
        }
        close(data[0]); _exit(done == LARGE_BYTES ? 0 : 22);
    }
    if (child < 0) return fail("large_fork");
    close(data[0]);
    unsigned char *buffer = malloc(LARGE_BYTES);
    if (!buffer) return fail("large_malloc");
    for (size_t i = 0; i < LARGE_BYTES; i++) buffer[i] = (unsigned char)(i * 7u);
    struct iovec parts[] = {{buffer, 17}, {buffer + 17, 2027}, {buffer + 2044, LARGE_BYTES - 2044}};
    ssize_t n;
    do n = vectored ? writev(data[1], parts, 3) : write(data[1], buffer, LARGE_BYTES);
    while (n < 0 && errno == EINTR);
    free(buffer);
    if (close(data[1]) || wait_ok(child) || n != LARGE_BYTES) return fail("large_blocking_returned_early");
    return 0;
}
static volatile sig_atomic_t broken_signals;
static void broken_pipe_signal(int signal_number)
{ (void)signal_number; broken_signals++; }
static int partial_broken_pipe(int vectored)
{
    int data[2];
    if (pipe(data)) return fail("partial_epipe_pipe");
    /* NOS has a 4096-byte pipe. A Linux host may start with a larger pipe;
     * shrink that pipe before the identical black-box experiment, so an
     * 8192-byte write cannot finish before the reader closes. */
    size_t capacity = fill(data[1]);
    if (capacity < RECORD_BYTES || drain_fill(data[0], capacity) || nonblock(data[1], 0))
        return fail("partial_epipe_capacity");
    if (capacity > RECORD_BYTES && fcntl(data[1], F_SETPIPE_SZ, RECORD_BYTES) != RECORD_BYTES)
        return fail("partial_epipe_host_pipe_size");
    struct sigaction action = {.sa_handler = broken_pipe_signal}, saved;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, &saved)) return fail("partial_epipe_handler");
    broken_signals = 0;
    pid_t child = fork();
    if (!child) {
        unsigned char byte;
        if (close(data[1]) || read(data[0], &byte, 1) != 1 || byte != 0x83 || close(data[0])) _exit(30);
        _exit(0);
    }
    if (child < 0 || close(data[0])) return fail("partial_epipe_fork");
    unsigned char bytes[2u * RECORD_BYTES];
    memset(bytes, 0x83, sizeof(bytes));
    struct iovec parts[] = {{bytes, RECORD_BYTES}, {bytes + RECORD_BYTES, RECORD_BYTES}};
    /* The reader's first successful read proves a prefix was published;
     * closing its only read descriptor then causes a real EPIPE while the
     * rest is still pending. No sleep or signal injection models that race.
     * POSIX permits a positive partial count; do not hard-code NOS's chunk
     * size into the asserted prefix length. SIGPIPE must still be raised. */
    ssize_t n = vectored ? writev(data[1], parts, 2) : write(data[1], bytes, sizeof(bytes));
    if (n <= 0 || n >= (ssize_t)sizeof(bytes) || broken_signals != 1)
        return fail(vectored ? "partial_epipe_writev_prefix_signal" : "partial_epipe_write_prefix_signal");
    if (wait_ok(child) || close(data[1]) || sigaction(SIGPIPE, &saved, NULL)) return fail("partial_epipe_cleanup");
    return 0;
}
static int iovec_bounds(void)
{
    int data[2];
    if (pipe2(data, O_NONBLOCK)) return fail("iov_pipe");
    unsigned char buffer[RECORD_BYTES];
    memset(buffer, 0x37, sizeof(buffer));
    size_t page = (size_t)sysconf(_SC_PAGESIZE), arrays = 1024 * sizeof(struct iovec);
    size_t guard = (arrays + page - 1) & ~(page - 1);
    unsigned char *mapping = mmap(NULL, guard + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED || mprotect(mapping + guard, page, PROT_NONE)) return fail("iov_map");
    struct iovec *vectors = (void *)(mapping + guard - arrays);
    for (unsigned i = 0; i < 1024; i++) vectors[i] = (struct iovec){buffer + 4u * i, 4};
    if (writev(data[1], vectors, 1024) != RECORD_BYTES || readv(data[0], vectors, 1024) != RECORD_BYTES)
        return fail("iov1024_exact_guard_boundary");
    for (size_t i = 0; i < sizeof(buffer); i++) if (buffer[i] != 0x37) return fail("iov1024_data_corrupt");
    errno = 0;
    if (syscall(SYS_writev, data[1], vectors, 1025) != -1 || errno != EINVAL) return fail("iov1025_not_einval");
    errno = 0;
    if (syscall(SYS_readv, data[0], vectors, 1025) != -1 || errno != EINVAL) return fail("read_iov1025_not_einval");
    errno = 0;
    if (writev(data[1], vectors + 1, 1024) != -1 || errno != EFAULT)
        return fail("iov_array_cross_guard");
    if (write(data[1], buffer, sizeof(buffer)) != RECORD_BYTES) return fail("read_iov_array_guard_setup");
    errno = 0;
    if (readv(data[0], vectors + 1, 1024) != -1 || errno != EFAULT)
        return fail("read_iov_array_cross_guard");
    if (readv(data[0], vectors, 1024) != RECORD_BYTES) return fail("read_iov_array_fault_consumed_data");
    /* NOS deliberately validates every pointed-to range before any IPC.
     * General Linux readv/writev may return a completed prefix on later
     * data-buffer faults; this stronger zero-publication assertion is NOS's
     * contract, not a claim about all POSIX systems. */
    vectors[1023] = (struct iovec){mapping + guard, 4};
    errno = 0;
    if (writev(data[1], vectors, 1024) != -1 || errno != EFAULT || empty(data[0]))
        return fail("iov_late_guard_published_prefix");
    if (munmap(mapping, guard + page) || close(data[0]) || close(data[1])) return fail("iov_cleanup");
    puts("PASS glibc_pipe_iov1024_guard_precheck");
    return 0;
}
int main(void)
{
    int data[2];
    if (pipe(data)) return fail("limits_pipe");
    if (sysconf(_SC_IOV_MAX) != 1024 || fpathconf(data[1], _PC_PIPE_BUF) != RECORD_BYTES ||
        close(data[0]) || close(data[1])) return fail("advertised_limits");
    puts("PASS glibc_pipe_limits_4096_iov1024");
    int rc = process_records(0);
    if (!rc) rc = process_records(1);
    if (!rc) rc = process_records(2);
    if (rc) return rc;
    puts("PASS glibc_pipe_atomic_process_records");
    if ((rc = nonblocking_atomic()) || (rc = large_nonblock(0)) || (rc = large_nonblock(1))) return rc;
    puts("PASS glibc_pipe_large_nonblock_prefix");
    struct sigaction action = {.sa_handler = interrupt_writer}, saved;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, &saved)) return fail("interrupt_install");
    if ((rc = blocked_writer(0)) || (rc = blocked_writer(1)) || (rc = blocked_writer(2))) return rc;
    if (sigaction(SIGUSR1, &saved, NULL) || (rc = close_reuse()) || (rc = large_blocking(0)) ||
        (rc = large_blocking(1))) return rc ? rc : fail("interrupt_restore");
    puts("PASS glibc_pipe_large_blocking_progress");
    if ((rc = partial_broken_pipe(0)) || (rc = partial_broken_pipe(1))) return rc;
    puts("PASS glibc_pipe_partial_epipe_sigpipe_prefix");
    return iovec_bounds();
}
