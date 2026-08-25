// scanner.cpp
#include "scanner.hpp"
#include <set>
namespace kw {

static bool region_interesting(const MEMORY_BASIC_INFORMATION& mbi){
    if(mbi.State != MEM_COMMIT) return false;
    if(mbi.Type  != MEM_PRIVATE) return false;                 // heap/stack, skip images/mapped
    if(mbi.Protect & PAGE_GUARD) return false;
    DWORD p = mbi.Protect & 0xFF;
    return p==PAGE_READONLY || p==PAGE_READWRITE || p==PAGE_WRITECOPY ||
           p==PAGE_EXECUTE_READ || p==PAGE_EXECUTE_READWRITE || p==PAGE_EXECUTE_WRITECOPY;
}

int Scanner::scan_process(DWORD pid, Oracle& oracle, const HitFn& onHit){
    HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if(!hp){ LOGE("OpenProcess(%lu) failed: %lu (need admin / same integrity)", pid, GetLastError()); return 0; }

    std::set<string> seen;
    int found=0;
    uintptr_t addr=0;
    const uintptr_t MAXADDR = (uintptr_t)0x7FFFFFFF0000ULL;
    MEMORY_BASIC_INFORMATION mbi;
    vector<uint8_t> buf;
    DWORD t0 = GetTickCount();
    size_t scanned=0, cand_tested=0;

    while(addr < MAXADDR && VirtualQueryEx(hp,(LPCVOID)addr,&mbi,sizeof(mbi))==sizeof(mbi)){
        uintptr_t base=(uintptr_t)mbi.BaseAddress; size_t size=mbi.RegionSize;
        if(base==0 && size==0) break;
        if(region_interesting(mbi) && size>=DEK_LEN+2 && size<=(size_t)512*1024*1024){
            buf.resize(size);
            SIZE_T got=0;
            if(ReadProcessMemory(hp,(LPCVOID)base,buf.data(),size,&got) && got>=DEK_LEN+2){
                scanned+=got;
                const uint8_t* d=buf.data();
                // anchor: d[i]==0x88, d[i-1]<TAG_MAX  ->  candidate key at d[i+1 .. +32]
                for(size_t i=1; i+1+DEK_LEN<=got; ++i){
                    if(d[i]!=ANCHOR_BYTE) continue;
                    if(d[i-1]>=ANCHOR_TAG_MAX) continue;
                    const uint8_t* key=&d[i+1];
                    // cheap entropy gate: few zero bytes
                    int zeros=0; for(int k=0;k<(int)DEK_LEN;++k) zeros += (key[k]==0);
                    if(zeros>2) continue;
                    ++cand_tested;
                    auto hit=oracle.test_key(key);
                    if(hit && !seen.count(hit->rel)){
                        seen.insert(hit->rel);
                        ++found;
                        onHit(*hit, base + (i+1));
                    }
                }
            }
        }
        uintptr_t next=base+size;
        if(next<=addr) break;
        addr=next;
    }
    CloseHandle(hp);
    LOGI("scan pid=%lu: %d DEKs, %zu candidates over %.0f MB in %lu ms",
         pid, found, cand_tested, scanned/1e6, GetTickCount()-t0);
    return found;
}

} // namespace kw
