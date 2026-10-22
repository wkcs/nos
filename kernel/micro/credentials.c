/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
#ifdef NOS_LINUX_COMPAT
/* The caller holds kernel_lock. References include zombies until task_reap;
 * the pool is independent of reusable task and address-space slots. */
struct credential_object {
    unsigned owner, references;
    struct nos_credentials value;
    uint64_t stack[2], nofile[2], address_space[2];
};
static struct credential_object credential_objects[NOS_PROCESSES];
static struct credential_object *process_object(const struct task *task)
{
    if (!task || !task->credential_ref || task->credential_ref > NOS_PROCESSES)
        kernel_fail("missing process credentials");
    struct credential_object *object = &credential_objects[task->credential_ref - 1];
    if (!object->references || object->owner != task->space_owner)
        kernel_fail("stale process credentials");
    return object;
}
const struct nos_credentials *credentials_get(const struct task *task)
{ return &process_object(task)->value; }
int credentials_create(struct task *task, const struct task *parent)
{
    if (task->credential_ref) kernel_fail("credential reference overwritten");
    const struct credential_object *source = parent ? process_object(parent) : NULL;
    for (unsigned i = 0; i < NOS_PROCESSES; i++) {
        struct credential_object *object = &credential_objects[i];
        if (object->references) continue;
        k_memset(object, 0, sizeof(*object));
        if (source) {
            object->value = source->value;
            k_memcpy(object->stack, source->stack, sizeof(object->stack));
            k_memcpy(object->nofile, source->nofile, sizeof(object->nofile));
            k_memcpy(object->address_space, source->address_space, sizeof(object->address_space));
        } else {
            object->stack[0] = object->stack[1] = 256u * 1024u;
            object->nofile[0] = object->nofile[1] = NOS_FD_LIMIT;
            object->address_space[0] = object->address_space[1] = UINT64_C(0x40000000);
        }
        object->owner = task->space_owner; object->references = 1;
        task->credential_ref = i + 1; return 0;
    }
    return -NOS_EAGAIN;
}
int credentials_share(struct task *task, const struct task *parent)
{
    if (task->credential_ref) kernel_fail("credential reference overwritten");
    (void)credentials_get(parent);
    if (task->space_owner != parent->space_owner) return -NOS_EINVAL;
    struct credential_object *object = &credential_objects[parent->credential_ref - 1];
    if (object->references >= NOS_TASKS) kernel_fail("credential reference overflow");
    object->references++; task->credential_ref = parent->credential_ref; return 0;
}
void credentials_release(struct task *task)
{
    if (!task->credential_ref) return;
    (void)credentials_get(task);
    struct credential_object *object = &credential_objects[task->credential_ref - 1];
    task->credential_ref = 0;
    if (!--object->references) k_memset(object, 0, sizeof(*object));
    k_memset(&task->request_subject, 0, sizeof(task->request_subject));
}
void credentials_exec(struct task *task)
{
    (void)credentials_get(task);
    struct nos_credentials *value = &credential_objects[task->credential_ref - 1].value;
    /* Ordinary exec also replaces saved IDs with the effective IDs. Call
     * only after the image commits, preserving all IDs on an exec failure.
     * The surviving process keeps its existing object and references. */
    value->suid = value->euid; value->sgid = value->egid;
}
void process_nofile_limits(const struct task *task, uint64_t limits[2])
{ k_memcpy(limits, process_object(task)->nofile, sizeof(uint64_t) * 2); }
void process_as_limits(const struct task *task, uint64_t limits[2])
{ k_memcpy(limits, process_object(task)->address_space, sizeof(uint64_t) * 2); }
intptr_t process_limit_operation(unsigned resource, uintptr_t new_limit, uintptr_t old_limit, unsigned wide)
{
    /* All three Linux target ABIs define RLIM_NLIMITS as 16. Known but
     * unimplemented resources differ from an invalid resource number. */
    if (resource >= 16u) return -NOS_EINVAL;
    struct credential_object *object = process_object(current_task);
    uint64_t *limits;
    if (resource == 3) limits = object->stack;
    else if (resource == 7) limits = object->nofile;
    else if (resource == 9) limits = object->address_space;
    else return -NOS_ENOTSUP;
    uint64_t old[2] = { limits[0], limits[1] }, replacement[2];
    if (new_limit) {
        if (wide) {
            if (user_copy_in(replacement, new_limit, sizeof(replacement))) return -NOS_EFAULT;
        } else {
            uint32_t narrow[2];
            if (user_copy_in(narrow, new_limit, sizeof(narrow))) return -NOS_EFAULT;
            for (unsigned i = 0; i < 2; i++) replacement[i] = narrow[i] == UINT32_MAX ? UINT64_MAX : narrow[i];
        }
        if (replacement[0] > replacement[1]) return -NOS_EINVAL;
        if (resource == 7) {
            /* UID privilege cannot create descriptor numbers beyond the
             * service's real structural capacity. No infinity is accepted. */
            if (replacement[1] > NOS_FD_LIMIT ||
                (replacement[1] > old[1] && object->value.euid)) return -NOS_EPERM;
        } else if (resource == 9) {
            /* An AS policy limit does not promise physical backing or alter
             * the architecture's valid user range. Infinity disables only
             * this admission bound, retaining every structural VM check. */
            if (replacement[1] > old[1] && object->value.euid) return -NOS_EPERM;
        } else if (replacement[0] != old[0] || replacement[1] != old[1]) {
            /* Main-stack admission and growth remain unsupported. */
            return -NOS_ENOTSUP;
        }
    }
    size_t bytes = wide ? sizeof(old) : sizeof(uint32_t) * 2;
    if (old_limit && !user_access(current_task, old_limit, bytes, 1)) return -NOS_EFAULT;
    if (old_limit) {
        if (wide) {
            if (user_copy_out(old_limit, old, sizeof(old))) return -NOS_EFAULT;
        } else {
            uint32_t narrow[2];
            for (unsigned i = 0; i < 2; i++) narrow[i] = old[i] > UINT32_MAX ? UINT32_MAX : (uint32_t)old[i];
            if (user_copy_out(old_limit, narrow, sizeof(narrow))) return -NOS_EFAULT;
        }
    }
    /* Both the complete candidate and the old-value output precede commit,
     * so new/old aliasing returns the old pair without losing the candidate. */
    if (new_limit) {
        k_memcpy(limits, replacement, sizeof(replacement));
        if (resource == 9) vm_as_limit_set(current_task, replacement[0]);
    }
    return 0;
}
static int is_id(uint32_t id, uint32_t real, uint32_t effective, uint32_t saved)
{ return id == real || id == effective || id == saved; }
static int change_credentials(struct task *task, unsigned command, const struct nos_credential_request *request)
{
    struct nos_credentials next = *credentials_get(task);
    unsigned privileged = next.euid == 0;
    uint32_t real, effective, saved, r = request->ids[0], e = request->ids[1], s = request->ids[2];
    if (command == NOS_CRED_SETGROUPS) {
        if (!privileged) return -NOS_EPERM;
        if (request->count > NOS_NGROUPS_MAX) return -NOS_EINVAL;
        for (unsigned i = 0; i < request->count; i++) if (request->groups[i] == UINT32_MAX) return -NOS_EINVAL;
        next.ngroups = request->count;
        k_memset(next.groups, 0, sizeof(next.groups));
        k_memcpy(next.groups, request->groups, request->count * sizeof(uint32_t));
    } else {
        unsigned gid = command >= NOS_CRED_SETGID && command <= NOS_CRED_SETRESGID;
        unsigned operation = gid ? command - NOS_CRED_SETGID + NOS_CRED_SETUID : command;
        if (operation < NOS_CRED_SETUID || operation > NOS_CRED_SETRESUID) return -NOS_EINVAL;
        real = gid ? next.rgid : next.ruid;
        effective = gid ? next.egid : next.euid;
        saved = gid ? next.sgid : next.suid;
        if (operation == NOS_CRED_SETUID) {
            if (r == UINT32_MAX) return -NOS_EINVAL;
            if (privileged) real = effective = saved = r;
            else {
                if (r != real && r != saved) return -NOS_EPERM;
                effective = r;
            }
        } else if (operation == NOS_CRED_SETEUID) {
            if (r == UINT32_MAX) return -NOS_EINVAL;
            if (!privileged && !is_id(r, real, effective, saved)) return -NOS_EPERM;
            effective = r;
        } else if (operation == NOS_CRED_SETREUID) {
            if (!privileged && ((r != UINT32_MAX && r != real && r != effective) ||
                (e != UINT32_MAX && !is_id(e, real, effective, saved)))) return -NOS_EPERM;
            /* Saved ID compares the requested effective ID with the OLD real
             * ID, before assigning either member. This also makes NPTL's
             * repeated process-wide SETXID transition a legal no-op. */
            if (r != UINT32_MAX || (e != UINT32_MAX && e != real)) saved = e == UINT32_MAX ? effective : e;
            if (r != UINT32_MAX) real = r;
            if (e != UINT32_MAX) effective = e;
        } else {
            if (!privileged && ((r != UINT32_MAX && !is_id(r, real, effective, saved)) ||
                (e != UINT32_MAX && !is_id(e, real, effective, saved)) ||
                (s != UINT32_MAX && !is_id(s, real, effective, saved)))) return -NOS_EPERM;
            if (r != UINT32_MAX) real = r;
            if (e != UINT32_MAX) effective = e;
            if (s != UINT32_MAX) saved = s;
        }
        if (gid) { next.rgid = real; next.egid = effective; next.sgid = saved; }
        else { next.ruid = real; next.euid = effective; next.suid = saved; }
    }
    credential_objects[task->credential_ref - 1].value = next;
    return 0;
}
intptr_t credentials_operation(uintptr_t command, uintptr_t pointer, uintptr_t flags)
{
    if (flags || command > NOS_CRED_SETGROUPS) return -NOS_EINVAL;
    if (command == NOS_CRED_GET) return user_copy_out(pointer, credentials_get(current_task), sizeof(struct nos_credentials));
    struct nos_credential_request request;
    if (user_copy_in(&request, pointer, sizeof(request))) return -NOS_EFAULT;
    return change_credentials(current_task, (unsigned)command, &request);
}
static intptr_t get_res_ids(const uintptr_t a[6], int gid)
{
    for (unsigned i = 0; i < 3; i++) if (!user_access(current_task, a[i], sizeof(uint32_t), 1)) return -NOS_EFAULT;
    const struct nos_credentials *value = credentials_get(current_task);
    uint32_t ids[3] = { gid ? value->rgid : value->ruid, gid ? value->egid : value->euid, gid ? value->sgid : value->suid };
    for (unsigned i = 0; i < 3; i++) if (user_copy_out(a[i], &ids[i], sizeof(ids[i]))) return -NOS_EFAULT;
    return 0;
}
#ifdef NOS_ARM
#define CNR(arm, generic) (arm)
#else
#define CNR(arm, generic) (generic)
#endif
static intptr_t scalar_id_result(uint32_t id)
{
#if defined(NOS_RISCV) && !defined(NOS_RISCV32)
    /* RV64 psABI canonicalizes even unsigned 32-bit C returns by sign
     * extension. Stock glibc 2.41's four no-errno ID wrappers return a0
     * directly, so this NOS personality supplies that canonical value.
     * Linux raw syscall results can instead be zero-extended: generic
     * syscall() errno decoding is ambiguous for these high IDs here.
     * GETRES, native GET and stored credentials remain unsigned 32-bit. */
    return (intptr_t)(int32_t)id;
#else
    return id;
#endif
}
intptr_t linux_credentials_syscall(uintptr_t number, const uintptr_t a[6], unsigned *handled)
{
    /* ARM UID 0x80000000 is a valid unsigned value and has the same bits as
     * INTPTR_MIN. Do not overload an in-band return value as dispatch state. */
    *handled = 1;
    unsigned command;
    const struct nos_credentials *value = credentials_get(current_task);
    switch (number) {
    case CNR(199, 174): return scalar_id_result(value->ruid);
    case CNR(201, 175): return scalar_id_result(value->euid);
    case CNR(200, 176): return scalar_id_result(value->rgid);
    case CNR(202, 177): return scalar_id_result(value->egid);
    case CNR(209, 148): return get_res_ids(a, 0);
    case CNR(211, 150): return get_res_ids(a, 1);
    case CNR(205, 158): {
        int32_t count = (int32_t)a[0];
        if (count < 0) return -NOS_EINVAL;
        if (!count) return value->ngroups;
        if ((uint32_t)count < value->ngroups) return -NOS_EINVAL;
        if (value->ngroups && user_copy_out(a[1], value->groups, value->ngroups * sizeof(uint32_t))) return -NOS_EFAULT;
        return value->ngroups;
    }
    case CNR(213, 146): command = NOS_CRED_SETUID; break;
    case CNR(214, 144): command = NOS_CRED_SETGID; break;
    case CNR(203, 145): command = NOS_CRED_SETREUID; break;
    case CNR(204, 143): command = NOS_CRED_SETREGID; break;
    case CNR(208, 147): command = NOS_CRED_SETRESUID; break;
    case CNR(210, 149): command = NOS_CRED_SETRESGID; break;
    case CNR(206, 159): {
        if (a[0] > NOS_NGROUPS_MAX) return -NOS_EINVAL;
        struct nos_credential_request request; k_memset(&request, 0, sizeof(request));
        request.count = (uint32_t)a[0];
        if (request.count && user_copy_in(request.groups, a[1], request.count * sizeof(uint32_t))) return -NOS_EFAULT;
        return change_credentials(current_task, NOS_CRED_SETGROUPS, &request);
    }
    /* Linux per-thread fs IDs are not part of this POSIX process-ID profile. */
    case CNR(215, 151): case CNR(216, 152): return -NOS_ENOSYS;
#ifdef NOS_ARM
    /* Old 16-bit ARM UID/GID ABIs are deliberately unsupported. */
    case 23: case 24: case 46: case 47: case 49: case 50:
    case 70: case 71: case 80: case 81: case 138: case 139:
    case 164: case 165: case 170: case 171: return -NOS_ENOSYS;
#endif
    default: *handled = 0; return 0;
    }
    struct nos_credential_request request; k_memset(&request, 0, sizeof(request));
    for (unsigned i = 0; i < 3; i++) request.ids[i] = (uint32_t)a[i];
    return change_credentials(current_task, command, &request);
}
int credentials_signal_allowed(const struct task *sender, const struct task *target, unsigned signal)
{
    const struct nos_credentials *from = credentials_get(sender), *to = credentials_get(target);
    return !from->euid || from->ruid == to->ruid || from->ruid == to->suid ||
        from->euid == to->ruid || from->euid == to->suid ||
        (signal == 18 && linux_process_session(sender) == linux_process_session(target));
}
#else
int credentials_create(struct task *task, const struct task *parent) { (void)task; (void)parent; return 0; }
int credentials_share(struct task *task, const struct task *parent) { (void)task; (void)parent; return 0; }
void credentials_release(struct task *task) { (void)task; }
void credentials_exec(struct task *task) { (void)task; }
void process_nofile_limits(const struct task *task, uint64_t limits[2])
{ (void)task; (void)limits; }
void process_as_limits(const struct task *task, uint64_t limits[2])
{ (void)task; limits[0] = limits[1] = UINT64_MAX; }
intptr_t process_limit_operation(unsigned resource, uintptr_t new_limit, uintptr_t old_limit, unsigned wide)
{ (void)resource; (void)new_limit; (void)old_limit; (void)wide; return -NOS_ENOTSUP; }
const struct nos_credentials *credentials_get(const struct task *task) { (void)task; return NULL; }
intptr_t credentials_operation(uintptr_t command, uintptr_t pointer, uintptr_t flags)
{ (void)command; (void)pointer; (void)flags; return -NOS_ENOTSUP; }
intptr_t linux_credentials_syscall(uintptr_t number, const uintptr_t arguments[6], unsigned *handled)
{ (void)number; (void)arguments; *handled = 0; return 0; }
int credentials_signal_allowed(const struct task *sender, const struct task *target, unsigned signal)
{ (void)sender; (void)target; (void)signal; return 0; }
#endif
