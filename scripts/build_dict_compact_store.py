#!/usr/bin/env python3
"""Stream Rime YAML dictionaries into AquaType's deterministic mmap stores.

C1 builds DictionaryFallbackStore from five dictionaries.  C2 builds LexicalPriorStore from the
bounded prefix of aquatype_mcbopomofo.dict.yaml.  Parsing and tie semantics mirror the frozen
Swift checkpoint, except that a percentage weight is read as librime reads it (see parse_weight);
SQLite keeps peak Python memory bounded while providing deterministic external sorts.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import random
import shutil
import sqlite3
import struct
import sys
import tempfile
import unicodedata
from typing import BinaryIO, Iterable, Iterator

try:
    import regex as grapheme_regex
except ImportError as error:  # pragma: no cover - deployment preflight gives a direct diagnostic.
    raise SystemExit("build_dict_compact_store.py requires the Python 'regex' package") from error


FORMAT_NAME = "aquatype-dict-compact"
FORMAT_VERSION = 2
ENDIAN_MARKER = 0x01020304
KIND_FALLBACK = 1
KIND_LEXICAL = 2
HEADER_BYTES = 256
MAGIC = b"AQDICT1\0"
BUILDER_NAME = "aquatype-dict-compact-builder"
BUILDER_VERSION = 1
LEXICAL_SOURCE_NAME = "aquatype_mcbopomofo.dict.yaml"
FALLBACK_BEST_INDEX_RECORD_BYTES = 20
FALLBACK_HASH_SLOT_BYTES = 3
FALLBACK_HASH_MAX_LOAD_NUMERATOR = 7
FALLBACK_HASH_MAX_LOAD_DENOMINATOR = 8
LEXICAL_INDEX_RECORD_BYTES = 16
LEXICAL_PROBE_ROWS = 100_000
SOURCE_NAMES = (
    "aquatype_lexicon.dict.yaml",
    "aquatype_phrases.dict.yaml",
    "aquatype_mcbopomofo.dict.yaml",
    "terra_pinyin.dict.yaml",
    "aquatype_variants.dict.yaml",
)
PINYIN_INTERNAL = "bpmfdtnlgkhjqxZCSrzcsiuvaoeEAIOUMNKGR12345"
ZHUYIN_KEYS = "1qaz2wsxedcrfv5tgbyhnujm8ik,9ol.0p;/- 6347"
PINYIN_TO_KEY = dict(zip(PINYIN_INTERNAL, ZHUYIN_KEYS))
CJK_RANGES = ((0x3400, 0x4DBF), (0x4E00, 0x9FFF), (0xF900, 0xFAFF))
U16_MAX = (1 << 16) - 1
U32_MAX = (1 << 32) - 1


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def clean_text(text: str) -> str:
    return "".join(text.split()).replace("\t", " ")


def has_cjk(text: str) -> bool:
    return any(any(lo <= ord(ch) <= hi for lo, hi in CJK_RANGES) for ch in text)


def grapheme_count(text: str) -> int:
    return len(grapheme_regex.findall(r"\X", text))


def parse_dict_line(line: str) -> list[str]:
    return line.split("\t") if "\t" in line else line.split()


def parse_weight(tokens: Iterable[str]) -> float | None:
    materialized = list(tokens)
    for token in reversed(materialized):
        cleaned = token.strip()
        # A percentage scales the weight the row would have without one, as librime reads it (essay.txt's count times
        # the percentage).  A row without a weight is 1.0 here, so "85%" is 0.85, not 85: it stays below a row of the
        # same word with no weight, as in librime.
        scale = 1.0
        if cleaned.endswith("%"):
            cleaned, scale = cleaned[:-1], 0.01
        try:
            value = float(cleaned) * scale
        except ValueError:
            continue
        if not math.isfinite(value):
            raise ValueError(f"non-finite dictionary weight is unsupported: {token!r}")
        return value
    return None


def pinyin_to_key(syllable: str, include_tone: bool = True) -> str:
    trimmed = syllable.strip().lower().replace("u:", "v").replace("ü", "v")
    if not trimmed:
        return ""
    tone = ""
    body = trimmed
    if trimmed[-1] in "12345":
        tone = trimmed[-1]
        body = trimmed[:-1]
    # Swift's unicodeScalars.allSatisfy is true for an empty body, so a tone-only token still
    # maps to its keyboard tone key.  Preserve that edge behavior even though production YAMLs
    # normally contain a phonetic body.
    if any(not ("a" <= ch <= "z") for ch in body):
        return ""
    if body == "r":                    # Terra Pinyin writes erhua as r5: it is ㄦ, not ㄖ
        body = "er"
    replacements = (
        (r"iu", "iou"),
        (r"ui", "uei"),
        (r"ong", "ung"),
        (r"yi?", "i"),
        (r"wu?", "u"),
        (r"iu", "v"),
        (r"([jqx])u", r"\1v"),
        (r"([iuv])n", r"\1en"),
        (r"zhi?", "Z"),
        (r"chi?", "C"),
        (r"shi?", "S"),
        (r"([zcsr])i", r"\1"),
        (r"ai", "A"),
        (r"ei", "I"),
        (r"ao", "O"),
        (r"ou", "U"),
        (r"ang", "K"),
        (r"eng", "G"),
        (r"an", "M"),
        (r"en", "N"),
        (r"er", "R"),
        (r"eh", "E"),
        (r"([iv])e", r"\1E"),
    )
    for pattern, replacement in replacements:
        body = re.sub(pattern, replacement, body)
    internal = body + (tone if include_tone else "")
    try:
        return "".join(PINYIN_TO_KEY[ch] for ch in internal)
    except KeyError:
        return ""


def iter_body_rows(
    path: Path, pinyin_cache: dict[str, str] | None = None
) -> Iterator[tuple[str, str, float]]:
    cache = pinyin_cache if pinyin_cache is not None else {}
    in_body = False
    with path.open("r", encoding="utf-8", newline=None) as stream:
        for raw_line in stream:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if line == "...":
                in_body = True
                continue
            if not in_body:
                continue
            parts = parse_dict_line(line)
            if len(parts) < 2:
                continue
            text = clean_text(parts[0])
            if not text or not has_cjk(text):
                continue
            syllables = parts[1].split()
            if not syllables:
                continue
            raw_parts: list[str] = []
            for syllable in syllables:
                converted = cache.get(syllable)
                if converted is None:
                    converted = pinyin_to_key(syllable, include_tone=True)
                    cache[syllable] = converted
                raw_parts.append(converted)
            # The engine looks words up by their keys without the first tone's key (Space), the way McBopomofo
            # writes them, so a row that spells the first tone out is stored the same way.
            raw = "".join(raw_parts).replace(" ", "")
            if not raw:
                continue
            weight = parse_weight(parts[2:])
            yield text, raw, 1.0 if weight is None else weight


def source_metadata(rime_dir: Path) -> list[dict[str, object]]:
    result: list[dict[str, object]] = []
    for name in SOURCE_NAMES:
        path = rime_dir / name
        if not path.is_file():
            raise FileNotFoundError(f"required C1 dictionary source missing: {path}")
        result.append({"name": name, "sha256": sha256_file(path), "bytes": path.stat().st_size})
    return result


def source_aggregate_sha(sources: list[dict[str, object]]) -> str:
    digest = hashlib.sha256()
    for source in sources:
        digest.update(str(source["name"]).encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(source["sha256"]).encode("ascii"))
        digest.update(b"\0")
        digest.update(str(source["bytes"]).encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def configure_database(path: Path, max_memory_mb: int) -> sqlite3.Connection:
    connection = sqlite3.connect(path)
    cache_kib = max(32 * 1024, min(max_memory_mb * 1024 // 2, 256 * 1024))
    connection.execute("PRAGMA journal_mode=OFF")
    connection.execute("PRAGMA synchronous=OFF")
    connection.execute("PRAGMA temp_store=FILE")
    connection.execute(f"PRAGMA cache_size=-{cache_kib}")
    connection.execute("PRAGMA locking_mode=EXCLUSIVE")
    connection.executescript(
        """
        CREATE TABLE rows (
            raw TEXT NOT NULL,
            text_key TEXT NOT NULL,
            text_display TEXT NOT NULL,
            weight REAL NOT NULL,
            weight_bits INTEGER NOT NULL,
            source_id INTEGER NOT NULL,
            text_len INTEGER NOT NULL,
            PRIMARY KEY (raw, text_key)
        ) WITHOUT ROWID;
        CREATE TABLE best (
            text_key TEXT PRIMARY KEY,
            text_display TEXT NOT NULL,
            raw TEXT NOT NULL,
            weight REAL NOT NULL,
            weight_bits INTEGER NOT NULL
        ) WITHOUT ROWID;
        """
    )
    return connection


def insert_sources(connection: sqlite3.Connection, rime_dir: Path) -> tuple[list[int], int]:
    row_counts: list[int] = []
    accepted = 0
    row_sql = """
        INSERT INTO rows(raw, text_key, text_display, weight, weight_bits, source_id, text_len)
        VALUES (?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(raw, text_key) DO UPDATE SET
          text_display=excluded.text_display,
          weight=excluded.weight,
          weight_bits=excluded.weight_bits,
          source_id=excluded.source_id,
          text_len=excluded.text_len
        WHERE excluded.weight > rows.weight
    """
    best_sql = """
        INSERT INTO best(text_key, text_display, raw, weight, weight_bits)
        VALUES (?, ?, ?, ?, ?)
        ON CONFLICT(text_key) DO UPDATE SET
          raw=excluded.raw,
          weight=excluded.weight,
          weight_bits=excluded.weight_bits
        WHERE excluded.weight > best.weight
    """
    connection.execute("BEGIN")
    pending = 0
    pinyin_cache: dict[str, str] = {}
    for source_id, name in enumerate(SOURCE_NAMES):
        count = 0
        for text, raw, weight in iter_body_rows(rime_dir / name, pinyin_cache):
            text_key = unicodedata.normalize("NFC", text)
            bits = struct.unpack("<Q", struct.pack("<d", weight))[0]
            sqlite_bits = bits if bits < (1 << 63) else bits - (1 << 64)
            # Swift Dictionary keys use canonical equality but retain the first key's original
            # representation.  best.text_display is therefore intentionally absent from the
            # conflict update; rows.text_display follows the strict-greater winning row value.
            connection.execute(best_sql, (text_key, text, raw, weight, sqlite_bits))
            if grapheme_count(text) >= 2:
                connection.execute(
                    row_sql,
                    (
                        raw, text_key, text, weight, sqlite_bits,
                        source_id, grapheme_count(text),
                    ),
                )
            count += 1
            accepted += 1
            pending += 1
            if pending >= 50_000:
                connection.commit()
                connection.execute("BEGIN")
                pending = 0
        row_counts.append(count)
    connection.commit()
    return row_counts, accepted


def checked_utf8(value: str) -> bytes:
    encoded = value.encode("utf-8")
    if len(encoded) > U16_MAX:
        raise ValueError(f"UTF-8 key exceeds u16 length: {len(encoded)} bytes")
    return encoded


def write_pool_and_offsets(
    connection: sqlite3.Connection,
    pool_path: Path,
    table_name: str,
    select_sql: str,
) -> tuple[int, int]:
    connection.execute(
        f"CREATE TABLE {table_name}(value TEXT PRIMARY KEY, off INTEGER NOT NULL, len INTEGER NOT NULL) WITHOUT ROWID"
    )
    offset = 0
    count = 0
    batch: list[tuple[str, int, int]] = []
    with pool_path.open("wb") as pool:
        for (value,) in connection.execute(select_sql):
            encoded = checked_utf8(value)
            if offset > U32_MAX:
                raise ValueError("string pool exceeds u32 offsets")
            pool.write(encoded)
            batch.append((value, offset, len(encoded)))
            offset += len(encoded)
            count += 1
            if len(batch) >= 20_000:
                connection.executemany(
                    f"INSERT INTO {table_name}(value, off, len) VALUES (?, ?, ?)", batch
                )
                connection.commit()
                batch.clear()
        if batch:
            connection.executemany(
                f"INSERT INTO {table_name}(value, off, len) VALUES (?, ?, ?)", batch
            )
            connection.commit()
    return offset, count


def write_rows_and_raw_index(
    connection: sqlite3.Connection, row_path: Path, raw_index_path: Path
) -> tuple[int, int]:
    query = """
        SELECT r.raw, ro.off, ro.len, tx.off, tx.len, r.source_id, r.weight_bits
        FROM rows AS r
        JOIN raw_offsets AS ro ON ro.value = r.raw
        JOIN text_offsets AS tx ON tx.value = r.text_display
        ORDER BY r.raw COLLATE BINARY ASC,
                 r.weight DESC,
                 r.text_len DESC,
                 r.text_key COLLATE BINARY ASC
    """
    current_raw: str | None = None
    current_raw_off = 0
    current_raw_len = 0
    group_start = 0
    group_count = 0
    row_count = 0
    raw_count = 0
    with row_path.open("wb") as rows, raw_index_path.open("wb") as raw_index:
        for raw, raw_off, raw_len, text_off, text_len, source_id, weight_bits in connection.execute(query):
            if raw != current_raw:
                if current_raw is not None:
                    raw_index.write(struct.pack(
                        "<IHHII", current_raw_off, current_raw_len, 0, group_start, group_count
                    ))
                    raw_count += 1
                current_raw = raw
                current_raw_off = int(raw_off)
                current_raw_len = int(raw_len)
                group_start = row_count
                group_count = 0
            rows.write(struct.pack(
                "<IHBBQ", int(text_off), int(text_len), int(source_id), 0,
                int(weight_bits) & ((1 << 64) - 1)
            ))
            row_count += 1
            group_count += 1
        if current_raw is not None:
            raw_index.write(struct.pack(
                "<IHHII", current_raw_off, current_raw_len, 0, group_start, group_count
            ))
            raw_count += 1
    return row_count, raw_count


def write_best_index(connection: sqlite3.Connection, path: Path) -> int:
    query = """
        SELECT key_tx.off, key_tx.len, display_tx.len, display_tx.off, ro.len, ro.off
        FROM best AS b
        JOIN text_offsets AS key_tx ON key_tx.value = b.text_key
        JOIN text_offsets AS display_tx ON display_tx.value = b.text_display
        JOIN raw_offsets AS ro ON ro.value = b.raw
        ORDER BY b.text_key COLLATE BINARY ASC
    """
    count = 0
    with path.open("wb") as stream:
        for key_off, key_len, display_len, display_off, raw_len, raw_off in connection.execute(query):
            stream.write(struct.pack(
                "<IHHIHHI",
                int(key_off), int(key_len), int(display_len), int(display_off),
                int(raw_len), 0, int(raw_off),
            ))
            count += 1
    return count


def fnv1a64(value: str) -> int:
    result = 0xCBF29CE484222325
    for byte in value.encode("utf-8"):
        result ^= byte
        result = (result * 0x100000001B3) & ((1 << 64) - 1)
    return result


def write_u24_hash_index(
    connection: sqlite3.Connection, path: Path, query: str, expected_count: int
) -> int:
    """Write a deterministic mmap-only open-address index whose values are sorted-index+1."""
    if expected_count < 0 or expected_count >= (1 << 20) - 1:
        raise ValueError(f"u20 hash index count is out of range: {expected_count}")
    minimum_slots = max(
        1,
        (
            expected_count * FALLBACK_HASH_MAX_LOAD_DENOMINATOR
            + FALLBACK_HASH_MAX_LOAD_NUMERATOR - 1
        ) // FALLBACK_HASH_MAX_LOAD_NUMERATOR,
    )
    slot_count = 1
    while slot_count < minimum_slots:
        slot_count <<= 1
    table = bytearray(slot_count * FALLBACK_HASH_SLOT_BYTES)
    mask = slot_count - 1

    def read_slot(slot: int) -> int:
        base = slot * FALLBACK_HASH_SLOT_BYTES
        return table[base] | (table[base + 1] << 8) | (table[base + 2] << 16)

    inserted = 0
    for index, (value,) in enumerate(connection.execute(query)):
        hash_value = fnv1a64(str(value))
        encoded_index = (((hash_value >> 60) & 0xF) << 20) | (index + 1)
        slot = hash_value & mask
        for _ in range(slot_count):
            if read_slot(slot) == 0:
                base = slot * FALLBACK_HASH_SLOT_BYTES
                table[base] = encoded_index & 0xFF
                table[base + 1] = (encoded_index >> 8) & 0xFF
                table[base + 2] = (encoded_index >> 16) & 0xFF
                inserted += 1
                break
            slot = (slot + 1) & mask
        else:  # pragma: no cover - the fixed load factor makes this unreachable.
            raise RuntimeError("fallback hash index unexpectedly filled")
    if inserted != expected_count:
        raise RuntimeError(
            f"fallback hash index row mismatch: inserted={inserted} expected={expected_count}"
        )
    with path.open("wb") as stream:
        stream.write(table)
    return slot_count


def write_probe_queries(connection: sqlite3.Connection, path: Path, limit: int = 100_000) -> dict[str, object]:
    # Select across the whole sorted key space, then deterministically shuffle before assigning
    # query classes.  This prevents a lexicographic cold-page bias and keeps exact/prefix/miss in
    # the first (and only) 100k rows consumed by the probe.
    total = int(connection.execute("SELECT COUNT(DISTINCT raw) FROM rows").fetchone()[0])
    if total <= 0:
        raise ValueError("cannot build probe corpus from an empty fallback store")
    target_count = min(limit, total)
    selected: list[str] = []
    for index, (raw,) in enumerate(
        connection.execute("SELECT DISTINCT raw FROM rows ORDER BY raw COLLATE BINARY ASC")
    ):
        target_index = ((2 * len(selected) + 1) * total) // (2 * target_count)
        if index >= target_index:
            selected.append(raw)
            if len(selected) >= target_count:
                break
    if len(selected) < target_count:
        seen = set(selected)
        for (raw,) in connection.execute(
            "SELECT DISTINCT raw FROM rows ORDER BY raw COLLATE BINARY ASC"
        ):
            if raw not in seen:
                selected.append(raw)
                seen.add(raw)
                if len(selected) >= target_count:
                    break
    random.Random(20260710).shuffle(selected)
    class_counts = {"exact": 0, "prefix": 0, "miss": 0}
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        for index, raw in enumerate(selected):
            query_class = index % 5
            if query_class <= 2:
                query = raw
                class_counts["exact"] += 1
            elif query_class == 3:
                query = raw[:-1] if len(raw) > 1 else raw
                class_counts["prefix"] += 1
            else:
                query = raw + "~"
                class_counts["miss"] += 1
            stream.write(query + "\n")
    return {
        "name": path.name,
        "rows": len(selected),
        "classes": class_counts,
        "seed": 20260710,
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def copy_stream(source: Path, output: BinaryIO, digest: hashlib._Hash | None = None) -> int:
    total = 0
    with source.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            output.write(chunk)
            if digest is not None:
                digest.update(chunk)
            total += len(chunk)
    return total


def build_fallback(rime_dir: Path, output: Path, max_memory_mb: int) -> dict[str, object]:
    sources = source_metadata(rime_dir)
    aggregate_sha = source_aggregate_sha(sources)
    output.parent.mkdir(parents=True, exist_ok=True)
    work_parent = output.parent
    with tempfile.TemporaryDirectory(prefix=".dict-store-build-", dir=work_parent) as temp_text:
        temp = Path(temp_text)
        connection = configure_database(temp / "build.sqlite3", max_memory_mb)
        try:
            source_rows, accepted_rows = insert_sources(connection, rime_dir)
            raw_blob_bytes, raw_string_count = write_pool_and_offsets(
                connection,
                temp / "raw.blob",
                "raw_offsets",
                """
                SELECT raw FROM (
                  SELECT raw FROM rows
                  UNION
                  SELECT raw FROM best
                ) ORDER BY raw COLLATE BINARY ASC
                """,
            )
            text_blob_bytes, text_string_count = write_pool_and_offsets(
                connection,
                temp / "text.blob",
                "text_offsets",
                """
                SELECT value FROM (
                  SELECT text_key AS value FROM best
                  UNION
                  SELECT text_display AS value FROM best
                  UNION
                  SELECT text_display AS value FROM rows
                ) ORDER BY value COLLATE BINARY ASC
                """,
            )
            row_count, raw_key_count = write_rows_and_raw_index(
                connection, temp / "rows.bin", temp / "raw_index.bin"
            )
            best_count = write_best_index(connection, temp / "best_index.bin")
            raw_hash_slots = write_u24_hash_index(
                connection,
                temp / "raw_hash.bin",
                "SELECT DISTINCT raw FROM rows ORDER BY raw COLLATE BINARY ASC",
                raw_key_count,
            )
            best_hash_slots = write_u24_hash_index(
                connection,
                temp / "best_hash.bin",
                "SELECT text_key FROM best ORDER BY text_key COLLATE BINARY ASC",
                best_count,
            )
            query_info = write_probe_queries(connection, temp / "probe_queries.txt")
            query_info["name"] = "fallback_probe_queries.v1.txt"
        finally:
            connection.close()

        sections_in_order = (
            (
                "raw_hash", temp / "raw_hash.bin", raw_hash_slots,
                FALLBACK_HASH_SLOT_BYTES,
            ),
            ("raw_index", temp / "raw_index.bin", raw_key_count, 16),
            ("rows", temp / "rows.bin", row_count, 16),
            (
                "best_hash", temp / "best_hash.bin", best_hash_slots,
                FALLBACK_HASH_SLOT_BYTES,
            ),
            (
                "best_index", temp / "best_index.bin", best_count,
                FALLBACK_BEST_INDEX_RECORD_BYTES,
            ),
            ("raw_blob", temp / "raw.blob", raw_blob_bytes, 1),
            ("text_blob", temp / "text.blob", text_blob_bytes, 1),
        )
        offset = HEADER_BYTES
        section_manifest: dict[str, dict[str, int]] = {}
        payload_digest = hashlib.sha256()
        for name, path, count, record_bytes in sections_in_order:
            size = path.stat().st_size
            section_manifest[name] = {
                "offset": offset,
                "bytes": size,
                "count": int(count),
                "record_bytes": record_bytes,
            }
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    payload_digest.update(chunk)
            offset += size
        file_size = offset
        payload_sha = payload_digest.hexdigest()

        builder_path = Path(__file__).resolve()
        builder_implementation_sha = sha256_file(builder_path)
        header = bytearray(HEADER_BYTES)
        header[:8] = MAGIC
        struct.pack_into(
            "<IIIIQ", header, 8, FORMAT_VERSION, ENDIAN_MARKER, KIND_FALLBACK,
            HEADER_BYTES, file_size,
        )
        header[32:64] = bytes.fromhex(payload_sha)
        header[64:96] = bytes.fromhex(aggregate_sha)
        struct.pack_into(
            "<10Q",
            header,
            96,
            section_manifest["raw_index"]["offset"], raw_key_count,
            section_manifest["rows"]["offset"], row_count,
            section_manifest["best_index"]["offset"], best_count,
            section_manifest["raw_blob"]["offset"], raw_blob_bytes,
            section_manifest["text_blob"]["offset"], text_blob_bytes,
        )
        # Header bytes 176...215 bind manifest-only accounting/identity to the immutable binary.
        # The remaining reserved bytes stay zero for deterministic forward-compatible rejection.
        struct.pack_into("<Q", header, 176, accepted_rows)
        header[184:216] = bytes.fromhex(builder_implementation_sha)
        struct.pack_into(
            "<4Q",
            header,
            216,
            section_manifest["raw_hash"]["offset"], raw_hash_slots,
            section_manifest["best_hash"]["offset"], best_hash_slots,
        )

        binary_tmp = temp / "output.aqds"
        with binary_tmp.open("wb") as stream:
            stream.write(header)
            for _, section_path, _, _ in sections_in_order:
                copy_stream(section_path, stream)
            stream.flush()
            os.fsync(stream.fileno())
        if binary_tmp.stat().st_size != file_size:
            raise RuntimeError("assembled binary size differs from header")

        manifest: dict[str, object] = {
            "format": FORMAT_NAME,
            "version": FORMAT_VERSION,
            "endian": "little",
            "kind": "fallback",
            "builder": {
                "name": BUILDER_NAME,
                "version": BUILDER_VERSION,
                "implementation_sha256": builder_implementation_sha,
                "python": sys.version.split()[0],
                "sqlite": sqlite3.sqlite_version,
                "unicode": unicodedata.unidata_version,
                "regex": grapheme_regex.__version__,
            },
            "sources": sources,
            "source_aggregate_sha256": aggregate_sha,
            "payload_sha256": payload_sha,
            "binary_sha256": sha256_file(binary_tmp),
            "binary_bytes": binary_tmp.stat().st_size,
            "sections": section_manifest,
            "counts": {
                "source_valid_rows_by_file": source_rows,
                "source_valid_rows": accepted_rows,
                "rows": row_count,
                "raw_keys": raw_key_count,
                "best_text_keys": best_count,
                "raw_strings": raw_string_count,
                "text_strings": text_string_count,
                "raw_hash_slots": raw_hash_slots,
                "best_hash_slots": best_hash_slots,
            },
            "hash_index": {
                "algorithm": "fnv1a64-linear-u20-fp4-v1",
                "empty_slot": 0,
                "stored_value": "high4_fingerprint_and_sorted_u20_index_plus_one",
                "max_load": "7/8",
            },
            "probe_queries": query_info,
        }
        # Fail closed on a source rewrite during the bounded SQLite build.  Publishing a payload
        # reconciled to a different pre-build SHA would make deterministic rebuild meaningless.
        final_sources = source_metadata(rime_dir)
        if final_sources != sources:
            raise RuntimeError("dictionary source changed during compact build; refusing publication")
        manifest_bytes = (
            json.dumps(manifest, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
        ).encode("utf-8")
        manifest_tmp = temp / "output.manifest.json"
        with manifest_tmp.open("wb") as stream:
            stream.write(manifest_bytes)
            stream.flush()
            os.fsync(stream.fileno())

        query_output = Path(str(output) + ".probe_queries.txt")
        query_tmp = temp / "query-output.tmp"
        shutil.copyfile(temp / "probe_queries.txt", query_tmp)
        os.replace(binary_tmp, output)
        os.replace(manifest_tmp, Path(str(output) + ".manifest.json"))
        os.replace(query_tmp, query_output)
        return manifest


def lexical_source_metadata(rime_dir: Path) -> list[dict[str, object]]:
    path = rime_dir / LEXICAL_SOURCE_NAME
    if not path.is_file():
        raise FileNotFoundError(f"required C2 lexical source missing: {path}")
    return [{"name": LEXICAL_SOURCE_NAME, "sha256": sha256_file(path), "bytes": path.stat().st_size}]


def configure_lexical_database(path: Path, max_memory_mb: int) -> sqlite3.Connection:
    connection = sqlite3.connect(path)
    cache_kib = max(32 * 1024, min(max_memory_mb * 1024 // 2, 256 * 1024))
    connection.execute("PRAGMA journal_mode=OFF")
    connection.execute("PRAGMA synchronous=OFF")
    connection.execute("PRAGMA temp_store=FILE")
    connection.execute(f"PRAGMA cache_size=-{cache_kib}")
    connection.execute("PRAGMA locking_mode=EXCLUSIVE")
    connection.executescript(
        """
        CREATE TABLE lexical (
            text TEXT PRIMARY KEY,
            weight REAL NOT NULL,
            weight_bits INTEGER NOT NULL,
            is_char INTEGER NOT NULL
        ) WITHOUT ROWID;
        """
    )
    return connection


def iter_lexical_rows(path: Path) -> Iterator[tuple[str, float]]:
    """Yield the rows accepted by frozen LexicalPriorStore, preserving source order."""
    in_body = False
    with path.open("r", encoding="utf-8", newline=None) as stream:
        for raw_line in stream:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if line == "...":
                in_body = True
                continue
            if not in_body:
                continue
            parts = parse_dict_line(line)
            if len(parts) < 2:
                continue
            text = clean_text(parts[0])
            if not text or not has_cjk(text):
                continue
            # Swift Dictionary/String equality is canonical-equivalence aware.  A byte-sorted mmap
            # index is not, so canonicalize the stored key while preserving observable lookup/count
            # behavior for canonically equivalent source spellings.
            text = unicodedata.normalize("NFC", text)
            weight = parse_weight(parts[1:])
            yield text, 1.0 if weight is None else weight


def insert_lexical_source(
    connection: sqlite3.Connection, source: Path, limit: int
) -> tuple[int, float]:
    normalized_limit = max(0, limit)
    if normalized_limit == 0:
        return 0, 1.0
    upsert = """
        INSERT INTO lexical(text, weight, weight_bits, is_char)
        VALUES (?, ?, ?, ?)
        ON CONFLICT(text) DO UPDATE SET
          weight=excluded.weight,
          weight_bits=excluded.weight_bits,
          is_char=excluded.is_char
        WHERE excluded.weight >= lexical.weight
    """
    parsed_rows = 0
    max_weight = 1.0
    pending = 0
    connection.execute("BEGIN")
    for text, parsed_weight in iter_lexical_rows(source):
        # Swift's max(lhs, rhs) returns rhs when the values compare equal.  That distinction is
        # observable for signed zero, so Python's max (which keeps lhs) is not bit-equivalent.
        # The >= upsert above likewise lets a later equal-weight row replace the prior bits.
        stored_weight = 0.0 if 0.0 > parsed_weight else parsed_weight
        bits = struct.unpack("<Q", struct.pack("<d", stored_weight))[0]
        sqlite_bits = bits if bits < (1 << 63) else bits - (1 << 64)
        connection.execute(
            upsert,
            (text, stored_weight, sqlite_bits, 1 if grapheme_count(text) == 1 else 0),
        )
        max_weight = max_weight if max_weight > parsed_weight else parsed_weight
        parsed_rows += 1
        pending += 1
        if pending >= 50_000:
            connection.commit()
            connection.execute("BEGIN")
            pending = 0
        if parsed_rows >= normalized_limit:
            break
    connection.commit()
    return parsed_rows, max_weight


def write_lexical_index(
    connection: sqlite3.Connection, path: Path, *, char_only: bool
) -> int:
    predicate = "WHERE lexical.is_char = 1" if char_only else ""
    query = f"""
        SELECT offsets.off, offsets.len, lexical.weight_bits
        FROM lexical
        JOIN lexical_text_offsets AS offsets ON offsets.value = lexical.text
        {predicate}
        ORDER BY lexical.text COLLATE BINARY ASC
    """
    count = 0
    with path.open("wb") as stream:
        for text_off, text_len, weight_bits in connection.execute(query):
            stream.write(struct.pack(
                "<IHHQ",
                int(text_off),
                int(text_len),
                0,
                int(weight_bits) & ((1 << 64) - 1),
            ))
            count += 1
    return count


def base36(value: int) -> str:
    alphabet = "0123456789abcdefghijklmnopqrstuvwxyz"
    if value == 0:
        return "0"
    digits: list[str] = []
    while value > 0:
        value, remainder = divmod(value, len(alphabet))
        digits.append(alphabet[remainder])
    return "".join(reversed(digits))


def write_lexical_probe_queries(
    connection: sqlite3.Connection,
    path: Path,
    rows: int = LEXICAL_PROBE_ROWS,
) -> dict[str, object]:
    if rows <= 0:
        raise ValueError("lexical probe row count must be positive")
    phrases = [
        str(row[0])
        for row in connection.execute("SELECT text FROM lexical ORDER BY text COLLATE BINARY ASC")
    ]
    chars = [
        str(row[0])
        for row in connection.execute(
            "SELECT text FROM lexical WHERE is_char = 1 ORDER BY text COLLATE BINARY ASC"
        )
    ]
    # The tables are bounded by --limit (50k by default); these two compact key lists keep the
    # 100k sidecar generation deterministic without materializing any source file or row payload.
    random.Random(20260710).shuffle(phrases)
    random.Random(20260711).shuffle(chars)
    phrase_set = set(phrases)
    class_counts = {"exact_phrase": 0, "char_composite": 0, "miss": 0}
    exact_index = 0
    char_index = 0
    miss_index = 0
    collision_escapes = 0
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        for index in range(rows):
            query_class = index % 5
            if query_class <= 2 and phrases:
                query = phrases[exact_index % len(phrases)]
                exact_index += 1
                class_counts["exact_phrase"] += 1
            elif query_class == 3 and chars:
                left = chars[(2 * char_index) % len(chars)]
                right = chars[(2 * char_index + 1) % len(chars)]
                char_index += 1
                query = left + right
                # This class must exercise the character-average path without a phrase hit.
                while query in phrase_set:
                    query = left + ("\ue000" * (collision_escapes + 1)) + right
                    collision_escapes += 1
                class_counts["char_composite"] += 1
            else:
                # Source rows require at least one CJK scalar, so PUA + ASCII is guaranteed absent
                # from both phrase and single-character maps while remaining valid UTF-8 text.
                query = "\ue000" + base36(miss_index)
                miss_index += 1
                class_counts["miss"] += 1
            stream.write(query + "\n")
    return {
        "name": path.name,
        "rows": rows,
        "classes": class_counts,
        "seed": 20260710,
        "phrase_keys": len(phrases),
        "char_keys": len(chars),
        "exact_unique_keys_covered": min(class_counts["exact_phrase"], len(phrases)),
        "char_composite_collision_escapes": collision_escapes,
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def build_lexical(
    rime_dir: Path, output: Path, max_memory_mb: int, limit: int
) -> dict[str, object]:
    normalized_limit = max(0, limit)
    sources = lexical_source_metadata(rime_dir)
    aggregate_sha = source_aggregate_sha(sources)
    source_path = rime_dir / LEXICAL_SOURCE_NAME
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".lexical-store-build-", dir=output.parent) as temp_text:
        temp = Path(temp_text)
        connection = configure_lexical_database(temp / "build.sqlite3", max_memory_mb)
        try:
            parsed_rows, max_weight = insert_lexical_source(
                connection, source_path, normalized_limit
            )
            text_blob_bytes, text_string_count = write_pool_and_offsets(
                connection,
                temp / "text.blob",
                "lexical_text_offsets",
                "SELECT text FROM lexical ORDER BY text COLLATE BINARY ASC",
            )
            phrase_count = write_lexical_index(
                connection, temp / "phrase_index.bin", char_only=False
            )
            char_count = write_lexical_index(
                connection, temp / "char_index.bin", char_only=True
            )
            phrase_hash_slots = write_u24_hash_index(
                connection,
                temp / "phrase_hash.bin",
                "SELECT text FROM lexical ORDER BY text COLLATE BINARY ASC",
                phrase_count,
            )
            char_hash_slots = write_u24_hash_index(
                connection,
                temp / "char_hash.bin",
                "SELECT text FROM lexical WHERE is_char = 1 ORDER BY text COLLATE BINARY ASC",
                char_count,
            )
            query_info = write_lexical_probe_queries(
                connection, temp / "probe_queries.txt"
            )
            query_info["name"] = "lexical_probe_queries.v1.txt"
        finally:
            connection.close()

        sections_in_order = (
            (
                "phrase_hash", temp / "phrase_hash.bin", phrase_hash_slots,
                FALLBACK_HASH_SLOT_BYTES,
            ),
            ("phrase_index", temp / "phrase_index.bin", phrase_count, LEXICAL_INDEX_RECORD_BYTES),
            (
                "char_hash", temp / "char_hash.bin", char_hash_slots,
                FALLBACK_HASH_SLOT_BYTES,
            ),
            ("char_index", temp / "char_index.bin", char_count, LEXICAL_INDEX_RECORD_BYTES),
            ("text_blob", temp / "text.blob", text_blob_bytes, 1),
        )
        offset = HEADER_BYTES
        section_manifest: dict[str, dict[str, int]] = {}
        payload_digest = hashlib.sha256()
        for name, section_path, count, record_bytes in sections_in_order:
            size = section_path.stat().st_size
            section_manifest[name] = {
                "offset": offset,
                "bytes": size,
                "count": int(count) if record_bytes != 1 else size,
                "record_bytes": record_bytes,
            }
            with section_path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    payload_digest.update(chunk)
            offset += size
        file_size = offset
        payload_sha = payload_digest.hexdigest()
        log_denominator = math.log1p(max_weight)
        log_denominator_bits = struct.unpack("<Q", struct.pack("<d", log_denominator))[0]
        max_weight_bits = struct.unpack("<Q", struct.pack("<d", max_weight))[0]
        builder_path = Path(__file__).resolve()
        builder_implementation_sha = sha256_file(builder_path)

        header = bytearray(HEADER_BYTES)
        header[:8] = MAGIC
        struct.pack_into(
            "<IIIIQ",
            header,
            8,
            FORMAT_VERSION,
            ENDIAN_MARKER,
            KIND_LEXICAL,
            HEADER_BYTES,
            file_size,
        )
        header[32:64] = bytes.fromhex(payload_sha)
        header[64:96] = bytes.fromhex(aggregate_sha)
        struct.pack_into(
            "<10Q",
            header,
            96,
            section_manifest["phrase_index"]["offset"],
            phrase_count,
            section_manifest["char_index"]["offset"],
            char_count,
            section_manifest["text_blob"]["offset"],
            text_blob_bytes,
            parsed_rows,
            normalized_limit,
            log_denominator_bits,
            max_weight_bits,
        )
        header[176:208] = bytes.fromhex(builder_implementation_sha)
        struct.pack_into(
            "<4Q",
            header,
            208,
            section_manifest["phrase_hash"]["offset"], phrase_hash_slots,
            section_manifest["char_hash"]["offset"], char_hash_slots,
        )

        binary_tmp = temp / "output.aqds"
        with binary_tmp.open("wb") as stream:
            stream.write(header)
            for _, section_path, _, _ in sections_in_order:
                copy_stream(section_path, stream)
            stream.flush()
            os.fsync(stream.fileno())
        if binary_tmp.stat().st_size != file_size:
            raise RuntimeError("assembled lexical binary size differs from header")

        manifest: dict[str, object] = {
            "format": FORMAT_NAME,
            "version": FORMAT_VERSION,
            "endian": "little",
            "kind": "lexical",
            "builder": {
                "name": BUILDER_NAME,
                "version": BUILDER_VERSION,
                "implementation_sha256": builder_implementation_sha,
                "python": sys.version.split()[0],
                "sqlite": sqlite3.sqlite_version,
                "unicode": unicodedata.unidata_version,
                "regex": grapheme_regex.__version__,
            },
            "sources": sources,
            "source_aggregate_sha256": aggregate_sha,
            "payload_sha256": payload_sha,
            "binary_sha256": sha256_file(binary_tmp),
            "binary_bytes": binary_tmp.stat().st_size,
            "limit": normalized_limit,
            "log_denominator_bits_hex": f"{log_denominator_bits:016x}",
            "max_weight_bits_hex": f"{max_weight_bits:016x}",
            "sections": section_manifest,
            "counts": {
                "parsed_rows": parsed_rows,
                "phrase_keys": phrase_count,
                "char_keys": char_count,
                "text_strings": text_string_count,
                "phrase_hash_slots": phrase_hash_slots,
                "char_hash_slots": char_hash_slots,
            },
            "hash_index": {
                "algorithm": "fnv1a64-linear-u20-fp4-v1",
                "empty_slot": 0,
                "stored_value": "high4_fingerprint_and_sorted_u20_index_plus_one",
                "max_load": "7/8",
            },
            "index_record": {
                "bytes": LEXICAL_INDEX_RECORD_BYTES,
                "layout": "u32_text_offset,u16_text_length,u16_reserved_zero,u64_weight_bits",
            },
            "probe_queries": query_info,
        }
        final_sources = lexical_source_metadata(rime_dir)
        if final_sources != sources:
            raise RuntimeError("lexical source changed during compact build; refusing publication")
        manifest_bytes = (
            json.dumps(manifest, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
        ).encode("utf-8")
        manifest_tmp = temp / "output.manifest.json"
        with manifest_tmp.open("wb") as stream:
            stream.write(manifest_bytes)
            stream.flush()
            os.fsync(stream.fileno())

        query_output = Path(str(output) + ".probe_queries.txt")
        query_tmp = temp / "query-output.tmp"
        shutil.copyfile(temp / "probe_queries.txt", query_tmp)
        os.replace(binary_tmp, output)
        os.replace(manifest_tmp, Path(str(output) + ".manifest.json"))
        os.replace(query_tmp, query_output)
        return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kind", choices=("fallback", "lexical"), required=True)
    parser.add_argument("--rime-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-memory-mb", type=int, default=512)
    parser.add_argument("--limit", type=int, default=50_000)
    args = parser.parse_args()
    if args.max_memory_mb < 128:
        parser.error("--max-memory-mb must be >=128")
    if args.output.resolve().is_relative_to((Path.home() / ".aquatype/production").resolve()):
        parser.error("refusing to write directly under ~/.aquatype/production")
    try:
        if args.kind == "fallback":
            manifest = build_fallback(args.rime_dir, args.output, args.max_memory_mb)
        else:
            manifest = build_lexical(
                args.rime_dir, args.output, args.max_memory_mb, args.limit
            )
    except Exception as error:
        print(f"dict compact build failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps({
        "ok": True,
        "kind": args.kind,
        "binary_bytes": manifest["binary_bytes"],
        "binary_sha256": manifest["binary_sha256"],
        "counts": manifest["counts"],
    }, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
