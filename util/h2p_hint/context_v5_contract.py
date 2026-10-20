"""Bit-exact reference and packed-image encoder for H2P Hint metadata v7 spec.

The unversioned H2PHINTL binary layout intentionally matches v6. Its bytes
alone cannot distinguish the history contract: callers must validate v7
metadata/profile identities and the accompanying image SHA before loading.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from pathlib import Path


MAGIC = b"H2PHINTL"
VERSION = 7
HASH_VERSION = 7
MASK28 = (1 << 28) - 1
MASK14 = (1 << 14) - 1
MASK47 = (1 << 47) - 1
MASK64 = (1 << 64) - 1
MASK128 = (1 << 128) - 1
CONTEXT_ENCODING = "xs-gem5-spec-startpc-ith128-path128-lhr16-inflight8-v1"
MASK_ORDER = ["ITH", "Call", "Ret", "LHR"]
HISTORY = {
    "ith_entries": 128,
    "ith_use_bits": 128,
    "lhr_entries": 16,
    "lhr_bits": 32,
    "lhr_use_bits": 16,
    "lhr_head_bits": 5,
    "call_entries": 128,
    "ret_entries": 128,
    "path_use_bits": 128,
    "target_shift": 1,
}
LAYOUT = {
    "cam_entries": 16,
    "context_entries": 4096,
    "ways": 2,
    "slot_entries": 32,
    "conf_queue_depth": 32,
    "hint_meta_entries": 16,
    "lhr_max_uncommitted": 8,
}
CAM_COUNT = 16
CONTEXT_COUNT = 4096
SLOT_COUNT = 32
SET_COUNT = CONTEXT_COUNT // 2
BODY_BYTES = CAM_COUNT * 8 + CONTEXT_COUNT * 4 + SLOT_COUNT * 8
IMAGE_BYTES = 16 + BODY_BYTES
FETCH_BLOCK_BYTES = 64
FETCH_ALIGN_BYTES = 32


def fold(value: int, width: int) -> int:
    if type(value) is not int or value < 0 or type(width) is not int or width <= 0:
        raise ValueError("fold requires unsigned integer and positive width")
    result = 0
    mask = (1 << width) - 1
    while value:
        result ^= value & mask
        value >>= width
    return result


def component28(value: int) -> int:
    index = fold(value, 11)
    tag = fold(value, 17) ^ ((fold(value, 16) << 2) & 0x1FFFF)
    return (tag << 11) | index


def fold14_pc(pc: int) -> int:
    return fold(address_word(pc), 14)


def fold128(value: int) -> int:
    if type(value) is not int or not 0 <= value <= MASK128:
        raise ValueError("history must be an unsigned 128-bit integer")
    return component28(value)


def context_components(ith_low128: int, lhr_low16: int,
                       call_low128: int, ret_low128: int) -> tuple[int, int, int, int]:
    # Keep named argument order for existing callers; return order follows mask bits.
    if type(lhr_low16) is not int or not 0 <= lhr_low16 <= 0xFFFF:
        raise ValueError("local history must be an unsigned 16-bit integer")
    return (fold128(ith_low128), fold128(call_low128),
            fold128(ret_low128), component28(lhr_low16))


def hash_key(pc: int, components: tuple[int, int, int, int], mask: int) -> int:
    if type(mask) is not int or not 0 <= mask < 16 or len(components) != 4:
        raise ValueError("expected four context components and a four-bit mask")
    key = component28(address_word(pc))
    for bit, component in enumerate(components):
        if type(component) is not int or not 0 <= component <= MASK28:
            raise ValueError("context component exceeds 28 bits")
        if mask & (1 << bit):
            key ^= component
    return key


def advance_target(history: int, target: int, bit_count: int) -> int:
    return ((history << 2) | ((target >> 1) & 3)) & ((1 << bit_count) - 1)


def advance_path(history: int, pc: int) -> int:
    return ((history << 2) ^ fold14_pc(pc)) & ((1 << 256) - 1)


def address_word(value: int) -> int:
    if type(value) is not int or not 0 <= value <= MASK64 or value & 1:
        raise ValueError("PC/target must be an even unsigned 64-bit address")
    low = value & ((1 << 48) - 1)
    canonical = low | (MASK64 ^ ((1 << 48) - 1)) if low >> 47 else low
    if value != canonical:
        raise ValueError("PC/target must be canonical in 48 bits")
    return (value >> 1) & MASK47


def cam_window_end(start_pc: int) -> int | None:
    address_word(start_pc)
    if start_pc > MASK64 - FETCH_BLOCK_BYTES:
        return None
    end = (start_pc + FETCH_BLOCK_BYTES) & ~(FETCH_ALIGN_BYTES - 1)
    return end if end > start_pc else None


def select_cam_pc(start_pc: int, pcs: list[int]) -> int | None:
    address_word(start_pc)
    return start_pc if start_pc in pcs else None


def _number(value: object) -> int:
    if type(value) is int:
        return value
    if type(value) is str and value.startswith("0x"):
        return int(value, 16)
    raise ValueError("expected integer or hexadecimal string")


def _same_typed(value: object, expected: object) -> bool:
    if type(value) is not type(expected):
        return False
    if isinstance(expected, dict):
        return value.keys() == expected.keys() and all(
            _same_typed(value[key], item) for key, item in expected.items()
        )
    if isinstance(expected, list):
        return len(value) == len(expected) and all(
            _same_typed(item, reference) for item, reference in zip(value, expected)
        )
    return value == expected


def _rows(metadata: dict) -> tuple[list[int], list[int], list[int]]:
    if not isinstance(metadata, dict):
        raise ValueError("metadata must be an object")
    for name, expected in (
        ("format", "h2p-hint"),
        ("version", VERSION),
        ("storage", "finite"),
        ("hash_version", HASH_VERSION),
        ("context_encoding", CONTEXT_ENCODING),
        ("mask_order", MASK_ORDER),
        ("history", HISTORY),
        ("layout", LAYOUT),
    ):
        if not _same_typed(metadata.get(name), expected):
            raise ValueError(f"metadata {name} differs from Hint v7 spec contract")
    source = metadata.get("source")
    if not isinstance(source, dict) or not isinstance(source.get("workload_id"), str) \
            or not source["workload_id"]:
        raise ValueError("metadata source must identify the workload")
    for name in ("checkpoint_sha256", "profile_sha256", "builder_sha256"):
        if not isinstance(source.get(name), str) or not re.fullmatch(
            r"[0-9a-f]{64}", source[name]
        ):
            raise ValueError(f"metadata source {name} must be a SHA-256 digest")

    cam = metadata.get("cam")
    slots = metadata.get("slots")
    entries = metadata.get("entries")
    if not isinstance(cam, list) or len(cam) > CAM_COUNT:
        raise ValueError("CAM exceeds 16 rows")
    if not isinstance(slots, list) or len(slots) > SLOT_COUNT:
        raise ValueError("Slot Table exceeds 32 rows")
    if not isinstance(entries, list) or len(entries) > CONTEXT_COUNT:
        raise ValueError("Context table exceeds 4096 rows")

    cam_words = [0] * CAM_COUNT
    last_pc = -1
    invalid_seen = False
    for index, row in enumerate(cam):
        if not isinstance(row, dict) or type(row.get("valid", True)) is not bool:
            raise ValueError("CAM row has invalid shape")
        valid = row.get("valid", True)
        if not valid:
            invalid_seen = True
            continue
        if invalid_seen:
            raise ValueError("valid CAM rows must precede invalid rows")
        pc = _number(row.get("pc"))
        word = address_word(pc)
        mask = _number(row.get("mask"))
        if pc <= last_pc or not 0 <= mask < 16:
            raise ValueError("CAM PCs must ascend and masks must fit four bits")
        last_pc = pc
        cam_words[index] = word | (mask << 47) | (1 << 51)

    slot_words = [0] * SLOT_COUNT
    seen_targets = set()
    for index, target in enumerate(slots):
        word = address_word(_number(target))
        if word in seen_targets:
            raise ValueError("duplicate Slot target")
        seen_targets.add(word)
        slot_words[index] = word | (1 << 47)

    context_words = [0] * CONTEXT_COUNT
    used_positions = set()
    used_tags = set()
    for row in entries:
        if not isinstance(row, dict) or type(row.get("valid", True)) is not bool:
            raise ValueError("Context row has invalid shape")
        if not row.get("valid", True):
            continue
        set_index = _number(row.get("set"))
        way = _number(row.get("way"))
        tag = _number(row.get("tag"))
        slot = _number(row.get("slot"))
        conf = _number(row.get("conf"))
        if not (0 <= set_index < SET_COUNT and 0 <= way < 2):
            raise ValueError("Context row position is out of range")
        if not (0 <= tag < (1 << 17) and 0 <= slot < len(slots)):
            raise ValueError("Context tag or Slot index is out of range")
        if conf != 3:
            raise ValueError("valid Context rows must start at confidence three")
        if (set_index, way) in used_positions or (set_index, tag) in used_tags:
            raise ValueError("duplicate Context position or set/tag")
        used_positions.add((set_index, way))
        used_tags.add((set_index, tag))
        context_words[set_index * 2 + way] = (
            tag | (slot << 17) | (conf << 22) | (1 << 24)
        )
    return cam_words, context_words, slot_words


def pack_image(metadata: dict) -> bytes:
    cam, context, slots = _rows(metadata)
    body = (
        struct.pack("<16Q", *cam)
        + struct.pack("<4096I", *context)
        + struct.pack("<32Q", *slots)
    )
    header = MAGIC + bytes(8)
    return header + body


def unpack_image(image: bytes) -> tuple[tuple[int, ...], tuple[int, ...], tuple[int, ...]]:
    if len(image) != IMAGE_BYTES:
        raise ValueError("Hint image length differs from v7 spec contract")
    magic, reserved = struct.unpack_from("<8s8s", image)
    body = image[16:]
    if magic != MAGIC or reserved != bytes(8):
        raise ValueError("Hint image header or checksum is invalid")
    cam = struct.unpack_from("<16Q", body)
    context = struct.unpack_from("<4096I", body, CAM_COUNT * 8)
    slots = struct.unpack_from("<32Q", body, CAM_COUNT * 8 + CONTEXT_COUNT * 4)
    if any(value >> 52 for value in cam):
        raise ValueError("CAM reserved bits are nonzero")
    if any(value >> 25 for value in context):
        raise ValueError("Context reserved bits are nonzero")
    if any(value >> 48 for value in slots):
        raise ValueError("Slot reserved bits are nonzero")
    valid_slots = set()
    seen_targets = set()
    invalid_slot_seen = False
    for index, value in enumerate(slots):
        if not value & (1 << 47):
            invalid_slot_seen = True
            if value:
                raise ValueError("invalid Slot row must be zero")
            continue
        target = value & MASK47
        if invalid_slot_seen or target in seen_targets:
            raise ValueError("valid Slots must ascend by index without duplicates")
        valid_slots.add(index)
        seen_targets.add(target)
    previous_pc = -1
    invalid_cam_seen = False
    for value in cam:
        if not value & (1 << 51):
            invalid_cam_seen = True
            if value:
                raise ValueError("invalid CAM row must be zero")
        else:
            pc_word = value & MASK47
            if invalid_cam_seen or pc_word <= previous_pc:
                raise ValueError("valid CAM rows must ascend before invalid rows")
            previous_pc = pc_word
    for set_index in range(SET_COUNT):
        seen_tags = set()
        for way in range(2):
            value = context[set_index * 2 + way]
            if not value & (1 << 24):
                if value:
                    raise ValueError("invalid Context row must be zero")
                continue
            if (value >> 22) & 3 != 3:
                raise ValueError("valid Context row must start at confidence three")
            if ((value >> 17) & 31) not in valid_slots:
                raise ValueError("valid Context row points to an unloaded Slot")
            tag = value & ((1 << 17) - 1)
            if tag in seen_tags:
                raise ValueError("duplicate valid Context tag within a set")
            seen_tags.add(tag)
    return cam, context, slots


def self_test() -> None:
    vectors = json.loads(Path(__file__).with_name("hint-context-v7-vectors.json").read_text())
    assert vectors["version"] == VERSION
    for row in vectors["hash"]:
        components = context_components(int(row["ith_low128"], 16),
            int(row["lhr_low16"], 16), int(row["call_low128"], 16),
            int(row["ret_low128"], 16))
        assert list(components) == row["components"]
        assert hash_key(int(row["pc"], 16), components, row["mask"]) == row["key"]
    sample = json.loads(Path(__file__).with_name("hint-context-v7-metadata.json").read_text())
    image = pack_image(sample)
    assert len(image) == IMAGE_BYTES
    assert image[:16] == MAGIC + bytes(8)
    assert hashlib.sha256(image).hexdigest() == vectors["sample_image_sha256"]
    unpack_image(image)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="action", required=True)
    subparsers.add_parser("self-test")
    pack = subparsers.add_parser("pack")
    pack.add_argument("metadata", type=Path)
    pack.add_argument("image", type=Path)
    verify = subparsers.add_parser("verify")
    verify.add_argument("image", type=Path)
    args = parser.parse_args()
    if args.action == "self-test":
        self_test()
    elif args.action == "pack":
        args.image.write_bytes(pack_image(json.loads(args.metadata.read_text())))
    else:
        unpack_image(args.image.read_bytes())


if __name__ == "__main__":
    main()
