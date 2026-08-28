// scanner.cpp
#include "scanner.hpp"
#include <set>
#include <array>
#include <algorithm>
namespace kw {

struct SecureKey32 {
    std::array<uint8_t, DEK_LEN> bytes{};
    SecureKey32() = default;
    SecureKey32(const SecureKey32&) = default;
    SecureKey32& operator=(const SecureKey32&) = default;
    ~SecureKey32() { SecureZeroMemory(bytes.data(), bytes.size()); }
    bool operator<(const SecureKey32& other) const { return bytes < other.bytes; }
};

static bool region_interesting(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Type != MEM_PRIVATE)
        return false; // heap/stack, skip images/mapped
    if (mbi.Protect & PAGE_GUARD)
        return false;
    DWORD p = mbi.Protect & 0xFF;
    return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

// Walk every private/committed/readable region; for each 0x88-anchored,
// low-zero 32-byte window at a verified codec-record offset, call perCand.
// The heavy validation (or not) is entirely up to perCand.
static size_t walk_anchored(HANDLE hp, const std::function<void(const uint8_t*, uintptr_t)>& perCand,
                            size_t* candCount) {
    uintptr_t addr = 0;
    const uintptr_t MAXADDR = (uintptr_t)0x7FFFFFFF0000ULL;
    MEMORY_BASIC_INFORMATION mbi;
    vector<uint8_t> buf;
    const size_t CHUNK = 1024 * 1024, OVERLAP = DEK_MAX_OFFSET + DEK_LEN + 1;
    size_t scanned = 0, cands = 0;

    while (addr < MAXADDR && VirtualQueryEx(hp, (LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        size_t size = mbi.RegionSize;
        if (base == 0 && size == 0)
            break;
        if (region_interesting(mbi) && size >= DEK_LEN + 2) {
            for (size_t pos = 0; pos < size; pos += CHUNK) {
                size_t readPos = pos ? pos - OVERLAP : 0;
                size_t want = std::min(CHUNK + (pos ? OVERLAP : 0), size - readPos);
                buf.resize(want);
                SIZE_T got = 0;
                ReadProcessMemory(hp, (LPCVOID)(base + readPos), buf.data(), want, &got);
                if (got >= DEK_LEN + 2) {
                    scanned += got;
                    const uint8_t* d = buf.data();
                    for (size_t i = 1; i + DEK_MAX_OFFSET + DEK_LEN <= got; ++i) {
                        if (d[i] != ANCHOR_BYTE)
                            continue;
                        if (d[i - 1] >= ANCHOR_TAG_MAX)
                            continue;
                        for (size_t oi = 0; oi < DEK_OFFSET_COUNT; ++oi) {
                            size_t keyOffset = DEK_OFFSETS[oi];
                            uintptr_t keyVa = base + readPos + i + keyOffset;
                            if (pos && keyVa < base + pos)
                                continue; // dedupe overlap
                            const uint8_t* key = &d[i + keyOffset];
                            int zeros = 0; // cheap entropy gate
                            for (int k = 0; k < (int)DEK_LEN; ++k)
                                zeros += (key[k] == 0);
                            if (zeros > 2)
                                continue;
                            ++cands;
                            perCand(key, keyVa);
                        }
                    }
                }
                SecureZeroMemory(buf.data(), buf.size());
            }
        }
        uintptr_t next = base + size;
        if (next <= addr)
            break;
        addr = next;
    }
    if (candCount)
        *candCount = cands;
    return scanned;
}

int Scanner::scan_process(DWORD pid, Oracle& oracle, const HitFn& onHit) {
    HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hp) {
        LOGE("OpenProcess(%lu) failed: %lu (need admin / same integrity)", pid, GetLastError());
        return 0;
    }
    std::set<string> seen;
    std::set<SecureKey32> testedKeys;
    int found = 0;
    size_t cands = 0;
    DWORD t0 = GetTickCount();
    size_t scanned = walk_anchored(
        hp,
        [&](const uint8_t* key, uintptr_t va) {
            SecureKey32 candidate;
            memcpy(candidate.bytes.data(), key, candidate.bytes.size());
            if (!testedKeys.insert(candidate).second)
                return;
            auto hit = oracle.test_key(key);
            if (hit && !seen.count(hit->rel)) {
                seen.insert(hit->rel);
                ++found;
                onHit(*hit, va);
            }
        },
        &cands);
    CloseHandle(hp);
    LOGI("scan pid=%lu: %d DEKs, %zu candidates over %.0f MB in %lu ms", pid, found, cands, scanned / 1e6,
         GetTickCount() - t0);
    return found;
}

size_t Scanner::collect_candidates(DWORD pid, const CandFn& onCand) {
    HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hp) {
        LOGE("OpenProcess(%lu) failed: %lu", pid, GetLastError());
        return 0;
    }
    size_t cands = 0;
    DWORD t0 = GetTickCount();
    size_t scanned = walk_anchored(hp, [&](const uint8_t* key, uintptr_t) { onCand(key); }, &cands);
    CloseHandle(hp);
    LOGI("storm collect pid=%lu: %zu anchored candidates over %.0f MB in %lu ms", pid, cands, scanned / 1e6,
         GetTickCount() - t0);
    return cands;
}

} // namespace kw
