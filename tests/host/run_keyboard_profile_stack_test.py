#!/usr/bin/env python3
"""Measure production NK60 object-store and unified RPC stack paths."""
from collections import defaultdict
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
import sys

BOARD = sys.argv[1] if len(sys.argv) > 1 else "nk60-v2"
BUILD = ROOT / "build" / BOARD
SOURCES = (
    "app/server/keyboard/keyboard_server.c",
    "app/server/keyboard/keyboard_config_service.c",
    "app/server/keyboard/keyboard_config_model.c",
    "app/server/keyboard/keyboard_config_rpc.c",
    "app/server/keyboard/keyboard_object_store.c",
    "app/server/keyboard/keyboard_document.c",
    "fs/vfs.c",
    "fs/vfs_io.c",
    "fs/vfs_fd.c",
    "fs/littlefs/littlefs_vfs.c",
    "libs/third_party/littlefs/lfs.c",
    "drivers/flash/w25q_flash.c",
    "drivers/flash/w25q_spi.c",
    "drivers/spi/spi_stm32.c",
    "kernel/ipc/mutex.c",
)
NLK_SOURCES = (
    "app/server/nlk/core/nlk_core.c",
    "app/server/nlk/core/nlk_router.c",
    "app/server/nlk/core/nlk_frame.c",
    "app/server/keyboard/keyboard_engine.c",
    "app/server/nlk/bearers/bearer_usb_winusb.c",
    "app/server/nlk/nlk_server.c",
)

# Cortex-M4: 32 hardware + 72 extended FP + 64 software FP + 36
# software core/FP flag + up to 4 alignment bytes (cpu_s.S).
CONTEXT_BYTES = 208
# Reserve for uncompiled scheduler, clock and small library callees. Fatal
# diagnostics are outside this successful-persistence budget.
CALLEE_MARGIN = 128

# Resolve operation tables and littlefs callbacks for the NK60 backend. GCC
# supplies direct calls, including whichever helpers it inlines or clones.
CALLBACKS = {
    "vfs_create_file": ("littlefs_create",),
    "vfs_make_dir": ("littlefs_mkdir",),
    "vfs_rename_entry": ("littlefs_rename",),
    "nos_rename_noreplace": ("littlefs_rename",),
    "vfs_remove_entry": ("littlefs_unlink",),
    "vfs_lookup": ("littlefs_lookup",),
    "nos_open": ("littlefs_file_open",),
    "nos_read": ("littlefs_read",),
    "nos_write": ("littlefs_write",),
    "nos_lseek": ("littlefs_llseek",),
    "nos_sync": ("littlefs_sync_fs",),
    "vfs_fd_destroy": ("littlefs_release",),
    "lfs_dir_traverse": ("lfs_dir_commit_commit", "lfs_dir_traverse_filter"),
    "lfs_dir_fetchmatch": ("lfs_dir_find_match", "lfs_fs_parent_match"),
    "lfs_bd_read": ("littlefs_bd_read",),
    "lfs_bd_flush": ("littlefs_bd_prog",),
    "lfs_bd_erase": ("littlefs_bd_erase",),
    "littlefs_bd_read": ("w25q_wl_block_read",),
    "littlefs_bd_prog": ("w25q_wl_block_write",),
    "littlefs_bd_erase": ("w25q_wl_block_erase",),
    "spi_transceive": ("stm32_spi_transceive",),
}


def base_name(name):
    return name.split(".", 1)[0]


def compile_commands():
    database = BUILD / "compile_commands.json"
    if database.exists():
        return {Path(item["file"]).resolve():
                (shlex.split(item["command"]), Path(item["directory"]))
                for item in json.loads(database.read_text())}
    output = subprocess.check_output(
        ["ninja", "-C", str(BUILD), "-t", "commands"], text=True)
    result = {}
    for line in output.splitlines():
        args = shlex.split(line)
        if "-c" in args:
            source = Path(args[args.index("-c") + 1])
            result[(BUILD / source).resolve()] = (args, BUILD)
    return result


def measure(directory):
    sizes = {}
    edges = defaultdict(set)
    tail_edges = set()
    call_edges = set()
    commands = compile_commands()
    for source in SOURCES + NLK_SOURCES:
        args, cwd = commands[(ROOT / source).resolve()]
        args = args.copy()
        output = directory / (Path(source).name + ".o")
        for option, value in (("-o", str(output)), ("-MF", str(output) + ".d"),
                              ("-MT", str(output))):
            if option in args:
                args[args.index(option) + 1] = value
        subprocess.run(args + ["-fstack-usage", "-fcallgraph-info=su"],
                       cwd=cwd, check=True)
        objdump = Path(args[0]).with_name(Path(args[0]).name.replace("gcc", "objdump"))
        assembly = subprocess.check_output([str(objdump), "-dr", str(output)], text=True)
        caller = None
        for line in assembly.splitlines():
            function = re.match(r"^[0-9a-fA-F]+ <([^>]+)>:$", line)
            if function:
                caller = function[1]
            relocation = re.search(r"R_ARM_THM_(JUMP24|CALL)\s+(\S+)\s*$", line)
            if caller is not None and relocation:
                target = relocation[2].removeprefix(".text.")
                # Only exact function symbols are usable. Section-relative
                # addends and unrecognised relocation kinds retain the frame.
                if "+" not in target and "-" not in target:
                    pair = (caller, target)
                    if relocation[1] == "JUMP24":
                        tail_edges.add(pair)
                    else:
                        call_edges.add(pair)
        for line in output.with_suffix(".ci").read_text().splitlines():
            node = re.search(r'node: \{ title: "([^"]+)".*?\\n(\d+) bytes', line)
            if node:
                name = node[1].rsplit(":", 1)[-1]
                sizes[name] = max(sizes.get(name, 0), int(node[2]))
            edge = re.search(r'edge: \{ sourcename: "([^"]+)" '
                             r'targetname: "([^"]+)"', line)
            if edge:
                edges[edge[1].rsplit(":", 1)[-1]].add(
                    edge[2].rsplit(":", 1)[-1])
    assert sizes, "GCC did not produce stack-usage callgraph information"
    for caller in list(sizes):
        for target in CALLBACKS.get(base_name(caller), ()):
            matches = [name for name in sizes if base_name(name) == target]
            assert matches, f"missing callback stack measurement: {target}"
            edges[caller].update(matches)
    # A helper can have both normal and sibling calls to the same callee.
    # Retain the caller's frame whenever any ordinary call exists.
    return sizes, edges, tail_edges - call_edges


def longest_path(start, sizes, edges, tail_edges=frozenset()):
    cache = {}

    def visit(name, active):
        assert name not in active, f"recursive stack path requires a bound: {name}"
        if name in cache:
            return cache[name]
        # Persisted configuration entries are regular files, so replacement
        # and unlink never recurse into a child dentry subtree.
        children = [child for child in sorted(edges[name]) if child in sizes
                    and not (base_name(name) == "vfs_free_dentry_Subtree"
                             and child == name)]
        candidates = [(sizes[name], [name])]
        for child in children:
            cost, path = visit(child, active | {name})
            if (name, child) not in tail_edges:
                cost += sizes[name]
            # The function's own peak remains relevant before a tail jump.
            candidates.append((max(sizes[name], cost), [name] + path))
        result = max(candidates, key=lambda item: item[0])
        cache[name] = result
        return result

    return visit(start, set())


def maintenance_route(edges):
    service = (ROOT / "app/server/keyboard/keyboard_config_service.c").read_text()
    engine = (ROOT / "app/server/keyboard/keyboard_engine.c").read_text()
    assert ".poll = kb_config_poll" in service
    assert ".on_poll = kb_on_poll" in engine
    route = ("nlk_server_task_entry", "nlk_node_poll_capabilities",
             "kb_on_poll", "kb_config_poll")
    indirect = {("nlk_node_poll_capabilities", "kb_on_poll"),
                ("kb_on_poll", "kb_config_poll")}
    for caller, child in zip(route, route[1:]):
        assert child in edges[caller] or (caller, child) in indirect, \
            f"configuration poll route changed: {caller} -> {child}"
    return route


def nlk_budget():
    source = (ROOT / "app/server/nlk/nlk_server.c").read_text()
    creation = re.search(r'task_create\(\s*"nlk_server",\s*nlk_server_task_entry,\s*'
                         r'&g_nlk_srv,\s*\d+[Uu]?,\s*([A-Z_0-9]+)\s*,', source)
    assert creation, "cannot find the NLK server's task_create stack argument"
    value = creation[1]
    if value.isdigit():
        return int(value)
    configured = (BUILD / "include/autocfg.h").read_text()
    definition = re.search(rf"^#define\s+{re.escape(value)}\s+(\d+)[Uu]?\s*$",
                           configured, re.MULTILINE)
    assert definition, f"cannot resolve configured NLK stack size: {value}"
    return int(definition[1])


def check_nlk(sizes, edges, tail_edges):
    # Fix the request direction: the router's reply path can loop back into
    # itself for ACK delivery, which cannot dispatch this request again.
    # The service callback includes RAM patch/read and lease operations.
    route = ("nlk_server_task_entry", "nlk_node_poll_bearers", "winusb_rx_poll",
             "nlk_winusb_bearer_poll_rx", "nlk_bearer_rx_packet",
             "nlk_node_input_packet", "nlk_dispatch_local", "kb_on_rpc")
    callbacks = {("nlk_node_poll_bearers", "winusb_rx_poll"),
                 ("nlk_dispatch_local", "kb_on_rpc")}
    for caller, child in zip(route, route[1:]):
        assert child in edges[caller] or (caller, child) in callbacks, \
            f"NLK request route changed: {caller} -> {child}"
    endpoint = "kb_config_rpc"
    assert ".rpc = kb_config_rpc" in (ROOT / SOURCES[1]).read_text(), \
        "unified keyboard capability must use the production configuration service"
    cost, path = longest_path(endpoint, sizes, edges, tail_edges)
    for caller, child in reversed(list(zip(route, route[1:] + (endpoint,)))):
        if (caller, child) not in tail_edges:
            cost += sizes[caller]
        cost = max(sizes[caller], cost)
    budget = nlk_budget()
    required = cost + CONTEXT_BYTES + CALLEE_MARGIN
    print("NLK configuration RPC path:", " -> ".join(route + tuple(path)))
    print(f"NLK configuration RPC stack: measured={cost}, context={CONTEXT_BYTES}, "
          f"callee_margin={CALLEE_MARGIN}, required={required}, configured={budget}")
    assert required <= budget, f"NLK configuration RPC requires {required} bytes, task has {budget}"


def main():
    budget = nlk_budget()
    with tempfile.TemporaryDirectory(prefix="nos-keyboard-stack-") as tmp:
        sizes, edges, tail_edges = measure(Path(tmp))
    for name in ("save_draft", "kb_config_load_object"):
        assert sizes[name] < 1024, f"{name} uses {sizes[name]} bytes of stack"
    # Measure complete routes, including mkdir/create, rename, compaction,
    # wear leveling and SPI locking, rather than checking isolated frames.
    route = maintenance_route(edges)
    cost, path = longest_path(route[-1], sizes, edges, tail_edges)
    for caller, child in reversed(list(zip(route, route[1:]))):
        if (caller, child) not in tail_edges:
            cost += sizes[caller]
        cost = max(sizes[caller], cost)
    path = list(route[:-1]) + path
    required = cost + CONTEXT_BYTES + CALLEE_MARGIN
    print("persistence path:", " -> ".join(
        f"{name}({sizes[name]})" for name in path))
    print(f"persistence stack: measured={cost}, context={CONTEXT_BYTES}, "
          f"callee_margin={CALLEE_MARGIN}, required={required}, configured={budget}")
    assert required <= budget, \
        f"persistence requires {required} bytes, NLK task has {budget}"
    check_nlk(sizes, edges, tail_edges)
    print("production keyboard persistence and RPC stack checks passed")


if __name__ == "__main__":
    main()
