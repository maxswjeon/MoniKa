// scanner.cpp
#include "scanner.hpp"
#include <set>
#include <array>
#include <algorithm>
#include <cctype>
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

static bool contains_bytes(const uint8_t* data, size_t size, const char* marker) {
    size_t length = strlen(marker);
    return length <= size && std::search(data, data + size, marker, marker + length) != data + size;
}

static string json_hex_field(const uint8_t* data, size_t size, const char* field) {
    string marker = string("\"") + field + "\":\"";
    const uint8_t* begin = std::search(data, data + size, marker.begin(), marker.end());
    if (begin == data + size)
        return {};
    begin += marker.size();
    const uint8_t* end = begin;
    while (end < data + size && std::isxdigit(static_cast<unsigned char>(*end)) && end - begin <= 64)
        ++end;
    if (end == data + size || *end != '"' || end - begin != 32)
        return {};
    return string(reinterpret_cast<const char*>(begin), reinterpret_cast<const char*>(end));
}

SessionInfo Scanner::inspect_session(DWORD pid) {
    SessionInfo info;
    HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hp)
        return info;

    const char* markers[] = {"S_LOGIN_100068", "login_img_line.png", "should_prevent_auto_login_once = no"};
    bool found[] = {false, false, false};
    uintptr_t addr = 0;
    const uintptr_t maxAddress = (uintptr_t)0x7FFFFFFF0000ULL;
    const size_t chunk = 1024 * 1024, overlap = 4096;
    MEMORY_BASIC_INFORMATION mbi;
    vector<uint8_t> buffer;

    while (addr < maxAddress && VirtualQueryEx(hp, (LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        size_t size = mbi.RegionSize;
        if (base == 0 && size == 0)
            break;
        if (region_interesting(mbi)) {
            for (size_t pos = 0; pos < size; pos += chunk) {
                size_t readPos = pos ? pos - overlap : 0;
                size_t want = std::min(chunk + (pos ? overlap : 0), size - readPos);
                buffer.resize(want);
                SIZE_T got = 0;
                if (!ReadProcessMemory(hp, (LPCVOID)(base + readPos), buffer.data(), want, &got) || !got)
                    continue;
                for (size_t i = 0; i < 3; ++i)
                    found[i] = found[i] || contains_bytes(buffer.data(), got, markers[i]);
                if (info.hashedTalkUserId.empty()) {
                    const char* drawerMarker = "drawerUserInfo{";
                    size_t markerLength = strlen(drawerMarker);
                    const uint8_t* drawer = std::search(buffer.data(), buffer.data() + got, drawerMarker,
                                                        drawerMarker + markerLength);
                    if (drawer != buffer.data() + got) {
                        size_t remaining = (size_t)(buffer.data() + got - drawer);
                        info.hashedTalkUserId = json_hex_field(drawer, std::min(remaining, (size_t)4096),
                                                               "hashedTalkUserId");
                    }
                }
            }
        }
        uintptr_t next = base + size;
        if (next <= addr)
            break;
        addr = next;
    }
    CloseHandle(hp);

    bool loginScene = found[0] && found[1];
    if (loginScene && found[2])
        info.state = "signed_out";
    else if (loginScene)
        info.state = "login_interstitial";
    else if (!found[2])
        info.state = "signed_in_candidate";
    return info;
}

} // namespace kw
