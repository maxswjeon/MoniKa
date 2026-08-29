// scanner.hpp - scan a live process's private heap for anchored SQLCipher DEKs.
#pragma once
#include "common.hpp"
#include "oracle.hpp"
#include <functional>

namespace kw {

struct SessionInfo {
    string state = "unknown";
    string hashedTalkUserId;
};

// Scans process `pid`: for every private, committed, readable region, finds the
// 0x88 anchor records and verifies the following 32 bytes as a DEK via `oracle`.
// Calls `onHit` for each verified DEK. Returns number of DEKs found (deduped by rel).
class Scanner {
  public:
    // onHit(hit, virtualAddress). Return value ignored.
    using HitFn = std::function<void(const DekHit&, uintptr_t)>;
    static int scan_process(DWORD pid, Oracle& oracle, const HitFn& onHit);

    // Storm mode: fast anchor-only pass, NO AES/validation. Calls onCand for every
    // 0x88-anchored, low-zero 32-byte window. Cheap enough to run repeatedly during
    // KakaoTalk's startup open-storm; validate the accumulated candidates later.
    using CandFn = std::function<void(const uint8_t* key32)>;
    static size_t collect_candidates(DWORD pid, const CandFn& onCand);

    // Extract login-scene markers and the cached self identity. Identity can
    // survive logout, so callers must disclose it only after confirming that
    // the returned session is signed in.
    static SessionInfo inspect_session(DWORD pid);
};

} // namespace kw
