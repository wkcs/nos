/* Copyright (C) 2026 NOS contributors. Host bounds tests only; OS and device transport are stubbed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../servers/vfs/server.c"
#include "../servers/vfs/regression.c"
static unsigned failed;
static struct nos_message *host_reply;
static uint64_t host_token;
static unsigned host_replied;
void *u_memcpy(void *a, const void *b, size_t n) { return memcpy(a,b,n); }
void *u_memset(void *a, int b, size_t n) { return memset(a,b,n); }
size_t u_strlen(const char *s) { return strlen(s); }
int u_streq(const char *a,const char *b) { return !strcmp(a,b); }
intptr_t nos_syscall(uintptr_t n, uintptr_t a, uintptr_t b, uintptr_t c)
{
    (void)a; (void)c;
    if (n == NOS_SYS_REPLY && host_reply && ((struct nos_message *)b)->token == host_reply->token) {
        *host_reply = *(struct nos_message *)b; host_replied = 1; return 0;
    }
    return -NOS_EACCES;
}
void nos_uart_init(void) { }
int nos_uart_readable(void) { return 0; }
int nos_uart_writable(void) { return 1; }
void nos_uart_get_stats(struct nos_uart_stats *s) { memset(s, 0, sizeof(*s)); }
intptr_t nos_random_read(void *buffer, size_t length, unsigned flags) { (void)buffer; (void)length; (void)flags; return -NOS_ENOTSUP; }
uint64_t nos_block_capacity(void) { return 0; }
uint64_t nos_block_interrupts(void) { return 0; }
uint64_t nos_block_requests(void) { return 0; }
uint64_t nos_block_poll_fallbacks(void) { return 0; }
unsigned nos_block_version(void) { return 0; }
intptr_t nos_device_uart_read(void) { return -NOS_EAGAIN; }
int nos_block_init(void) { return -NOS_ENOTSUP; }
int nos_block_read(unsigned n,void *b) { (void)n;(void)b;return -NOS_ENOTSUP; }
int nos_block_write(unsigned n,const void *b) { (void)n;(void)b;return -NOS_ENOTSUP; }
int nos_block_flush(void) { return -NOS_ENOTSUP; }
int nos_fat_mount(int n) { (void)n;return -NOS_ENOTSUP; }
int nos_fat_list(const char *p,unsigned i,char *n,unsigned *m,size_t *s) { (void)p;(void)i;(void)n;(void)m;(void)s;return -NOS_ENOTSUP; }
int nos_fat_read(const char *p,void *b,size_t n) { (void)p;(void)b;(void)n;return -NOS_ENOTSUP; }
int nos_fat_write(const char *p,const void *b,size_t n) { (void)p;(void)b;(void)n;return -NOS_ENOTSUP; }
int nos_fat_mkdir(const char *p) { (void)p;return -NOS_ENOTSUP; }
int nos_fat_unlink(const char *p) { (void)p;return -NOS_ENOTSUP; }
int nos_fat_rename(const char *p,const char *n) { (void)p;(void)n;return -NOS_ENOTSUP; }
int nos_fat_sync(void) { return -NOS_ENOTSUP; }
intptr_t nos_rpc(struct nos_message *m)
{
    m->sender = 1; m->process = 1; m->token = ++host_token;
    host_reply = m; host_replied = 0; defer_reply = 0;
    service(m);
    if (defer_reply) {
        pipe_complete();
        if (!host_replied) { pipe_cancel(1, 0, 0); m->value = -NOS_EAGAIN; }
    }
    host_reply = NULL;
    return m->value;
}
static int path_op(int op,const char *p,int f) { struct nos_message m={.op=op,.arg=f,.len=strlen(p)+1};strcpy(m.data,p);return nos_rpc(&m); }
int nos_open(const char *p,unsigned f) { return path_op(NOS_VFS_OPEN,p,f); }
int nos_unlink(const char *p) { return path_op(NOS_VFS_UNLINK,p,0); }
int nos_mkdir(const char *p) { return path_op(NOS_VFS_MKDIR,p,0); }
int nos_close(int f) { return request(NOS_VFS_CLOSE,f,0); }
intptr_t nos_seek(int f,intptr_t v,int w) { struct nos_message m={.op=NOS_VFS_SEEK,.arg=f,.value=v,.len=w};return nos_rpc(&m); }
int nos_stat_fd(int f,struct nos_stat *s) { struct nos_message m={.op=NOS_VFS_STAT,.arg=f};int r=nos_rpc(&m);if(!r)memcpy(s,m.data,sizeof(*s));return r; }
intptr_t nos_write(int f,const void *b,size_t n) { struct nos_message m={.op=NOS_VFS_WRITE,.arg=f,.len=n};memcpy(m.data,b,n);return nos_rpc(&m); }
intptr_t nos_read(int f,void *b,size_t n) { struct nos_message m={.op=NOS_VFS_READ,.arg=f,.len=n};intptr_t r=nos_rpc(&m);if(r>0)memcpy(b,m.data,r);return r; }
void nos_check(int ok,const char *n) { if (!strcmp(n, "device_capability")) { puts("HOST SKIP device_capability"); return; } printf("HOST %s %s\n",ok?"PASS":"FAIL",n);failed+=!ok; }
int main(void) {
const char *initial[]={"/","/tmp","/dev","/proc","/dev/console","/proc/version"};
for(unsigned i=0;i<6;i++){strcpy(nodes[i].name,initial[i]);nodes[i].used=nodes[i].linked=1;nodes[i].mode=i<4?MODE_DIR:i==4?MODE_CHAR:MODE_FILE;}
for(unsigned i=0;i<3;i++)opened[i]=(struct description){.node=4,.kind=1,.flags=i?NOS_O_WRONLY:0};
for(unsigned i=0;i<NOS_TASKS;i++)fds[i].pid=-1;
struct nos_message large_write={.op=NOS_VFS_WRITE,.arg=1,.len=1,.value=NOS_IO_WRITE_LARGE};
large_write.data[0]='X';
nos_check(nos_rpc(&large_write)==-NOS_EINVAL,"default_large_write_flag_rejected");
struct nos_message invalid_read={.op=NOS_VFS_READ,.arg=0,.len=1,.value=NOS_IO_WRITE_LARGE};
nos_check(nos_rpc(&invalid_read)==-NOS_EINVAL,"default_read_write_flag_rejected");
nos_vfs_regression();
unsigned random = 123456789u;
for (unsigned i=0;i<100000;i++) {
 struct nos_message m={0}; random=random*1664525u+1013904223u; m.op=1+(random%44);m.sender=1;m.process=1;m.arg=(intptr_t)(random%40)-4;m.value=(intptr_t)(random%40)-4;
 m.len=random%90;
 for(size_t j=0;j<NOS_MSG_DATA;j++){random=random*1664525u+1013904223u;m.data[j]=(char)(random>>24);}
 if(i%3==0){strcpy(m.data,"/tmp/fuzz");m.len=10;}
 defer_reply = 0; service(&m); pipe_complete();
}
printf("PASS vfs_stateful_host_fuzz_100000\n");return failed?1:0;
}
