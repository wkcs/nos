/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#if __has_include(<nos/shell_session.h>)
#include <nos/shell_session.h>
#else
#include <stdio.h>
int main(void) { fputs("FAIL: shell session interface unavailable\n", stderr); return 1; }
#endif
#ifdef NOS_SHELL_SESSION_API
#include <assert.h>
#include <string.h>

struct endpoint { char output[1024]; unsigned int used; };
static int write_endpoint(void *context, const unsigned char *buf, unsigned int len)
{
    struct endpoint *ep = context;
    assert(ep->used + len <= sizeof(ep->output));
    memcpy(ep->output + ep->used, buf, len);
    ep->used += len;
    return (int)len;
}
static void run_command(struct shell_session *session, void *priv)
{
    (void)priv;
    shell_session_write(session, "result:", 7);
    shell_session_write(session, session->input, (unsigned int)strlen(session->input));
    shell_session_write(session, "\r\n", 2);
}

int main(void)
{
    struct endpoint a = {0}, b = {0};
    struct nos_tty_ops ops = { .write = write_endpoint };
    struct nos_tty ta, tb;
    struct shell_session sa, sb;
    struct dentry *cwd_a = (struct dentry *)1, *cwd_b = (struct dentry *)2;
    assert(nos_tty_init(&ta, &ops, &a) == 0);
    assert(nos_tty_init(&tb, &ops, &b) == 0);
    shell_session_init(&sa, &ta, cwd_a);
    shell_session_init(&sb, &tb, cwd_b);
    strcpy(sa.input, "partial");
    sa.input_len = 7;
    assert(sb.input_len == 0 && sb.input[0] == '\0');
    assert(sa.cwd == cwd_a && sb.cwd == cwd_b);
    assert(shell_session_write(&sa, "one", 3) == 3);
    assert(shell_session_write(&sb, "two", 3) == 3);
    assert(memcmp(a.output, "one", 3) == 0 && memcmp(b.output, "two", 3) == 0);
    shell_session_reset(&sa, cwd_b);
    assert(sa.input_len == 0 && sa.input[0] == '\0' && sa.cwd == cwd_b);
    assert(sb.cwd == cwd_b && b.used == 3);
    shell_session_history_add(&sa, "private command");
    shell_session_reset(&sa, cwd_a);
    assert(shell_session_history_count(&sa) == 0);
#ifdef NOS_SHELL_FEED_API
    a.used = b.used = 0;
    shell_session_reset(&sa, cwd_a);
    shell_session_reset(&sb, cwd_b);
    shell_session_feed(&sa, 'v', run_command, NULL);
    shell_session_feed(&sb, 'p', run_command, NULL);
    shell_session_feed(&sa, 'e', run_command, NULL);
    shell_session_feed(&sb, 'w', run_command, NULL);
    shell_session_feed(&sa, '\r', run_command, NULL);
    shell_session_feed(&sb, '\r', run_command, NULL);
    assert(a.used == strlen("ve\r\nresult:ve\r\nnos# "));
    assert(b.used == strlen("pw\r\nresult:pw\r\nnos# "));
    assert(memcmp(a.output, "ve\r\nresult:ve\r\nnos# ", a.used) == 0);
    assert(memcmp(b.output, "pw\r\nresult:pw\r\nnos# ", b.used) == 0);
    a.used = 0;
    for (unsigned int i = 0; i < SHELL_SESSION_INPUT_MAX + 1; i++)
        shell_session_feed(&sa, 'x', run_command, NULL);
    shell_session_feed(&sa, '\r', run_command, NULL);
    assert(strstr(a.output, "line too long") != NULL);
    assert(strstr(a.output, "result:") == NULL);
    a.used = 0;
    shell_session_feed(&sa, 0xe4, run_command, NULL);
    shell_session_feed(&sa, 0xbd, run_command, NULL);
    shell_session_feed(&sa, 0xa0, run_command, NULL);
    shell_session_feed(&sa, 0xe5, run_command, NULL);
    shell_session_feed(&sa, 0xa5, run_command, NULL);
    shell_session_feed(&sa, 0xbd, run_command, NULL);
    shell_session_feed(&sa, 0x7f, run_command, NULL);
    shell_session_feed(&sa, '\r', run_command, NULL);
    assert(strstr(a.output, "result:\xe4\xbd\xa0\r\n") != NULL);
#else
    assert(!"shell interpreter must own editing state per session");
#endif
    return 0;
}

#endif
