# MoniKa

[한국어](README.ko.md) | [English](README.en.md)

`MoniKa` (Monitor + KakaoTalk) is a personal Windows utility that captures local database encryption keys (DEKs) and watches `chat_data` changes for a KakaoTalk account and PC that you own and use.

This project does not connect to Kakao remote servers, private protocols, or unofficial APIs. It only reads the local process memory of the official KakaoTalk Windows application and local files under the user's profile, and it does not transmit collected information off the user's PC.

> [!IMPORTANT]
> This is an independent open-source project. It is not developed, distributed, approved, certified, warranted, or supported by Kakao Corp. KakaoTalk and related marks are trademarks or property of their respective owners. Use this tool only with accounts, devices, and data you are legally authorized to access. Read the [Disclaimer and Limitation of Liability](DISCLAIMER.md) before use.

## Current features

- Detects `KakaoTalk.exe` startup through real-time `Microsoft-Windows-Kernel-Process` ETW events.
- Finds an already-running KakaoTalk process at watcher startup.
- Searches KakaoTalk process memory for SQLCipher DEK candidates belonging to currently open databases and validates them with a BCrypt AES-256 page-1 oracle.
- Stores validated DEKs in `%LOCALAPPDATA%\MoniKa\cache.db`.
- Recursively watches `.edb` and `.edb-wal` changes under `chat_data` using `ReadDirectoryChangesW`.
- Shows the number of captured DEKs in a tray icon and provides manual rescan and exit commands.
- Writes runtime diagnostics to `%LOCALAPPDATA%\MoniKa\watcher.log`.
- Provides the separately installable [`monika-mcp`](https://pypi.org/project/monika-mcp/) package for authenticated, read-only access to unlocked rooms and multi-instance routing.

## Not implemented yet

Native Windows new-message notifications are not implemented. A filesystem event by itself means only that a database or WAL file changed; it does not establish that a new message arrived.

The MCP sidecar can query decrypted message rows after a supplied timestamp, but it does not run a background notification service. Implementing native alerts still requires tracking row state and generating a Windows notification.

## Supported environment

- Windows 10 or Windows 11 x64
- KakaoTalk for Windows
- Visual Studio 2022 C++ Build Tools (`v143` toolset)
- Administrator access recommended

Administrator access is needed for the ETW kernel provider and cross-process memory reads. The application requests elevation at startup. If the user cancels UAC or elevation fails, it continues with reduced functionality.

## Build

Run the following from an **x64 Native Tools Command Prompt** for Visual Studio:

```bat
build.bat
```

You can also invoke MSBuild directly:

```bat
msbuild MoniKa.sln /p:Configuration=Release /p:Platform=x64
```

The output is `x64\Release\MoniKa.exe`. There are no third-party package dependencies: SQLite uses the Windows `winsqlite3.dll`, and AES uses CNG/BCrypt.

## Run

Double-click `x64\Release\MoniKa.exe`. The application runs in the system tray without a console window.

At startup, it requests administrator elevation. When elevation succeeds, the elevated instance stops a non-elevated instance running from the same executable path and replaces it. If UAC is cancelled, the current process continues non-elevated. If an elevated instance already exists, the new process exits instead of creating a duplicate.

If the icon is not visible, check the hidden-icons area of the Windows notification tray. For diagnostics, inspect `%LOCALAPPDATA%\MoniKa\watcher.log`.

## Why this does not use the legacy PRAGMA method

Earlier research on KakaoTalk for Windows described a scheme that generated a `pragma` value from device identifiers in the registry, combined it with the user ID to derive a shared AES-128 key and IV, and decrypted each `.edb` in 4,096-byte AES-CBC blocks. In this documentation, “the PRAGMA method” means that legacy device/user-derived key scheme, not SQLite's `PRAGMA` syntax in general.

That method targeted particular older KakaoTalk versions and database formats. Against the KakaoTalk builds tested by this project, keys and IVs produced by that formula did not recover the SQLite header of current `chatLogs_*.edb` files. Running the published legacy scripts unchanged, or merely searching a process dump for 88-character Base64 pragma candidates, is therefore insufficient for these databases. Kakao has not publicly documented the internal format transition, so this project does not claim an exact cutoff version or universal failure across every distribution.

Instead of recalculating the historical formula, `MoniKa` captures candidate 32-byte per-database DEKs that the running KakaoTalk process actually uses for open database connections. It validates each candidate by decrypting a real `.edb` page 1 according to the observed SQLCipher layout and stores only successful keys. The `DEK` used by this project is therefore not the same value as the device-derived `pragma` described by older publications.

References:

- [Digital forensic analysis of encrypted database files in instant messaging applications on Windows operating systems (2019)](https://doi.org/10.1016/j.diin.2019.01.011) — analysis of the PRAGMA-derived key and AES-128-CBC database encryption in an older Windows KakaoTalk release
- [Windows KakaoTalk database decryption analysis and implementation #1 (2024, Korean)](https://blog.system32.kr/304) — implementation deriving a key and IV from device information, a pragma value, and a user ID
- [kdevil2k/Kakaotalk_decDB](https://github.com/kdevil2k/Kakaotalk_decDB) — examples that search for an 88-character Base64 pragma and apply the legacy decryption scheme
- [KakaoTalk chat database decryption attempt on current Windows builds (2026, Korean)](https://devconq.tistory.com/143) — a report that legacy fixed-key and pragma combinations failed on a current installation, leading to direct recovery of the raw SQLCipher key

## How it works

1. ETW detects KakaoTalk startup, or Toolhelp finds an existing process.
2. A front-loaded series of inexpensive memory collection passes preserves transient DEK candidates shortly after startup.
3. Each candidate is tested against an actual `.edb` page 1, and only successful keys are saved to the local cache.
4. The application watches `chat_data`; when a database without a cached key changes, it schedules another collection and validation pass.
5. The tray tooltip displays the current number of usable DEKs.

## Privacy and local files

KakaoTalk database paths, encryption keys, the cache database, and logs handled by this project are sensitive information.

- Never commit real DEKs, databases, logs, or user paths to the repository.
- Never copy real conversation text, room names, user names, or identifiers into fixtures or documentation.
- Use synthetic data only in automated tests.
- Keep logs and caches inside the user profile and delete them safely when no longer needed.
- Do not use this project to access another person's account, device, or data.

## Limitations

- Only databases with an open connection may have their DEKs resident in process memory. To capture a particular room, open it in KakaoTalk and select **Rescan now** from the tray menu.
- A KakaoTalk update may change the internal codec-record layout and break the current `0x88` anchor detection.
- A file-change event cannot distinguish a new message from read-state updates, synchronization, checkpoints, or other writes.
- ETW startup and process-memory reads may fail when UAC elevation is declined.
- Continued operation on future KakaoTalk or Windows versions is not guaranteed.

## Repository layout

```text
MoniKa.sln / MoniKa.vcxproj       MSBuild project
build.bat                         command-line build script
src/common.hpp                    shared utilities, logging, and constants
src/app.*                         initialization and orchestration
src/etw.*                         KakaoTalk process-start detection
src/scanner.*                     process-memory candidate scan
src/oracle.*                      SQLCipher page-1 key validation
src/dek_cache.*                   local SQLite DEK/tag cache
src/watcher.*                     chat_data change watcher
src/tray.*                        system-tray UI
src/main.cpp                      startup, elevation, single-instance guard, and message loop
mcp/                              authenticated room-data MCP server and multi-instance router
tools/verify_dumps.py             parallel NumPy/offline dump verifier
```

## MCP access

Version `0.1.0` of [`monika-mcp`](https://pypi.org/project/monika-mcp/) is available from PyPI. It exposes authenticated, read-only Streamable HTTP tools to list rooms and DEK availability, read bounded decrypted message rows, and check for messages after a timestamp. The same package includes a router that probes and routes requests across multiple MoniKa instances.

Install the persistent commands with uv:

```powershell
uv tool install monika-mcp
monika-mcp
# Or run the multi-instance service:
monika-mcp-router
```

Raw DEKs are never returned through MCP. Both services bind to loopback by default, require scoped bearer authentication, and require TLS for non-loopback deployments. See [`mcp/README.md`](mcp/README.md) for required environment variables and security configuration.

## Offline dump verification

`tools/verify_dumps.py` tests full-memory x64 minidumps against the same private/readable-memory, anchor, entropy, AES-header, and reserved-byte rules used by MoniKa. It processes dumps concurrently, deduplicates candidates globally, and uses NumPy for bulk candidate and header comparisons. It never prints keys, database paths, or message content.

```powershell
uv sync --all-packages
uv run python tools\verify_dumps.py *_x64.dmp --workers 8 --json
uv run ruff check .
uv run pytest
```

## Disclaimer

The software and documentation are provided **AS IS**, without warranty of any kind. You are responsible for authorization to access the account, device, and data; compliance with applicable law and KakaoTalk terms; data protection; account restrictions; and all other consequences of use. See [DISCLAIMER.md](DISCLAIMER.md) for details.

## License

This project is distributed under the [MIT License](LICENSE). The warranty disclaimer and limitation of liability in the MIT License are supplemented by the [Disclaimer and Limitation of Liability](DISCLAIMER.md).
