#!/usr/bin/env python3
"""Offline, parallel verification of MoniKa DEK and session-state extraction.

Only Memory64 payload ranges are searched. NumPy performs anchor/entropy and
decrypted-header comparisons in bulk; multiprocessing distributes dump parsing
and AES validation. No keys, paths, or message contents are printed.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import glob
import json
import mmap
import os
import re
import struct
import time
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

ANCHOR = 0x88
ANCHOR_TAG_MAX = 8
DEK_LEN = 32
RESERVED = np.asarray((80, 48, 64, 16, 32, 96), dtype=np.uint8)
MARKERS = (
    b"S_LOGIN_100068",
    b"login_img_line.png",
    b"should_prevent_auto_login_once = no",
)
CHUNK_SIZE = 32 * 1024 * 1024


@dataclass
class DumpCandidates:
    path: str
    architecture: str
    bytes_scanned: int
    anchored: int
    candidates: np.ndarray
    candidate_offsets: np.ndarray
    markers: tuple[bool, bool, bool]


def memory64_layout(path: str) -> tuple[str, list[tuple[int, int]]]:
    """Return architecture and private/readable (file offset, size) ranges."""
    with open(path, "rb") as stream:
        header = stream.read(32)
        if len(header) != 32 or header[:4] != b"MDMP":
            raise ValueError(f"{path}: not a minidump")
        stream_count, directory_rva = struct.unpack_from("<II", header, 8)
        stream.seek(directory_rva)
        directories = [struct.unpack("<III", stream.read(12)) for _ in range(stream_count)]

        system_info = next((entry for entry in directories if entry[0] == 7), None)
        architecture = "unknown"
        if system_info:
            stream.seek(system_info[2])
            machine = struct.unpack("<H", stream.read(2))[0]
            architecture = {0: "x86", 9: "x64", 12: "arm64"}.get(machine, str(machine))

        memory64 = next((entry for entry in directories if entry[0] == 9), None)
        if not memory64:
            raise ValueError(f"{path}: Memory64 stream not found")
        stream.seek(memory64[2])
        range_count, payload_offset = struct.unpack("<QQ", stream.read(16))
        descriptors = [struct.unpack("<QQ", stream.read(16)) for _ in range(range_count)]

        memory_info = next((entry for entry in directories if entry[0] == 16), None)
        interesting: dict[int, bool] = {}
        if memory_info:
            stream.seek(memory_info[2])
            header_size, entry_size, entry_count = struct.unpack("<IIQ", stream.read(16))
            for index in range(entry_count):
                stream.seek(memory_info[2] + header_size + index * entry_size)
                raw = stream.read(entry_size)
                if len(raw) < 44:
                    continue
                base = struct.unpack_from("<Q", raw, 0)[0]
                state, protect, memory_type = struct.unpack_from("<III", raw, 32)
                protection = protect & 0xFF
                readable = protection in (0x02, 0x04, 0x08, 0x20, 0x40, 0x80)
                interesting[base] = state == 0x1000 and memory_type == 0x20000 and not (protect & 0x100) and readable

    ranges: list[tuple[int, int]] = []
    offset = payload_offset
    for virtual_address, size in descriptors:
        if size and (not interesting or interesting.get(virtual_address, False)):
            ranges.append((offset, size))
        offset += size
    return architecture, ranges


def extract_dump(task: tuple[str, tuple[int, ...]]) -> DumpCandidates:
    path, key_offsets = task
    architecture, ranges = memory64_layout(path)
    candidate_batches: list[np.ndarray] = []
    offset_batches: list[np.ndarray] = []
    marker_flags = [False] * len(MARKERS)
    anchored = 0
    scanned = 0

    with open(path, "rb") as stream:
        mapped = mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            for file_offset, range_size in ranges:
                carry = b""
                seen_anchor_offsets: set[int] = set()
                for relative in range(0, range_size, CHUNK_SIZE):
                    size = min(CHUNK_SIZE, range_size - relative)
                    block = carry + mapped[file_offset + relative : file_offset + relative + size]
                    block_base = relative - len(carry)
                    scanned += size

                    for index, marker in enumerate(MARKERS):
                        if not marker_flags[index] and block.find(marker) >= 0:
                            marker_flags[index] = True

                    data = np.frombuffer(block, dtype=np.uint8)
                    positions = np.flatnonzero(data == ANCHOR)
                    positions = positions[(positions > 0) & (positions + max(key_offsets) + DEK_LEN <= data.size)]
                    if positions.size:
                        positions = positions[data[positions - 1] < ANCHOR_TAG_MAX]
                    if positions.size:
                        absolute = positions.astype(np.int64) + block_base
                        fresh = np.fromiter(
                            (int(value) not in seen_anchor_offsets for value in absolute),
                            dtype=np.bool_,
                            count=absolute.size,
                        )
                        positions, absolute = positions[fresh], absolute[fresh]
                        seen_anchor_offsets.update(int(value) for value in absolute)
                    for key_offset in key_offsets:
                        if positions.size:
                            keys = np.stack(
                                [data[positions + key_offset + column] for column in range(DEK_LEN)], axis=1
                            )
                            keys = keys[np.count_nonzero(keys == 0, axis=1) <= 2]
                            anchored += keys.shape[0]
                            if keys.size:
                                candidate_batches.append(keys.copy())
                                offset_batches.append(np.full(keys.shape[0], key_offset, dtype=np.uint16))
                    carry = block[-(DEK_LEN + max(key_offsets)) :]
        finally:
            mapped.close()

    if candidate_batches:
        combined = np.concatenate(candidate_batches)
        labels = np.concatenate(offset_batches)
        candidates, first_indexes = np.unique(combined, axis=0, return_index=True)
        candidate_offsets = labels[first_indexes]
    else:
        candidates = np.empty((0, DEK_LEN), dtype=np.uint8)
        candidate_offsets = np.empty((0,), dtype=np.uint16)
    return DumpCandidates(path, architecture, scanned, anchored, candidates, candidate_offsets, tuple(marker_flags))


def discover_user_dir(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit).resolve()
    local = os.environ.get("LOCALAPPDATA")
    if not local:
        raise RuntimeError("LOCALAPPDATA is unavailable; pass --user-dir")
    root = Path(local) / "Kakao" / "KakaoTalk" / "users"
    users = [path for path in root.iterdir() if path.is_dir() and re.fullmatch(r"[0-9a-fA-F]{40}", path.name)]
    if not users:
        raise RuntimeError("no KakaoTalk user directory found")
    return max(
        users, key=lambda path: (path / "keystore.bin").stat().st_mtime if (path / "keystore.bin").exists() else 0
    )


def load_oracle(user_dir: Path) -> tuple[bytes, np.ndarray]:
    cipher_blocks: list[bytes] = []
    ivs: list[list[bytes]] = []
    for path in user_dir.rglob("*.edb"):
        try:
            with path.open("rb") as stream:
                page = stream.read(4096)
        except OSError:
            continue
        if len(page) != 4096:
            continue
        cipher_blocks.append(page[16:32])
        ivs.append(
            [
                np.frombuffer(page[4096 - int(reserved) : 4096 - int(reserved) + 16], dtype=np.uint8)
                for reserved in RESERVED
            ]
        )
    if not cipher_blocks:
        raise RuntimeError("no complete .edb page-1 headers found")
    return b"".join(cipher_blocks), np.asarray(ivs, dtype=np.uint8)


_ORACLE_BLOCKS = b""
_ORACLE_IVS = np.empty((0, 0, 0), dtype=np.uint8)


def init_validator(blocks: bytes, ivs: np.ndarray) -> None:
    global _ORACLE_BLOCKS, _ORACLE_IVS
    _ORACLE_BLOCKS, _ORACLE_IVS = blocks, ivs


def validate_batch(indexed_keys: tuple[int, np.ndarray]) -> list[tuple[int, tuple[int, ...]]]:
    base_index, keys = indexed_keys
    results: list[tuple[int, tuple[int, ...]]] = []
    database_count = _ORACLE_IVS.shape[0]
    for offset, key_array in enumerate(keys):
        key = key_array.tobytes()
        decrypted = Cipher(algorithms.AES(key), modes.ECB()).decryptor().update(_ORACLE_BLOCKS)
        plain = np.frombuffer(decrypted, dtype=np.uint8).reshape(database_count, 16)
        decoded = np.bitwise_xor(plain[:, None, :], _ORACLE_IVS)
        valid = (
            (decoded[:, :, 4] == RESERVED[None, :])
            & (decoded[:, :, 0] == 0x10)
            & (decoded[:, :, 1] == 0)
            & np.isin(decoded[:, :, 2], (1, 2))
            & np.isin(decoded[:, :, 3], (1, 2))
            & (decoded[:, :, 5] == 0x40)
            & (decoded[:, :, 6] == 0x20)
            & (decoded[:, :, 7] == 0x20)
        )
        hits = tuple(int(value) for value in np.flatnonzero(np.any(valid, axis=1)))
        if hits:
            results.append((base_index + offset, hits))
    return results


def batches(keys: np.ndarray, size: int) -> Iterable[tuple[int, np.ndarray]]:
    for start in range(0, keys.shape[0], size):
        yield start, keys[start : start + size]


def classify(markers: tuple[bool, bool, bool]) -> str:
    login_scene = markers[0] and markers[1]
    if login_scene and markers[2]:
        return "signed_out"
    if login_scene:
        return "login_interstitial"
    if not markers[2]:
        return "signed_in_candidate"
    return "unknown"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("dumps", nargs="+", help="minidumps or glob-expanded paths")
    parser.add_argument("--user-dir", help="KakaoTalk 40-hex user directory (auto-detected by default)")
    parser.add_argument("--workers", type=int, default=max(1, min(os.cpu_count() or 1, 8)))
    parser.add_argument("--batch-size", type=int, default=2048)
    parser.add_argument(
        "--key-offsets",
        default="1,16,49,56",
        help="comma-separated offsets after 0x88, or an inclusive range such as 1:64",
    )
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = parser.parse_args()
    expanded: list[str] = []
    for pattern in args.dumps:
        matches = glob.glob(pattern)
        expanded.extend(matches if matches else [pattern])
    paths = sorted(dict.fromkeys(str(Path(path).resolve()) for path in expanded))
    if ":" in args.key_offsets:
        first, last = (int(value) for value in args.key_offsets.split(":", 1))
        key_offsets = tuple(range(first, last + 1))
    else:
        key_offsets = tuple(int(value) for value in args.key_offsets.split(","))
    if not key_offsets or min(key_offsets) < 1 or max(key_offsets) > 256:
        parser.error("--key-offsets must be between 1 and 256")
    started = time.perf_counter()

    with concurrent.futures.ProcessPoolExecutor(max_workers=min(args.workers, len(paths))) as pool:
        dumps = list(pool.map(extract_dump, ((path, key_offsets) for path in paths)))

    all_keys = np.unique(np.concatenate([dump.candidates for dump in dumps]), axis=0)
    blocks, ivs = load_oracle(discover_user_dir(args.user_dir))
    valid: dict[bytes, tuple[int, ...]] = {}
    with concurrent.futures.ProcessPoolExecutor(
        max_workers=args.workers, initializer=init_validator, initargs=(blocks, ivs)
    ) as pool:
        for result in pool.map(validate_batch, batches(all_keys, args.batch_size)):
            for index, database_indexes in result:
                valid[all_keys[index].tobytes()] = database_indexes

    rows = []
    for dump in dumps:
        databases: set[int] = set()
        validated_keys = 0
        validated_by_offset: dict[int, int] = {}
        for key, key_offset in zip(dump.candidates, dump.candidate_offsets, strict=True):
            hits = valid.get(key.tobytes())
            if hits:
                validated_keys += 1
                databases.update(hits)
                offset_value = int(key_offset)
                validated_by_offset[offset_value] = validated_by_offset.get(offset_value, 0) + 1
        rows.append(
            {
                "dump": Path(dump.path).name,
                "architecture": dump.architecture,
                "memory_mb": round(dump.bytes_scanned / 1_000_000, 1),
                "anchored_candidates": dump.anchored,
                "unique_candidates": int(dump.candidates.shape[0]),
                "validated_keys": validated_keys,
                "validated_keys_by_offset": dict(sorted(validated_by_offset.items())),
                "unlocked_databases": len(databases),
                "session": classify(dump.markers),
                "login_scene": dump.markers[0] and dump.markers[1],
                "signed_out_marker": dump.markers[2],
            }
        )

    report = {
        "oracle_databases": int(ivs.shape[0]),
        "globally_unique_candidates": int(all_keys.shape[0]),
        "elapsed_seconds": round(time.perf_counter() - started, 2),
        "results": rows,
    }
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(
            f"Oracle databases: {report['oracle_databases']}; global candidates: {report['globally_unique_candidates']}"
        )
        for row in rows:
            print(
                f"{row['dump']}: {row['architecture']}, {row['session']}, "
                f"{row['validated_keys']} keys / {row['unlocked_databases']} databases "
                f"({row['unique_candidates']} candidates)"
            )
        print(f"Elapsed: {report['elapsed_seconds']}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
