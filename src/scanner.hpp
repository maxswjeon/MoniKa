// scanner.hpp - scan a live process's private heap for anchored SQLCipher DEKs.
#pragma once
#include "common.hpp"
#include "oracle.hpp"
#include <functional>

namespace kw {

// Scans process `pid`: for every private, committed, readable region, finds the
// 0x88 anchor records and verifies the following 32 bytes as a DEK via `oracle`.
// Calls `onHit` for each verified DEK. Returns number of DEKs found (deduped by rel).
class Scanner {
  public:
    // onHit(hit, virtualAddress). Return value ignored.
    using HitFn = std::function<void(const DekHit&, uintptr_t)>;
    static int scan_process(DWORD pid, Oracle& oracle, const HitFn& onHit);
};

} // namespace kw
