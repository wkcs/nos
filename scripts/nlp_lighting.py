#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Strict JSON compiler for declarative NK60 lighting."""
import argparse
import json
from pathlib import Path
import struct
import sys

SIZE = 400
MAX_RULES = 12
TRIGGERS = [
    "always",
    "press",
    "release",
    "tap",
    "double_tap",
    "held",
    "hold_decided",
    "layer_enter",
    "layer_leave",
    "layer_active",
    "host_led_on",
    "host_led_off",
    "host_led_active",
    "caps_word_on",
    "caps_word_off",
    "caps_word_active",
    "macro_start",
    "macro_end",
    "macro_active",
    "connect",
    "disconnect",
    "idle",
    "wake",
    "modifiers_on",
    "modifiers_off",
    "modifiers_active",
]
MATCHES = ["any", "key", "usage", "action", "layer", "host_led", "macro", "consumer", "modifiers"]
EFFECTS = ["solid", "fade", "pulse", "breathe", "rainbow", "wave", "ripple"]
BLENDS = ["replace", "add", "max", "multiply"]
ACTIONS = [
    "none",
    "transparent",
    "key",
    "modifier",
    "momentary",
    "toggle",
    "mod_tap",
    "layer_tap",
    "oneshot",
    "consumer",
    "macro",
    "local",
    "grave_escape",
]
STATE = {
    "always",
    "held",
    "layer_active",
    "host_led_active",
    "caps_word_active",
    "macro_active",
    "modifiers_active",
}
KEY_EVENTS = {
    "press",
    "release",
    "tap",
    "double_tap",
    "held",
    "hold_decided",
    "macro_start",
    "macro_end",
    "macro_active",
}
MODS = {
    "LCTRL": 1,
    "LSHIFT": 2,
    "LALT": 4,
    "LGUI": 8,
    "RCTRL": 16,
    "RSHIFT": 32,
    "RALT": 64,
    "RGUI": 128,
    "CTRL": 17,
    "SHIFT": 34,
    "ALT": 68,
    "GUI": 136,
}
USAGES = {chr(65 + i): 4 + i for i in range(26)}
USAGES.update({str((i + 1) % 10): 30 + i for i in range(10)})
USAGES.update({f"F{i+1}": 58 + i for i in range(12)})
USAGES.update(
    dict(
        ENTER=40,
        ESCAPE=41,
        BACKSPACE=42,
        TAB=43,
        SPACE=44,
        MINUS=45,
        EQUAL=46,
        LBRACKET=47,
        RBRACKET=48,
        BACKSLASH=49,
        SEMICOLON=51,
        QUOTE=52,
        GRAVE=53,
        COMMA=54,
        DOT=55,
        SLASH=56,
        CAPS=57,
        DELETE=76,
        RIGHT=79,
        LEFT=80,
        DOWN=81,
        UP=82,
    )
)
USAGES.update(
    {
        name: 224 + i
        for i, name in enumerate(
            ["LCTRL", "LSHIFT", "LALT", "LGUI", "RCTRL", "RSHIFT", "RALT", "RGUI"]
        )
    }
)
ROWS = [
    list(range(0, 14)),
    list(range(14, 28)),
    list(range(28, 41)),
    list(range(41, 53)),
    list(range(53, 61)),
]


def integer(value, low, high, label):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f"{label} must be an integer in {low}..{high}")
    return value


def boolean(value, label):
    if type(value) is not bool:
        raise ValueError(f"{label} must be boolean")
    return value


def fields(value, allowed, label):
    if not isinstance(value, dict) or set(value) - set(allowed):
        raise ValueError(f"unknown fields or invalid {label}")
    return value


def option(value, choices, label):
    if value not in choices:
        raise ValueError(f"unsupported {label}: {value}")
    return choices.index(value)


def modifiers(value):
    if type(value) is int:
        return integer(value, 0, 255, "modifier mask")
    if not isinstance(value, list) or len(value) > 8:
        raise ValueError("modifiers must be a bounded list or mask")
    mask = 0
    for item in value:
        if not isinstance(item, str) or item not in MODS:
            raise ValueError(f"unknown modifier: {item}")
        mask |= MODS[item]
    return mask


def color(value, palette):
    if isinstance(value, str):
        if value.startswith("@"):
            if value[1:] not in palette:
                raise ValueError("unknown palette entry")
            return color(palette[value[1:]], {})
        if not isinstance(value, str) or len(value) != 7 or value[0] != "#":
            raise ValueError("color must be #RRGGBB")
        if any(ch not in "0123456789abcdefABCDEF" for ch in value[1:]):
            raise ValueError("invalid hex color")
        try:
            return list(bytes.fromhex(value[1:]))
        except ValueError:
            raise ValueError("invalid hex color") from None
    if not isinstance(value, list) or len(value) != 3:
        raise ValueError("color must contain three RGB bytes")
    return [integer(v, 0, 255, "color") for v in value]


def keys(value):
    if not isinstance(value, list) or not 1 <= len(value) <= 61:
        raise ValueError("key list must contain 1..61 IDs")
    result = [integer(v, 0, 60, "key") for v in value]
    if len(set(result)) != len(result):
        raise ValueError("duplicate key in list")
    return result


def zone(value, zones):
    if not isinstance(value, str) or value not in zones:
        raise ValueError(f"unknown zone: {value}")
    return keys(zones[value])


def target(value, zones):
    if value == "source":
        return 0, 0
    if value == "all":
        return 2, 0
    fields(value, ["keys", "rows", "zone"], "target")
    if len(value) != 1:
        raise ValueError("target requires one selector")
    if "keys" in value:
        selected = keys(value["keys"])
    elif "zone" in value:
        selected = zone(value["zone"], zones)
    else:
        rows = value["rows"]
        if not isinstance(rows, list) or not 1 <= len(rows) <= 5:
            raise ValueError("rows must be a bounded list")
        selected = [key for r in rows for key in ROWS[integer(r, 0, 4, "row")]]
    return 1, sum(1 << key for key in set(selected))


def match_selectors(value, zones):
    fields(value, [*MATCHES, "parameter", "keys", "usages", "zone"], "match")
    if "keys" in value or "zone" in value:
        if len(value) != 1:
            raise ValueError("group match cannot combine selectors; use conditions")
        selected = keys(value["keys"]) if "keys" in value else zone(value["zone"], zones)
        return [(1, key) for key in selected]
    if "usages" in value:
        if (
            len(value) != 1
            or not isinstance(value["usages"], list)
            or not 1 <= len(value["usages"]) <= 12
        ):
            raise ValueError("usages requires a bounded list")
        return [match_selectors({"usage": usage}, zones)[0] for usage in value["usages"]]
    names = [name for name in MATCHES if name in value]
    if len(names) != 1 or ("parameter" in value and names[0] != "action"):
        raise ValueError("match requires one selector")
    name = names[0]
    v = value[name]
    if name == "any":
        if v is not True:
            raise ValueError("any must be true")
        v = 0
    elif name == "usage":
        v = USAGES.get(v, v) if isinstance(v, str) else v
        v = integer(v, 4, 231, "usage")
    elif name == "action":
        v = option(v, ACTIONS, "action") | (
            integer(value.get("parameter", 255), 0, 255, "parameter") << 8
        )
    elif name == "modifiers":
        v = modifiers(v)
        if not v:
            raise ValueError("modifier match must not be empty")
    else:
        v = integer(
            v,
            0 if name != "consumer" else 1,
            {"key": 60, "layer": 3, "host_led": 4, "macro": 3, "consumer": 1023}[name],
            name,
        )
    return [(MATCHES.index(name), v)]


def compile_program(document):
    fields(
        document,
        [
            "version",
            "enabled",
            "dark_base",
            "frame_ms",
            "idle_event_ms",
            "rules",
            "palette",
            "zones",
            "description",
        ],
        "program",
    )
    if type(document.get("version", 1)) is not int or document.get("version", 1) != 1:
        raise ValueError("unsupported program version")
    flags = (
        int(boolean(document.get("enabled", True), "enabled"))
        | int(boolean(document.get("dark_base", False), "dark_base")) << 1
    )
    frame = integer(document.get("frame_ms", 40), 40, 1000, "frame_ms")
    idle = integer(document.get("idle_event_ms", 0), 0, 60000, "idle_event_ms")
    palette = document.get("palette", {})
    zones = document.get("zones", {})
    if (
        not isinstance(palette, dict)
        or len(palette) > 32
        or not isinstance(zones, dict)
        or len(zones) > 16
    ):
        raise ValueError("palette/zones exceed their limits")
    for name, value in palette.items():
        if not isinstance(name, str) or not 1 <= len(name) <= 32:
            raise ValueError("invalid palette name")
        color(value, {})
    for name, values in zones.items():
        if not isinstance(name, str) or not 1 <= len(name) <= 32:
            raise ValueError("invalid zone name")
        keys(values)
    rules = document.get("rules", [])
    if not isinstance(rules, list) or len(rules) > MAX_RULES:
        raise ValueError("at most12 rules before expansion")
    compiled = []
    for rule in rules:
        fields(
            rule,
            [
                "name",
                "when",
                "match",
                "target",
                "effect",
                "blend",
                "priority",
                "color",
                "color_to",
                "duration_ms",
                "period_ms",
                "threshold_ms",
                "opacity",
                "single",
                "reverse",
                "axis",
                "conditions",
            ],
            "rule",
        )
        when = rule.get("when", "press")
        trigger = option(when, TRIGGERS, "trigger")
        destination, mask = target(
            rule.get("target", "source" if when in KEY_EVENTS else "all"), zones
        )
        effect = option(rule.get("effect", "fade"), EFFECTS, "effect")
        if (destination == 0 or effect == 6) and when not in KEY_EVENTS:
            raise ValueError("source/ripple needs a key source")
        duration = integer(
            rule.get("duration_ms", 0 if when in STATE else 500),
            0 if when in STATE else 1,
            60000,
            "duration_ms",
        )
        period = integer(rule.get("period_ms", 500), 20, 60000, "period_ms")
        threshold = integer(
            rule.get(
                "threshold_ms",
                (
                    300
                    if when == "held"
                    else 250 if when == "double_tap" else 200 if when == "tap" else 0
                ),
            ),
            20 if when in ("held", "double_tap", "tap") else 0,
            60000 if when == "held" else 2000 if when in ("double_tap", "tap") else 0,
            "threshold_ms",
        )
        a = color(rule.get("color", "#ffffff"), palette)
        b = color(rule.get("color_to", rule.get("color", "#ffffff")), palette)
        blend = option(rule.get("blend", "replace"), BLENDS, "blend")
        priority = integer(rule.get("priority", 0), 0, 255, "priority")
        opacity = integer(rule.get("opacity", 255), 0, 255, "opacity")
        single = boolean(rule.get("single", False), "single")
        reverse = boolean(rule.get("reverse", False), "reverse")
        if reverse and effect == 0:
            raise ValueError("reverse has no meaning for solid color")
        axis = rule.get("axis", "x")
        if axis not in ("x", "y") or (axis == "y" and effect != 5):
            raise ValueError("vertical axis is wave only")
        if single and when in STATE:
            raise ValueError("single is an edge-trigger option")
        conditions = fields(
            rule.get("conditions", {}),
            ["layers", "not_layers", "modifiers", "modifiers_any", "not_modifiers"],
            "conditions",
        )
        layers = 0
        if "layers" in conditions:
            if not isinstance(conditions["layers"], list) or len(conditions["layers"]) > 4:
                raise ValueError("invalid layer conditions")
            for layer in conditions["layers"]:
                layers |= 1 << integer(layer, 0, 3, "layer")
        excluded_layers = 0
        if "not_layers" in conditions:
            if not isinstance(conditions["not_layers"], list) or len(conditions["not_layers"]) > 4:
                raise ValueError("invalid excluded layers")
            for layer in conditions["not_layers"]:
                excluded_layers |= 1 << integer(layer, 0, 3, "excluded layer")
        if layers & excluded_layers:
            raise ValueError("a layer cannot be both required and excluded")
        mod_kinds = [
            name for name in ("modifiers", "modifiers_any", "not_modifiers") if name in conditions
        ]
        if len(mod_kinds) > 1:
            raise ValueError("choose one modifier condition mode")
        mod_kind = mod_kinds[0] if mod_kinds else "modifiers"
        required_mods = modifiers(conditions.get(mod_kind, 0))
        if mod_kind != "modifiers" and not required_mods:
            raise ValueError("empty modifier condition")
        if (
            mod_kind == "modifiers"
            and isinstance(conditions.get("modifiers"), list)
            and any(name in ("SHIFT", "CTRL", "GUI", "ALT") for name in conditions["modifiers"])
        ):
            raise ValueError(
                "use modifiers_any for generic SHIFT/CTRL/ALT/GUI, or specify side-specific modifiers"
            )
        condition_flags = (
            8 if mod_kind == "modifiers_any" else 16 if mod_kind == "not_modifiers" else 0
        )
        for kind, value in match_selectors(rule.get("match", {"any": True}), zones):
            if kind in (1, 2, 3, 7) and when not in KEY_EVENTS:
                raise ValueError("key/action match requires a key source")
            entry = bytearray(32)
            struct.pack_into(
                "<BBH4B", entry, 0, trigger, kind, value, destination, effect, blend, priority
            )
            entry[8:11] = bytes(a)
            entry[11:14] = bytes(b)
            struct.pack_into("<3HQ", entry, 14, duration, period, threshold, mask)
            entry[28:32] = bytes(
                [
                    int(single) | int(reverse) << 1 | int(axis == "y") << 2 | condition_flags,
                    opacity,
                    layers | excluded_layers << 4,
                    required_mods,
                ]
            )
            compiled.append(entry)
    if len(compiled) > MAX_RULES:
        raise ValueError("expanded program exceeds12 MCU rules")
    compiled.sort(key=lambda r: r[7])
    output = bytearray(SIZE)
    output[:4] = b"KLFX"
    output[4] = 1
    output[5] = len(compiled)
    struct.pack_into("<3H", output, 6, flags, frame, idle)
    for i, entry in enumerate(compiled):
        output[16 + i * 32 : 48 + i * 32] = entry
    return bytes(output)


def decode_program(data):
    if len(data) != SIZE or data[:4] != b"KLFX" or data[4] != 1 or data[5] > MAX_RULES:
        raise ValueError("invalid lighting program header")
    flags, frame, idle = struct.unpack_from("<3H", data, 6)
    if flags & ~3 or any(data[12:16]):
        raise ValueError("unknown program flags or reserved bytes")
    result = dict(
        version=1,
        enabled=bool(flags & 1),
        dark_base=bool(flags & 2),
        frame_ms=frame,
        idle_event_ms=idle,
        rules=[],
    )
    for i in range(data[5]):
        r = data[16 + i * 32 : 48 + i * 32]
        t, m, value, destination, effect, blend, priority = struct.unpack_from("<BBH4B", r)
        if (
            t >= len(TRIGGERS)
            or m >= len(MATCHES)
            or destination > 2
            or effect >= len(EFFECTS)
            or blend >= len(BLENDS)
            or r[28] & ~31
        ):
            raise ValueError("unknown rule enum")
        duration, period, threshold, mask = struct.unpack_from("<3HQ", r, 14)
        if mask >> 61 or (destination != 1 and mask):
            raise ValueError("invalid target mask")
        selector = {MATCHES[m]: True if m == 0 else value}
        if m == 3:
            if (value & 255) >= len(ACTIONS):
                raise ValueError("unknown action")
            selector = {"action": ACTIONS[value & 255], "parameter": value >> 8}
        rule = dict(
            when=TRIGGERS[t],
            match=selector,
            target=(
                "source"
                if destination == 0
                else (
                    "all"
                    if destination == 2
                    else {"keys": [key for key in range(61) if mask & (1 << key)]}
                )
            ),
            effect=EFFECTS[effect],
            blend=BLENDS[blend],
            priority=priority,
            color="#" + r[8:11].hex(),
            color_to="#" + r[11:14].hex(),
            duration_ms=duration,
            period_ms=period,
            threshold_ms=threshold,
            opacity=r[29],
            single=bool(r[28] & 1),
            reverse=bool(r[28] & 2),
            axis="y" if r[28] & 4 else "x",
            conditions=dict(
                layers=[i for i in range(4) if r[30] & (1 << i)],
                not_layers=[i for i in range(4) if (r[30] >> 4) & (1 << i)],
                **{
                    (
                        "modifiers_any"
                        if r[28] & 8
                        else "not_modifiers" if r[28] & 16 else "modifiers"
                    ): r[31]
                },
            ),
        )
        result["rules"].append(rule)
    if compile_program(result) != data:
        raise ValueError("noncanonical or invalid program")
    return result


def load(path):
    if path.stat().st_size > 65536:
        raise ValueError("lighting program JSON exceeds64KiB")
    return json.loads(path.read_text())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    raw = compile_program(load(args.file))
    normalized = decode_program(raw)
    if args.output:
        with args.output.open("xb") as output:
            output.write(raw)
    print(
        json.dumps(
            dict(bytes=len(raw), expanded_rules=len(normalized["rules"]), program=normalized),
            indent=2,
        )
    )


def client_command(client, session, args):
    info = client.call(session, 11)
    if (
        len(info) != 32
        or struct.unpack_from("<H", info)[0] != 1
        or struct.unpack_from("<H", info, 4)[0] != SIZE
    ):
        raise ValueError("unsupported lighting program capability")
    generation = struct.unpack_from("<I", info, 24)[0]
    if args.command == "fx-info":
        return dict(
            schema=1,
            max_rules=info[2],
            max_instances=info[3],
            wire_bytes=SIZE,
            min_frame_ms=struct.unpack_from("<H", info, 6)[0],
            max_frame_ms=struct.unpack_from("<H", info, 8)[0],
            effect_mask=struct.unpack_from("<H", info, 10)[0],
            trigger_mask=struct.unpack_from("<I", info, 12)[0],
            match_mask=struct.unpack_from("<H", info, 16)[0],
            blend_mask=info[18],
            active_instances=info[19],
            dropped_instances=struct.unpack_from("<I", info, 20)[0],
            generation=generation,
            rule_count=info[28],
            enabled=bool(info[29] & 1),
            dark_base=bool(info[29] & 2),
        )
    if args.command == "fx-get":
        output = bytearray()
        while len(output) < SIZE:
            count = min(104, SIZE - len(output))
            page = client.call(session, 12, struct.pack("<HHI", len(output), count, generation))
            if len(page) != 8 + count:
                raise ValueError("short lighting program page")
            gen, offset, length = struct.unpack_from("<IHH", page)
            if (gen, offset, length) != (generation, len(output), count):
                raise ValueError("lighting program changed during read")
            output.extend(page[8:])
        program = decode_program(bytes(output))
        if args.file is not None:
            with args.file.open("x") as target_file:
                json.dump(program, target_file, indent=2)
                target_file.write("\n")
        return dict(generation=generation, program=program)
    if args.command == "fx-clear":
        raw = compile_program(dict(enabled=False, rules=[]))
    else:
        if args.file is None:
            raise ValueError("fx-put requires --file")
        raw = compile_program(load(args.file))
    if raw[5] > info[2]:
        raise ValueError("program exceeds device rule count")
    result = client.call(session, 13, struct.pack("<I", generation))
    if len(result) != 8:
        raise ValueError("invalid program transaction response")
    tx = struct.unpack("<Q", result)[0]
    try:
        for offset in range(0, SIZE, 100):
            block = raw[offset : offset + 100]
            client.call(session, 14, struct.pack("<QHH", tx, offset, len(block)) + block)
        response = client.call(session, 15, struct.pack("<QIB3x", tx, generation, int(args.save)))
        if len(response) != 8 or response[4] > 2:
            raise ValueError("invalid program commit response")
        return dict(
            generation=struct.unpack_from("<I", response)[0],
            rules=raw[5],
            persistence=["APPLIED", "SAVE_PENDING", "PERSISTED"][response[4]],
        )
    except Exception:
        try:
            client.call(session, 16, struct.pack("<Q", tx))
        except (OSError, ValueError):
            pass
        raise


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print(f"nlp-lighting: {error}", file=sys.stderr)
        sys.exit(1)
