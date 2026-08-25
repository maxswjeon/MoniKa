// oracle.cpp
#include "oracle.hpp"
#include <algorithm>
#pragma comment(lib, "bcrypt.lib")
namespace kw {

Oracle::~Oracle() {
    if (hAlg_)
        BCryptCloseAlgorithmProvider(hAlg_, 0);
}

bool Oracle::init() {
    if (BCryptOpenAlgorithmProvider(&hAlg_, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
        return false;
    if (BCryptSetProperty(hAlg_, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB),
                          0) != 0)
        return false;
    ULONG got = 0;
    if (BCryptGetProperty(hAlg_, BCRYPT_OBJECT_LENGTH, (PUCHAR)&keyObjLen_, sizeof(DWORD), &got, 0) != 0)
        return false;
    return true;
}

bool Oracle::ecb_decrypt(const uint8_t* key32, const uint8_t* in, size_t len, uint8_t* out) {
    vector<uint8_t> keyObj(keyObjLen_);
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (BCryptGenerateSymmetricKey(hAlg_, &hKey, keyObj.data(), keyObjLen_, (PUCHAR)key32, 32, 0) != 0)
        return false;
    ULONG res = 0;
    NTSTATUS s = BCryptDecrypt(hKey, (PUCHAR)in, (ULONG)len, nullptr, nullptr, 0, out, (ULONG)len, &res,
                               0); // ECB: no IV, no padding
    BCryptDestroyKey(hKey);
    return s == 0;
}

bool Oracle::add_edb(const wstring& full, const string& rel) {
    vector<uint8_t> head;
    if (!read_file_head(full, PAGE_SIZE, head) || head.size() < (size_t)PAGE_SIZE)
        return false;
    EdbEntry e;
    e.path = full;
    e.rel = rel;
    memcpy(e.c0, head.data() + 16, 16);
    for (int i = 0; i < RESERVED_NCAND; ++i) {
        int R = RESERVED_CANDS[i];
        memcpy(e.iv[i], head.data() + (PAGE_SIZE - R), 16);
    }
    std::lock_guard<std::mutex> lk(mu_);
    auto samePath = [&](const EdbEntry& old) { return to_lower(old.path) == to_lower(full); };
    auto it = std::find_if(files_.begin(), files_.end(), samePath);
    if (it != files_.end())
        *it = std::move(e);
    else
        files_.push_back(std::move(e));
    c0blob_.clear();
    return true;
}

void Oracle::rebuild_blob_() {
    c0blob_.resize(files_.size() * 16);
    for (size_t i = 0; i < files_.size(); ++i)
        memcpy(&c0blob_[i * 16], files_[i].c0, 16);
}

void Oracle::load_edb_dir(const wstring& userDir) {
    vector<wstring> stack{userDir};
    size_t added = 0;
    while (!stack.empty()) {
        wstring d = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do {
            wstring name = fd.cFileName;
            if (name == L"." || name == L"..")
                continue;
            wstring full = d + L"\\" + name;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                stack.push_back(full);
            } else if (iends_with(name, L".edb")) {
                string rel = wide_to_utf8(full.substr(userDir.size() + 1));
                if (add_edb(full, rel))
                    ++added;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    size_t total = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        rebuild_blob_();
        total = files_.size();
    }
    LOGI("oracle: loaded %zu .edb probe files", total);
}

vector<string> Oracle::relative_paths() const {
    std::lock_guard<std::mutex> lk(mu_);
    vector<string> paths;
    paths.reserve(files_.size());
    for (const auto& file : files_)
        paths.push_back(file.rel);
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::optional<DekHit> Oracle::test_key(const uint8_t* key32) {
    std::lock_guard<std::mutex> lk(mu_);
    if (files_.empty())
        return std::nullopt;
    if (c0blob_.size() != files_.size() * 16)
        rebuild_blob_();
    static thread_local vector<uint8_t> dd;
    dd.resize(c0blob_.size());
    if (!ecb_decrypt(key32, c0blob_.data(), c0blob_.size(), dd.data()))
        return std::nullopt;
    for (size_t i = 0; i < files_.size(); ++i) {
        const uint8_t* p = &dd[i * 16];
        const EdbEntry& e = files_[i];
        for (int r = 0; r < RESERVED_NCAND; ++r) {
            const uint8_t* iv = e.iv[r];
            int R = RESERVED_CANDS[r];
            if ((uint8_t)(p[4] ^ iv[4]) != R)
                continue; // cheapest discriminator
            uint8_t d0 = (uint8_t)(p[0] ^ iv[0]), d1 = (uint8_t)(p[1] ^ iv[1]);
            uint8_t d2 = (uint8_t)(p[2] ^ iv[2]), d3 = (uint8_t)(p[3] ^ iv[3]);
            uint8_t d5 = (uint8_t)(p[5] ^ iv[5]), d6 = (uint8_t)(p[6] ^ iv[6]), d7 = (uint8_t)(p[7] ^ iv[7]);
            if (d0 == 0x10 && d1 == 0x00 && d5 == 0x40 && d6 == 0x20 && d7 == 0x20 && (d2 == 1 || d2 == 2) &&
                (d3 == 1 || d3 == 2)) {
                DekHit hit;
                hit.rel = e.rel;
                hit.path = e.path;
                hit.reserved = R;
                memcpy(hit.key, key32, 32);
                return hit;
            }
        }
    }
    return std::nullopt;
}

} // namespace kw
