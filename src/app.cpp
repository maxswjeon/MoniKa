// app.cpp
#include "app.hpp"
#include "scanner.hpp"
#include <tlhelp32.h>

namespace kw {

// Find %LOCALAPPDATA%\Kakao\KakaoTalk\users\<40hex> that contains keystore.bin
static wstring discover_user_dir(){
    wstring base = env_var(L"LOCALAPPDATA");
    if(base.empty()) return L"";
    wstring users = base + L"\\Kakao\\KakaoTalk\\users";
    if(!dir_exists(users)) return L"";
    WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW((users+L"\\*").c_str(),&fd);
    if(h==INVALID_HANDLE_VALUE) return L"";
    wstring best;
    do{
        if(!(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) continue;
        wstring name=fd.cFileName;
        if(name.size()!=40) continue;                 // 40-hex user hash
        wstring cand=users+L"\\"+name;
        if(dir_exists(cand) &&
           GetFileAttributesW((cand+L"\\keystore.bin").c_str())!=INVALID_FILE_ATTRIBUTES){
            best=cand; break;
        }
    } while(FindNextFileW(h,&fd));
    FindClose(h);
    return best;
}

bool App::init(){
    userDir_ = discover_user_dir();
    if(userDir_.empty()){ LOGE("could not find KakaoTalk user dir under %%LOCALAPPDATA%%"); return false; }
    chatDataDir_ = userDir_ + L"\\chat_data";
    LOGI("user dir: %s", wide_to_utf8(userDir_).c_str());

    wstring appdir = env_var(L"LOCALAPPDATA") + L"\\kakao_watcher";
    CreateDirectoryW(appdir.c_str(), nullptr);
    cacheDbPath_ = appdir + L"\\cache.db";

    if(!oracle_.init()){ LOGE("oracle/BCrypt init failed"); return false; }
    oracle_.load_edb_dir(userDir_);
    if(!cache_.init(cacheDbPath_)) return false;
    refresh_armed_();
    return true;
}

void App::refresh_armed_(){
    int n = cache_.count_armed();
    int prev = armed_.exchange(n);
    if(n!=prev && onArmedChanged) onArmedChanged(n);
}

void App::scan_pid(DWORD pid){
    std::lock_guard<std::mutex> lk(scanMu_);
    LOGI("scanning KakaoTalk pid=%lu ...", pid);
    int newly=0;
    Scanner::scan_process(pid, oracle_, [&](const DekHit& hit, uintptr_t va){
        bool isNew = cache_.put_dek(hit);
        if(isNew){ ++newly;
            LOGI("  armed DEK: %s (reserved=%d) @0x%llx", hit.rel.c_str(), hit.reserved,
                 (unsigned long long)va);
        }
    });
    LOGI("scan done: %d newly-armed", newly);
    refresh_armed_();
}

DWORD App::find_kakaotalk_pid(){
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap==INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe; pe.dwSize=sizeof(pe); DWORD pid=0;
    if(Process32FirstW(snap,&pe)){
        do{
            if(to_lower(pe.szExeFile)==KAKAO_IMAGE){ pid=pe.th32ProcessID; break; }
        } while(Process32NextW(snap,&pe));
    }
    CloseHandle(snap);
    return pid;
}

void App::rescan_running(){
    DWORD pid=find_kakaotalk_pid();
    if(pid) scan_pid(pid);
    else LOGI("KakaoTalk not running; nothing to scan");
}

void App::on_file_event(const wstring& fullPath, bool /*added*/){
    // derive rel path under userDir_
    if(fullPath.size()<=userDir_.size()+1) return;
    wstring low=to_lower(fullPath);
    bool is_edb = iends_with(low,L".edb");
    bool is_wal = iends_with(low,L".edb-wal");
    if(!is_edb && !is_wal) return;

    wstring edbFull = is_wal ? fullPath.substr(0, fullPath.size()-4) : fullPath; // strip -wal
    string rel = wide_to_utf8(edbFull.substr(userDir_.size()+1));

    cache_.touch_tag(rel, "changed");

    // New/unknown room changed -> its connection is likely open now: try to grab its DEK.
    if(is_edb && !cache_.has_dek(rel)){
        // make sure the oracle knows this file (it may be brand new)
        oracle_.add_edb(edbFull, rel);
        DWORD pid=find_kakaotalk_pid();
        if(pid){
            LOGI("new/unknown edb changed (%s) -> targeted rescan", rel.c_str());
            scan_pid(pid);
        }
    }
}

} // namespace kw
