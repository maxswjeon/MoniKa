# KakaoTalk DEK Watcher

[한국어](README.ko.md) | [English](README.en.md)

`kakao_watcher` is a personal Windows utility that captures local database encryption keys (DEKs) and watches `chat_data` changes for a KakaoTalk account and PC that you own and use.

This project does not connect to Kakao remote servers, private protocols, or unofficial APIs. It only reads the local process memory of the official KakaoTalk Windows application and local files under the user's profile, and it does not transmit collected information off the user's PC.

> [!IMPORTANT]
> This is an independent open-source project. It is not developed, distributed, approved, certified, warranted, or supported by Kakao Corp. KakaoTalk and related marks are trademarks or property of their respective owners. Use this tool only with accounts, devices, and data you are legally authorized to access. Read the [Disclaimer and Limitation of Liability](DISCLAIMER.md) before use.

## Current features

- Detects `KakaoTalk.exe` startup through real-time `Microsoft-Windows-Kernel-Process` ETW events.
- Finds an already-running KakaoTalk process at watcher startup.
- Searches KakaoTalk process memory for SQLCipher DEK candidates belonging to currently open databases and validates them with a BCrypt AES-256 page-1 oracle.
- Stores validated DEKs in `%LOCALAPPDATA%\kakao_watcher\cache.db`.
- Recursively watches `.edb` and `.edb-wal` changes under `chat_data` using `ReadDirectoryChangesW`.
- Shows the number of captured DEKs in a tray icon and provides manual rescan and exit commands.
- Writes runtime diagnostics to `%LOCALAPPDATA%\kakao_watcher\watcher.log`.

## Not implemented yet

New-message alerts are not implemented. A filesystem event currently means only that a database or WAL file changed; it does not establish that a new message arrived.

Implementing actual alerts requires decrypting changed database and WAL pages with the captured DEK, diffing new `chatLogHistory` rows against prior state, and generating a Windows notification.

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
msbuild kakao_watcher.sln /p:Configuration=Release /p:Platform=x64
```

The output is `x64\Release\kakao_watcher.exe`. There are no third-party package dependencies: SQLite uses the Windows `winsqlite3.dll`, and AES uses CNG/BCrypt.

## Run

Double-click `x64\Release\kakao_watcher.exe`. The application runs in the system tray without a console window.

At startup, it requests administrator elevation. When elevation succeeds, the elevated instance stops a non-elevated instance running from the same executable path and replaces it. If UAC is cancelled, the current process continues non-elevated. If an elevated instance already exists, the new process exits instead of creating a duplicate.

If the icon is not visible, check the hidden-icons area of the Windows notification tray. For diagnostics, inspect `%LOCALAPPDATA%\kakao_watcher\watcher.log`.

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
kakao_watcher.sln / .vcxproj      MSBuild project
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
```

## Disclaimer

The software and documentation are provided **AS IS**, without warranty of any kind. You are responsible for authorization to access the account, device, and data; compliance with applicable law and KakaoTalk terms; data protection; account restrictions; and all other consequences of use. See [DISCLAIMER.md](DISCLAIMER.md) for details.

## License

This project is distributed under the [MIT License](LICENSE). The warranty disclaimer and limitation of liability in the MIT License are supplemented by the [Disclaimer and Limitation of Liability](DISCLAIMER.md).
