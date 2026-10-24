#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Typed NK60 product and lighting operations."""
import json
import struct
import nlp_codec as wire

READ_COMMANDS = {"info", "layout", "storage", "health", "lighting", "leds-get"}
WRITE_COMMANDS = {"lighting-set", "lighting-policy", "leds-put"}
PRODUCT_COMMANDS = {"info", "layout", "storage", "health"}
MODES = ["off", "static", "layer", "breathe", "rainbow", "wave", "reactive", "custom"]


def exact(data, size):
    if len(data) != size:
        raise wire.ProtocolError("invalid keyboard capability response size")
    return data


def command(client, session, args):
    action = args.command
    if action == "info":
        data = exact(client.call(session, 1), 32)
        (
            schema,
            keys,
            leds,
            width,
            height,
            units,
            layers,
            scan,
            flash,
            active,
            reserved,
            generation,
        ) = struct.unpack("<5HBB5I", data)
        if schema != 1 or units != 4 or keys != 61 or leds != 61:
            raise wire.ProtocolError("unsupported keyboard layout schema")
        return dict(
            schema=schema,
            keys=keys,
            leds=leds,
            width_u=width / units,
            height_u=height / units,
            layers=layers,
            scan_target_us=scan,
            flash_bytes=flash,
            active_features=active,
            reserved_features=reserved,
            generation=generation,
        )
    if action == "layout":
        result, cursor = [], 0
        for _ in range(61):
            data = client.call(session, 2, struct.pack("<HHI", cursor, 9, 0))
            if len(data) < 8:
                raise wire.ProtocolError("short layout page")
            first, count, following, total = struct.unpack_from("<4H", data)
            if first != cursor or not count or total != 61 or len(data) != 8 + 12 * count:
                raise wire.ProtocolError("malformed layout page")
            for at in range(count):
                key, led, x, y, width, height = struct.unpack_from("<6H", data, 8 + 12 * at)
                if key != first + at or led >= total or x + width > 60 or y + height > 20:
                    raise wire.ProtocolError("invalid physical key position")
                result.append(
                    dict(key=key, led=led, x=x / 4, y=y / 4, width=width / 4, height=height / 4)
                )
            if not following:
                if len(result) != total:
                    raise wire.ProtocolError("incomplete layout")
                return result
            if following != cursor + count:
                raise wire.ProtocolError("nonprogressing layout")
            cursor = following
        raise wire.ProtocolError("layout page limit")
    if action == "storage":
        data = exact(client.call(session, 3), 56)
        capacity, sector, page = struct.unpack_from("<IHH", data)
        if data[8] > 3:
            raise wire.ProtocolError("invalid storage partition count")
        names = ["preserved_unallocated", "boot_journal", "configuration_journal"]
        parts = []
        for i in range(data[8]):
            kind, flags, offset, size = struct.unpack_from("<HHII", data, 12 + 12 * i)
            if kind > 2 or offset + size > capacity:
                raise wire.ProtocolError("invalid storage partition")
            parts.append(dict(kind=names[kind], flags=flags, offset=offset, bytes=size))
        return dict(
            capacity=capacity,
            sector=sector,
            page=page,
            partitions=parts,
            durable_generation=struct.unpack_from("<I", data, 48)[0],
            dirty=bool(data[52]),
            settings_schema_locked=bool(data[53]),
            saving=bool(data[54]),
        )
    if action == "health":
        data = exact(client.call(session, 4), 48)
        names = [
            "scan_overruns",
            "scan_errors",
            "report_overflows",
            "usb_errors",
            "save_errors",
            "rgb_errors",
            "bad_nlp_frames",
            "uart_dropped",
        ]
        result = dict(zip(names, struct.unpack_from("<8I", data, 8)))
        result.update(
            uptime_us=struct.unpack_from("<Q", data)[0],
            estimated_led_ma=struct.unpack_from("<H", data, 40)[0],
            configured=bool(data[42]),
            suspended=bool(data[43]),
            settings_schema_locked=bool(data[44]),
            unlocked=bool(data[45]),
            hard_rgb_channel_limit=data[46],
            flash_available=bool(data[47]),
        )
        return result
    state = exact(client.call(session, 3), 11)
    generation, mode, brightness, reserved, red, green, blue = struct.unpack("<IBBH3B", state)
    if reserved or mode >= len(MODES):
        raise wire.ProtocolError("unsupported lighting state")
    if action == "lighting":
        settings = exact(client.call(session, 4), 16)
        if struct.unpack_from("<I", settings)[0] != generation:
            raise wire.ProtocolError("lighting changed during snapshot")
        return dict(
            generation=generation,
            mode=MODES[mode],
            brightness=brightness,
            rgb=[red, green, blue],
            speed=settings[4],
            indicators=bool(settings[5] & 1),
            idle_seconds=struct.unpack_from("<H", settings, 6)[0],
            estimated_led_budget_ma=struct.unpack_from("<H", settings, 8)[0],
            persistence=settings[13],
        )
    if action == "leds-get":
        colors = []
        while len(colors) < 61:
            count = min(32, 61 - len(colors))
            data = client.call(session, 6, struct.pack("<HHI", len(colors), count, generation))
            if len(data) < 8:
                raise wire.ProtocolError("short LED page")
            gen, first, got = struct.unpack_from("<IHH", data)
            if (
                gen != generation
                or first != len(colors)
                or got != count
                or len(data) != 8 + count * 3
            ):
                raise wire.ProtocolError("malformed or changing LED page")
            colors.extend([list(data[8 + i * 3 : 11 + i * 3]) for i in range(count)])
        if args.file is not None:
            with args.file.open("x") as output:
                json.dump(colors, output, indent=2)
                output.write("\n")
        return dict(generation=generation, colors=colors)
    if action == "lighting-set":
        rgb = args.rgb if args.rgb is not None else [red, green, blue]
        target_mode = mode if args.mode is None else MODES.index(args.mode)
        level = brightness if args.brightness is None else args.brightness
        client.call(
            session,
            2,
            struct.pack("<IBBH3BB", generation, target_mode, level, 0, *rgb, int(args.save)),
        )
        return dict(
            applied=True,
            persistence="SAVE_PENDING" if args.save else "APPLIED",
            mode=MODES[target_mode],
        )
    if action == "lighting-policy":
        data = bytearray(exact(client.call(session, 4), 16))
        struct.pack_into("<I", data, 0, generation)
        if args.speed is not None:
            data[4] = args.speed
        if args.indicators is not None:
            data[5] = args.indicators
        if args.idle_seconds is not None:
            struct.pack_into("<H", data, 6, args.idle_seconds)
        if args.led_budget is not None:
            struct.pack_into("<H", data, 8, args.led_budget)
        data[13] = int(args.save)
        data[14:16] = b"\0\0"
        client.call(session, 5, bytes(data))
        return dict(applied=True, persistence="SAVE_PENDING" if args.save else "APPLIED")
    if action == "leds-put":
        if args.file is None:
            raise ValueError("leds-put requires --file")
        if args.file.stat().st_size > 16384:
            raise ValueError("LED file is too large")
        colors = json.loads(args.file.read_text())
        if (
            not isinstance(colors, list)
            or len(colors) != 61
            or any(
                not isinstance(c, list)
                or len(c) != 3
                or any(type(v) is not int or not 0 <= v <= 255 for v in c)
                for c in colors
            )
        ):
            raise ValueError("LED file must contain exactly 61 RGB byte triples")
        tx = struct.unpack("<Q", exact(client.call(session, 7, struct.pack("<I", generation)), 8))[
            0
        ]
        try:
            for first in range(0, 61, 32):
                block = colors[first : first + 32]
                client.call(
                    session,
                    8,
                    struct.pack("<QHH", tx, first, len(block)) + bytes(v for c in block for v in c),
                )
            data = exact(
                client.call(session, 9, struct.pack("<QIB3x", tx, generation, int(args.save))), 8
            )
            return dict(
                generation=struct.unpack_from("<I", data)[0],
                mode="custom",
                persistence="SAVE_PENDING" if data[4] else "APPLIED",
            )
        except Exception:
            try:
                client.call(session, 10, struct.pack("<Q", tx))
            except (OSError, ValueError):
                pass
            raise
    raise ValueError("unknown keyboard command")
