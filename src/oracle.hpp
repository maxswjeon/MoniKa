// oracle.hpp - SQLCipher DEK verification (BCrypt AES-256) + probe over .edb files.
#pragma once
#include "common.hpp"
#include <optional>
#include <bcrypt.h>

namespace kw {

struct EdbEntry {
    wstring path;                    // full path
    string  rel;                     // relative label (cache key)
    uint8_t c0[16];                  // ciphertext block 0 = page1[16:32]
    uint8_t iv[RESERVED_NCAND][16];  // per-reserved IV = page1[4096-R : +16]
};

struct DekHit { string rel; wstring path; int reserved; uint8_t key[32]; };

// Loads the page-1 heads of every .edb under a user dir and verifies candidate keys.
class Oracle {
public:
    ~Oracle();
    bool   init();                                 // opens BCrypt AES/ECB provider
    void   load_edb_dir(const wstring& userDir);   // recursively scan *.edb
    bool   add_edb(const wstring& fullPath, const string& rel); // one file
    size_t file_count() const { return files_.size(); }

    // Test a 32-byte key against ALL loaded files. Returns the match if any.
    std::optional<DekHit> test_key(const uint8_t* key32);

private:
    bool ecb_decrypt(const uint8_t* key32, const uint8_t* in, size_t len, uint8_t* out);
    void rebuild_blob_();
    vector<EdbEntry> files_;
    vector<uint8_t>  c0blob_;
    BCRYPT_ALG_HANDLE hAlg_ = nullptr;
    DWORD keyObjLen_ = 0;
    std::mutex mu_;
};

} // namespace kw
