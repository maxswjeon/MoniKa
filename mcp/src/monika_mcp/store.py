from __future__ import annotations

import re
import sqlite3
from contextlib import closing
from dataclasses import dataclass
from pathlib import Path, PureWindowsPath
from typing import Any

CHAT_PREFIX = "chat_data\\"
IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def json_value(value: Any) -> Any:
    """Keep database values JSON-safe without guessing how BLOBs are encoded."""
    if isinstance(value, bytes):
        return {"encoding": "hex", "data": value.hex()}
    return value


@dataclass(frozen=True)
class Room:
    id: str
    available: bool
    last_change: int | None = None


class MonikaStore:
    """Read-only view of MoniKa's key cache and KakaoTalk databases.

    DEKs are intentionally private to this class and are never included in MCP
    responses or exceptions.
    """

    def __init__(self, cache_db: Path, user_dir: Path):
        self.cache_db = cache_db
        self.user_dir = user_dir.resolve()

    def _cache(self) -> sqlite3.Connection:
        return sqlite3.connect(f"file:{self.cache_db.as_posix()}?mode=ro", uri=True)

    def rooms(self) -> list[Room]:
        tags: dict[str, int] = {}
        keys: set[str] = set()
        with closing(self._cache()) as db:
            keys = {str(r[0]) for r in db.execute("SELECT rel FROM dek")}
            tags = {str(r[0]): int(r[1]) for r in db.execute("SELECT rel,last_change FROM tag") if r[1] is not None}
        disk = set()
        root = self.user_dir / "chat_data"
        if root.is_dir():
            disk = {str(p.relative_to(self.user_dir)).replace("/", "\\") for p in root.rglob("*.edb")}
        ids = sorted(disk | {p for p in keys | tags.keys() if p.lower().startswith(CHAT_PREFIX)})
        return [Room(p, p in keys and p in disk, tags.get(p)) for p in ids]

    def _key_and_path(self, room_id: str) -> tuple[str, Path]:
        # PureWindowsPath also rejects POSIX tricks consistently on Windows.
        rel = PureWindowsPath(room_id)
        if rel.is_absolute() or ".." in rel.parts or not room_id.lower().startswith(CHAT_PREFIX):
            raise ValueError("invalid room_id")
        with closing(self._cache()) as db:
            row = db.execute("SELECT dek_hex FROM dek WHERE rel=?", (room_id,)).fetchone()
        if not row:
            raise PermissionError("chatroom is locked (no captured DEK)")
        path = (self.user_dir / Path(*rel.parts)).resolve()
        if self.user_dir not in path.parents or not path.is_file():
            raise FileNotFoundError("chatroom database is unavailable")
        return str(row[0]), path

    @staticmethod
    def _message_layout(db: Any) -> tuple[str, str, str | None]:
        tables = [r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")]
        preferred = [t for t in tables if t.lower() in {"chatlog", "chatlogs", "chatloghistory"}]
        for table in preferred + tables:
            if not IDENTIFIER.fullmatch(table):
                continue
            columns = [r[1] for r in db.execute(f'PRAGMA table_info("{table}")')]
            time_col = next((c for c in ("send_at", "created_at", "timestamp", "time") if c in columns), None)
            id_col = next((c for c in ("id", "logId", "log_id") if c in columns), None)
            if time_col:
                return table, time_col, id_col
        raise RuntimeError("no supported message table/timestamp column found")

    def _room_db(self, room_id: str):
        key, path = self._key_and_path(room_id)
        try:
            from sqlcipher3 import dbapi2 as sqlcipher
        except ImportError as exc:  # pragma: no cover - installation error
            raise RuntimeError("sqlcipher3 is required") from exc
        db = sqlcipher.connect(f"file:{path.as_posix()}?mode=ro", uri=True)
        db.row_factory = sqlcipher.Row
        db.execute(f"PRAGMA key=\"x'{key}'\"")
        db.execute("PRAGMA cipher_page_size=4096")
        db.execute("PRAGMA query_only=ON")
        db.execute("SELECT count(*) FROM sqlite_master").fetchone()
        return db

    def messages_after(self, room_id: str, after: int, limit: int = 100) -> list[dict[str, Any]]:
        limit = max(1, min(int(limit), 500))
        with closing(self._room_db(room_id)) as db:
            table, time_col, id_col = self._message_layout(db)
            order = f'"{time_col}" ASC' + (f', "{id_col}" ASC' if id_col else "")
            rows = db.execute(
                f'SELECT * FROM "{table}" WHERE "{time_col}" > ? ORDER BY {order} LIMIT ?',
                (int(after), limit),
            ).fetchall()
            return [{k: json_value(row[k]) for k in row.keys()} for row in rows]

    def has_new_messages(self, room_id: str, after: int) -> dict[str, Any]:
        with closing(self._room_db(room_id)) as db:
            table, time_col, _ = self._message_layout(db)
            row = db.execute(
                f'SELECT COUNT(*), MAX("{time_col}") FROM "{table}" WHERE "{time_col}" > ?', (int(after),)
            ).fetchone()
            return {"has_new": bool(row[0]), "count": int(row[0]), "latest_time": row[1]}
