// dek_cache.cpp - dynamic winsqlite3.dll binding + cache logic.
#include "dek_cache.hpp"

namespace kw {

// ---- minimal winsqlite3 dynamic binding ----
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
#define SQLITE_OK 0
#define SQLITE_ROW 100
#define SQLITE_DONE 101
#define SQLITE_TRANSIENT ((void (*)(void*)) - 1)

namespace sq {
typedef int (*open_t)(const char*, sqlite3**);
typedef int (*close_t)(sqlite3*);
typedef int (*exec_t)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
typedef int (*prepare_t)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
typedef int (*step_t)(sqlite3_stmt*);
typedef int (*finalize_t)(sqlite3_stmt*);
typedef int (*bind_text_t)(sqlite3_stmt*, int, const char*, int, void (*)(void*));
typedef int (*bind_int64_t)(sqlite3_stmt*, int, long long);
typedef int (*column_int_t)(sqlite3_stmt*, int);
typedef const unsigned char* (*column_text_t)(sqlite3_stmt*, int);
typedef const char* (*errmsg_t)(sqlite3*);
typedef void (*free_t)(void*);

open_t s_open = nullptr;
close_t s_close = nullptr;
exec_t s_exec = nullptr;
prepare_t s_prepare = nullptr;
step_t s_step = nullptr;
finalize_t s_finalize = nullptr;
bind_text_t s_bind_text = nullptr;
bind_int64_t s_bind_int64 = nullptr;
column_int_t s_column_int = nullptr;
column_text_t s_column_text = nullptr;
errmsg_t s_errmsg = nullptr;
free_t s_free = nullptr;
HMODULE module = nullptr;
bool loaded = false;

bool load() {
    if (loaded)
        return true;
    HMODULE h = LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) {
        LOGE("winsqlite3.dll not found (Windows 10+ required)");
        return false;
    }
    s_open = (open_t)GetProcAddress(h, "sqlite3_open");
    s_close = (close_t)GetProcAddress(h, "sqlite3_close");
    s_exec = (exec_t)GetProcAddress(h, "sqlite3_exec");
    s_prepare = (prepare_t)GetProcAddress(h, "sqlite3_prepare_v2");
    s_step = (step_t)GetProcAddress(h, "sqlite3_step");
    s_finalize = (finalize_t)GetProcAddress(h, "sqlite3_finalize");
    s_bind_text = (bind_text_t)GetProcAddress(h, "sqlite3_bind_text");
    s_bind_int64 = (bind_int64_t)GetProcAddress(h, "sqlite3_bind_int64");
    s_column_int = (column_int_t)GetProcAddress(h, "sqlite3_column_int");
    s_column_text = (column_text_t)GetProcAddress(h, "sqlite3_column_text");
    s_errmsg = (errmsg_t)GetProcAddress(h, "sqlite3_errmsg");
    s_free = (free_t)GetProcAddress(h, "sqlite3_free");
    loaded = s_open && s_close && s_exec && s_prepare && s_step && s_finalize && s_bind_text &&
             s_bind_int64 && s_column_int && s_column_text && s_errmsg && s_free;
    if (loaded)
        module = h;
    else
        FreeLibrary(h);
    if (!loaded)
        LOGE("winsqlite3.dll missing exports");
    return loaded;
}
} // namespace sq

DekCache::~DekCache() {
    if (db_)
        sq::s_close((sqlite3*)db_);
}

bool DekCache::exec(const char* sql) {
    char* err = nullptr;
    int rc = sq::s_exec((sqlite3*)db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        LOGE("sqlite exec failed rc=%d: %s", rc, err ? err : "?");
        if (err)
            sq::s_free(err);
        return false;
    }
    return true;
}

bool DekCache::init(const wstring& dbPath) {
    if (!sq::load())
        return false;
    string u8 = wide_to_utf8(dbPath);
    sqlite3* db = nullptr;
    if (sq::s_open(u8.c_str(), &db) != SQLITE_OK) {
        LOGE("sqlite open failed: %s", u8.c_str());
        if (db)
            sq::s_close(db);
        return false;
    }
    db_ = db;
    if (!exec("PRAGMA journal_mode=WAL;"))
        return false;
    bool ok = exec("CREATE TABLE IF NOT EXISTS dek("
                   " rel TEXT PRIMARY KEY, dek_hex TEXT NOT NULL, reserved INTEGER NOT NULL,"
                   " first_seen INTEGER, last_armed INTEGER);"
                   "CREATE TABLE IF NOT EXISTS tag("
                   " rel TEXT PRIMARY KEY, last_change INTEGER, change_count INTEGER DEFAULT 0,"
                   " state TEXT);");
    LOGI("cache: %s", u8.c_str());
    return ok;
}

bool DekCache::put_dek(const DekHit& hit) {
    std::lock_guard<std::mutex> lk(mu_);
    string kh = to_hex(hit.key, 32);
    int64_t now = now_unix();
    // returns true if this rel was not previously armed
    bool isNew = !has_dek_locked(hit.rel);
    const char* sql =
        "INSERT INTO dek(rel,dek_hex,reserved,first_seen,last_armed) VALUES(?,?,?,?,?) "
        "ON CONFLICT(rel) DO UPDATE SET dek_hex=excluded.dek_hex, reserved=excluded.reserved, "
        "last_armed=excluded.last_armed;";
    sqlite3_stmt* st = nullptr;
    if (sq::s_prepare((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        LOGE("sqlite prepare DEK failed: %s", sq::s_errmsg((sqlite3*)db_));
        return false;
    }
    int rc = sq::s_bind_text(st, 1, hit.rel.c_str(), -1, (void (*)(void*))SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sq::s_bind_text(st, 2, kh.c_str(), -1, (void (*)(void*))SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sq::s_bind_int64(st, 3, hit.reserved);
    if (rc == SQLITE_OK)
        rc = sq::s_bind_int64(st, 4, now);
    if (rc == SQLITE_OK)
        rc = sq::s_bind_int64(st, 5, now);
    if (rc == SQLITE_OK)
        rc = sq::s_step(st);
    sq::s_finalize(st);
    if (rc != SQLITE_DONE) {
        LOGE("sqlite store DEK failed rc=%d: %s", rc, sq::s_errmsg((sqlite3*)db_));
        return false;
    }
    return isNew;
}

bool DekCache::has_dek(const string& rel) {
    std::lock_guard<std::mutex> lk(mu_);
    return has_dek_locked(rel);
}

bool DekCache::has_dek_locked(const string& rel) {
    sqlite3_stmt* st = nullptr;
    if (sq::s_prepare((sqlite3*)db_, "SELECT 1 FROM dek WHERE rel=?;", -1, &st, nullptr) !=
        SQLITE_OK)
        return false;
    sq::s_bind_text(st, 1, rel.c_str(), -1, (void (*)(void*))SQLITE_TRANSIENT);
    bool found = (sq::s_step(st) == SQLITE_ROW);
    sq::s_finalize(st);
    return found;
}

int DekCache::count_armed() {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sq::s_prepare((sqlite3*)db_, "SELECT COUNT(*) FROM dek;", -1, &st, nullptr) != SQLITE_OK)
        return 0;
    int n = 0;
    if (sq::s_step(st) == SQLITE_ROW)
        n = sq::s_column_int(st, 0);
    sq::s_finalize(st);
    return n;
}

void DekCache::touch_tag(const string& rel, const char* state) {
    std::lock_guard<std::mutex> lk(mu_);
    int64_t now = now_unix();
    const char* sql = "INSERT INTO tag(rel,last_change,change_count,state) VALUES(?,?,1,?) "
                      "ON CONFLICT(rel) DO UPDATE SET last_change=excluded.last_change, "
                      "change_count=tag.change_count+1, state=excluded.state;";
    sqlite3_stmt* st = nullptr;
    if (sq::s_prepare((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        LOGE("sqlite prepare tag failed: %s", sq::s_errmsg((sqlite3*)db_));
        return;
    }
    int rc = sq::s_bind_text(st, 1, rel.c_str(), -1, (void (*)(void*))SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sq::s_bind_int64(st, 2, now);
    if (rc == SQLITE_OK)
        rc = sq::s_bind_text(st, 3, state, -1, (void (*)(void*))SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sq::s_step(st);
    sq::s_finalize(st);
    if (rc != SQLITE_DONE)
        LOGE("sqlite tag update failed rc=%d: %s", rc, sq::s_errmsg((sqlite3*)db_));
}

} // namespace kw
