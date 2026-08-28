import sqlite3
from pathlib import Path

import pytest
from monika_mcp.store import MonikaStore


def cache(path: Path):
    db = sqlite3.connect(path)
    db.executescript(
        "CREATE TABLE dek(rel TEXT PRIMARY KEY, dek_hex TEXT); CREATE TABLE tag(rel TEXT PRIMARY KEY,last_change INTEGER);"
    )
    return db


def test_rooms_report_file_and_key_availability(tmp_path):
    user = tmp_path / "user"
    room = user / "chat_data" / "chatLogs_1.edb"
    room.parent.mkdir(parents=True)
    room.write_bytes(b"synthetic")
    db = cache(tmp_path / "cache.db")
    db.execute("INSERT INTO dek VALUES(?,?)", ("chat_data\\chatLogs_1.edb", "00" * 32))
    db.execute("INSERT INTO tag VALUES(?,?)", ("chat_data\\chatLogs_1.edb", 123))
    db.commit()
    db.close()
    assert MonikaStore(tmp_path / "cache.db", user).rooms()[0].available is True


def test_room_id_cannot_escape_user_dir(tmp_path):
    db = cache(tmp_path / "cache.db")
    db.close()
    with pytest.raises(ValueError):
        MonikaStore(tmp_path / "cache.db", tmp_path).messages_after("chat_data\\..\\secret.edb", 0)


def test_reads_and_checks_encrypted_message_rows(tmp_path):
    from sqlcipher3 import dbapi2 as sqlcipher

    user = tmp_path / "user"
    room = user / "chat_data" / "chatLogs_2.edb"
    room.parent.mkdir(parents=True)
    key = "12" * 32
    encrypted = sqlcipher.connect(room)
    encrypted.execute(f"PRAGMA key=\"x'{key}'\"")
    encrypted.execute("CREATE TABLE chatLog(id INTEGER, send_at INTEGER, message TEXT, attachment BLOB)")
    encrypted.executemany("INSERT INTO chatLog VALUES(?,?,?,?)", [(1, 100, "old", b"a"), (2, 200, "new", b"b")])
    encrypted.commit()
    encrypted.close()

    db = cache(tmp_path / "cache.db")
    db.execute("INSERT INTO dek VALUES(?,?)", ("chat_data\\chatLogs_2.edb", key))
    db.commit()
    db.close()
    store = MonikaStore(tmp_path / "cache.db", user)
    assert store.messages_after("chat_data\\chatLogs_2.edb", 100)[0]["message"] == "new"
    assert store.messages_after("chat_data\\chatLogs_2.edb", 100)[0]["attachment"] == {"encoding": "hex", "data": "62"}
    assert store.has_new_messages("chat_data\\chatLogs_2.edb", 100) == {"has_new": True, "count": 1, "latest_time": 200}
