# KakaoTalk DEK Watcher

[한국어](README.ko.md) | [English](README.en.md)

`kakao_watcher`는 본인이 소유하고 사용하는 Windows PC와 KakaoTalk 계정의 로컬 데이터베이스 암호화 키(DEK)를 포착하고, `chat_data` 변경을 감시하는 개인용 도구입니다.

이 프로젝트는 카카오 원격 서버, 비공개 프로토콜 또는 비공식 API에 접속하지 않습니다. 공식 KakaoTalk Windows 앱의 로컬 프로세스 메모리와 사용자 프로필 아래의 로컬 파일만 읽으며, 수집한 정보는 사용자 PC 밖으로 전송하지 않습니다.

> [!IMPORTANT]
> 이 프로젝트는 주식회사 카카오가 개발·배포·승인·인증·보증·지원한 제품이 아닙니다. KakaoTalk, 카카오톡 및 관련 표지는 각 권리자의 상표 또는 자산입니다. 이 도구는 본인이 정당하게 접근할 권한이 있는 계정, 기기 및 데이터에만 사용하십시오. 사용하기 전에 [면책 및 책임 제한 고지](DISCLAIMER.md)를 읽으십시오.

## 현재 제공하는 기능

- 실시간 `Microsoft-Windows-Kernel-Process` ETW 이벤트로 `KakaoTalk.exe` 시작을 감지합니다.
- 이미 실행 중인 KakaoTalk 프로세스도 시작 시 검색합니다.
- KakaoTalk 프로세스 메모리에서 현재 열린 데이터베이스의 SQLCipher DEK 후보를 찾고 BCrypt AES-256 page-1 oracle로 검증합니다.
- 검증된 DEK를 `%LOCALAPPDATA%\kakao_watcher\cache.db`에 저장합니다.
- `ReadDirectoryChangesW`로 `chat_data` 아래의 `.edb` 및 `.edb-wal` 변경을 재귀적으로 감시합니다.
- 트레이 아이콘에서 포착된 DEK 수를 표시하고 수동 재검색과 종료 메뉴를 제공합니다.
- `%LOCALAPPDATA%\kakao_watcher\watcher.log`에 실행 로그를 기록합니다.

## 아직 제공하지 않는 기능

새 메시지 알림은 아직 구현되지 않았습니다. 현재의 파일 변경 이벤트는 데이터베이스 또는 WAL 파일이 변경되었다는 사실만 나타내며, 새 메시지가 도착했다는 뜻은 아닙니다.

실제 메시지 알림을 구현하려면 포착한 DEK로 변경된 데이터베이스와 WAL 페이지를 복호화하고, `chatLogHistory`의 새 행을 이전 상태와 비교한 뒤 Windows 알림을 생성해야 합니다.

## 지원 환경

- Windows 10 또는 Windows 11 x64
- Windows용 KakaoTalk
- Visual Studio 2022 C++ Build Tools (`v143` toolset)
- 관리자 권한 권장

관리자 권한은 ETW 커널 공급자와 다른 프로세스 메모리 읽기에 필요합니다. 실행 시 UAC 승인을 요청하며, 사용자가 취소하거나 승격에 실패하면 프로그램은 제한된 기능으로 계속 실행됩니다.

## 빌드

Visual Studio의 **x64 Native Tools Command Prompt**에서 다음을 실행합니다.

```bat
build.bat
```

또는 MSBuild를 직접 사용할 수 있습니다.

```bat
msbuild kakao_watcher.sln /p:Configuration=Release /p:Platform=x64
```

결과 파일은 `x64\Release\kakao_watcher.exe`입니다. 외부 패키지 의존성은 없으며 SQLite는 Windows의 `winsqlite3.dll`, AES는 CNG/BCrypt를 사용합니다.

## 실행

`x64\Release\kakao_watcher.exe`를 더블 클릭합니다. 프로그램은 콘솔 창 없이 시스템 트레이에서 실행됩니다.

시작 시 관리자 권한 승격을 요청합니다. 승격에 성공하면 같은 경로에서 실행 중인 비관리자 인스턴스를 종료하고 관리자 인스턴스로 교체합니다. UAC를 취소하면 현재 프로세스가 비관리자 모드로 계속 실행됩니다. 이미 관리자 인스턴스가 실행 중이면 새 인스턴스는 중복 실행하지 않습니다.

트레이 아이콘이 보이지 않으면 Windows 알림 영역의 숨겨진 아이콘 메뉴를 확인하십시오. 문제를 진단할 때는 `%LOCALAPPDATA%\kakao_watcher\watcher.log`를 확인하십시오.

## 동작 구조

1. ETW가 KakaoTalk 시작을 감지하거나 Toolhelp가 실행 중인 프로세스를 찾습니다.
2. 시작 직후 여러 차례 가벼운 메모리 후보 수집을 실행하여 일시적으로 존재하는 DEK 후보를 보관합니다.
3. 각 후보가 실제 `.edb` page-1을 복호화하는지 검증하고 성공한 키만 로컬 캐시에 저장합니다.
4. `chat_data` 변경을 감시하고, 아직 키가 없는 방의 데이터베이스가 변경되면 후보 수집과 검증을 다시 예약합니다.
5. 트레이 툴팁에 현재 사용 가능한 DEK 수를 표시합니다.

## 개인정보와 로컬 파일

이 프로젝트가 다루는 KakaoTalk DB 경로, 암호화 키, 캐시 DB 및 로그는 민감 정보입니다.

- 실제 DEK, DB, 로그 또는 사용자 경로를 저장소에 커밋하지 마십시오.
- 실제 대화 내용, 채팅방 이름, 사용자 이름 또는 식별자를 테스트 fixture나 문서에 복사하지 마십시오.
- 자동화 테스트에는 합성 데이터만 사용하십시오.
- 로그와 캐시는 사용자 프로필 안에만 보관하고 불필요해지면 안전하게 삭제하십시오.
- 다른 사람의 계정, 기기 또는 데이터에 접근하는 용도로 사용하지 마십시오.

## 제한 사항

- 스캔 시점에 열린 연결의 DEK만 프로세스 메모리에 존재할 수 있습니다. 특정 채팅방의 키가 필요하면 KakaoTalk에서 해당 방을 연 뒤 트레이의 **Rescan now**를 실행하십시오.
- KakaoTalk 업데이트로 내부 codec record 구조가 바뀌면 현재 `0x88` anchor 탐지가 동작하지 않을 수 있습니다.
- 파일 변경 감지는 새 메시지, 읽음 상태 변경, 동기화, 체크포인트 등 변경 원인을 구분하지 않습니다.
- UAC를 거부한 경우 ETW 시작과 프로세스 메모리 읽기가 실패할 수 있습니다.
- KakaoTalk 또는 Windows의 향후 버전에서 계속 동작한다는 보장은 없습니다.

## 파일 구성

```text
kakao_watcher.sln / .vcxproj      MSBuild 프로젝트
build.bat                         명령행 빌드 스크립트
src/common.hpp                    공통 유틸리티, 로그, 상수
src/app.*                         초기화 및 작업 조정
src/etw.*                         KakaoTalk 프로세스 시작 감지
src/scanner.*                     프로세스 메모리 후보 검색
src/oracle.*                      SQLCipher page-1 키 검증
src/dek_cache.*                   로컬 SQLite DEK/태그 캐시
src/watcher.*                     chat_data 변경 감시
src/tray.*                        시스템 트레이 UI
src/main.cpp                      시작, 승격, 단일 인스턴스, 메시지 루프
```

## 면책

이 소프트웨어와 문서는 어떠한 보증 없이 **있는 그대로(AS IS)** 제공됩니다. 사용자는 자신의 계정·기기·데이터에 대한 접근 권한, 관련 법률과 KakaoTalk 약관 준수, 데이터 보호, 계정 제한 및 기타 결과에 대해 책임집니다. 자세한 내용은 [DISCLAIMER.md](DISCLAIMER.md)를 참조하십시오.

