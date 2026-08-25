# KakaoTalk DEK Watcher

Self-use monitor for **your own** KakaoTalk account on **your own** PC. It captures the per-database SQLCipher DEKs that KakaoTalk keeps resident in its own process, caches them locally, and watches `chat_data` for changes so you can build alerting on top.

> Your machine, your account, your data. This does not attack a remote system, does
> not modify KakaoTalk's files, and stores nothing outside your user profile.

## The five pieces

1. **ETW trigger** — a real-time `Microsoft-Windows-Kernel-Process` consumer fires when `KakaoTalk.exe` starts, resolves its PID, and scans it (works when the watcher is started **before** KakaoTalk). An already-running instance is also caught on startup via Toolhelp.
2. **Catch DEKs on load** — best-effort `ReadProcessMemory` scan of the private heap using the `0x88` codec-record anchor + a BCrypt AES-256 SQLCipher page-1 oracle. Missing some is fine (only currently-open DBs have resident keys).
3. **chat_data watcher** — `ReadDirectoryChangesW` (recursive) tags every `.edb` / `.edb-wal` change; a new/unknown room's change triggers a targeted rescan.
4. **DEK cache** — program-local SQLite at `%LOCALAPPDATA%\kakao_watcher\cache.db` via the OS `winsqlite3.dll` (tables `dek`, `tag`). DEKs are stable, so a captured key works until the DB is recreated.
5. **Tray icon** — hover shows `kakao_watcher: N DEKs armed`; right-click for *Rescan now* / *Exit*.

## Build (VS Build Tools, no IDE)

From an **x64 Native Tools Command Prompt for VS**:

```shell
build.bat
```

or via MSBuild:

```shell
msbuild kakao_watcher.sln /p:Configuration=Release /p:Platform=x64
```

Output: `x64\Release\kakao_watcher.exe`. No third-party dependencies — SQLite via `winsqlite3.dll`, AES via CNG/BCrypt, ETW via `tdh.lib`.

## Run

Run **as Administrator** (the ETW kernel provider requires it):

```shell
x64\Release\kakao_watcher.exe
```

Logs go to stderr (console build). The cache DB and the tray tooltip show progress.

## How it fits an alerter

`kakao_watcher` is the *key-management* layer. To surface messages:

- On a `chatLogs_<id>.edb` / `-wal` change for a room whose DEK is cached, decrypt the changed pages (AES-256-CBC, page 4096, `reserved=80`, IV in the page's reserved tail) and diff the chatLogHistory` table for new rows.
- WAL frames carry the newest, not-yet-checkpointed messages (same DEK).

The decryption/diff/alert step is intentionally separate; `kakao_watcher` guarantees the DEK is available and current.

## Limitations

- Only DBs with an **open connection** at scan time have resident DEKs. To capture a specific room, open it in KakaoTalk (the watcher's targeted rescan then grabs it).
- The `0x88` anchor was verified on the current build; if a future build changes the codec record layout, adjust `ANCHOR_BYTE` / `ANCHOR_TAG_MAX` in `common.hpp`.

## Files

```text
kakao_watcher.sln / .vcxproj      MSBuild project
build.bat                         one-shot cl.exe build
src/common.hpp                    helpers + constants (0x88 anchor, reserved list)
src/etw.*                         Kernel-Process ETW consumer -> PID on start
src/scanner.*                     ReadProcessMemory + anchor scan
src/oracle.*                      BCrypt AES-256 SQLCipher page-1 verifier
src/watcher.*                     ReadDirectoryChangesW over chat_data
src/dek_cache.*                   winsqlite3 cache (dek + tag tables)
src/tray.*                        Shell_NotifyIcon tray + tooltip count
src/app.*                         orchestration + user-dir discovery
src/main.cpp                      wiring + message loop
```
