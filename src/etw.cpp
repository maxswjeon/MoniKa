// etw.cpp
#include "etw.hpp"
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "advapi32.lib")

namespace kw {

// Microsoft-Windows-Kernel-Process {22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}
static const GUID KERNEL_PROCESS_GUID =
    { 0x22fb2cd6, 0x0e7b, 0x422b, { 0xa0, 0xc7, 0x2f, 0xad, 0x1f, 0xd0, 0xe7, 0x16 } };
static const ULONGLONG KW_KEYWORD_PROCESS = 0x10; // WINEVENT_KEYWORD_PROCESS
static const USHORT EVID_PROCESS_START = 1;

static bool get_prop(EVENT_RECORD* ev, const wchar_t* name, vector<uint8_t>& out){
    PROPERTY_DATA_DESCRIPTOR desc;
    desc.PropertyName = (ULONGLONG)(uintptr_t)name;
    desc.ArrayIndex = 0;
    ULONG sz = 0;
    if(TdhGetPropertySize(ev, 0, nullptr, 1, &desc, &sz)!=ERROR_SUCCESS || sz==0) return false;
    out.resize(sz);
    return TdhGetProperty(ev, 0, nullptr, 1, &desc, sz, out.data())==ERROR_SUCCESS;
}

static void WINAPI record_cb(EVENT_RECORD* ev){
    auto* self = (EtwProcessMonitor*)ev->UserContext;
    if(self) self->handle_record(ev);
}

void EtwProcessMonitor::handle_record(void* rec){
    EVENT_RECORD* ev = (EVENT_RECORD*)rec;
    if(!IsEqualGUID(ev->EventHeader.ProviderId, KERNEL_PROCESS_GUID)) return;
    if(ev->EventHeader.EventDescriptor.Id != EVID_PROCESS_START) return;

    vector<uint8_t> pidBuf, imgBuf;
    DWORD pid = 0;
    if(get_prop(ev, L"ProcessID", pidBuf)){
        if(pidBuf.size()>=8) pid=(DWORD)(*(ULONGLONG*)pidBuf.data());
        else if(pidBuf.size()>=4) pid=*(DWORD*)pidBuf.data();
    }
    wstring image;
    if(get_prop(ev, L"ImageName", imgBuf) && imgBuf.size()>=2)
        image = (wchar_t*)imgBuf.data();
    if(pid && cb_) cb_(pid, image);
}

bool EtwProcessMonitor::start(StartFn onStart){
    cb_ = std::move(onStart);

    size_t nameBytes = (sessionName_.size()+1)*sizeof(wchar_t);
    propsBuf_.assign(sizeof(EVENT_TRACE_PROPERTIES)+nameBytes, 0);
    auto* props = (EVENT_TRACE_PROPERTIES*)propsBuf_.data();
    props->Wnode.BufferSize = (ULONG)propsBuf_.size();
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1; // QPC
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    TRACEHANDLE session = 0;
    ULONG rc = StartTraceW(&session, sessionName_.c_str(), props);
    if(rc == ERROR_ALREADY_EXISTS){
        ControlTraceW(0, sessionName_.c_str(), props, EVENT_TRACE_CONTROL_STOP);
        // rebuild props (ControlTrace may have altered them)
        propsBuf_.assign(sizeof(EVENT_TRACE_PROPERTIES)+nameBytes, 0);
        props = (EVENT_TRACE_PROPERTIES*)propsBuf_.data();
        props->Wnode.BufferSize = (ULONG)propsBuf_.size();
        props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        props->Wnode.ClientContext = 1;
        props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        rc = StartTraceW(&session, sessionName_.c_str(), props);
    }
    if(rc != ERROR_SUCCESS){
        LOGE("StartTrace failed rc=%lu%s", rc,
             rc==ERROR_ACCESS_DENIED ? " (run as Administrator)" : "");
        return false;
    }
    session_ = session;

    rc = EnableTraceEx2(session, &KERNEL_PROCESS_GUID, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                        TRACE_LEVEL_INFORMATION, KW_KEYWORD_PROCESS, 0, 0, nullptr);
    if(rc != ERROR_SUCCESS){ LOGE("EnableTraceEx2 failed rc=%lu", rc); stop(); return false; }

    EVENT_TRACE_LOGFILEW log; ZeroMemory(&log, sizeof(log));
    log.LoggerName = (LPWSTR)sessionName_.c_str();
    log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    log.EventRecordCallback = record_cb;
    log.Context = this;

    TRACEHANDLE th = OpenTraceW(&log);
    if(th == (TRACEHANDLE)INVALID_HANDLE_VALUE){ LOGE("OpenTrace failed %lu", GetLastError()); stop(); return false; }
    traceHandle_ = th;

    running_ = true;
    th_ = std::thread([this]{
        TRACEHANDLE h = (TRACEHANDLE)traceHandle_;
        LOGI("ETW Kernel-Process consumer running");
        ProcessTrace(&h, 1, nullptr, nullptr);   // blocks until session stopped
        LOGI("ETW consumer stopped");
    });
    return true;
}

void EtwProcessMonitor::stop(){
    if(session_){
        size_t nameBytes=(sessionName_.size()+1)*sizeof(wchar_t);
        vector<uint8_t> buf(sizeof(EVENT_TRACE_PROPERTIES)+nameBytes, 0);
        auto* props=(EVENT_TRACE_PROPERTIES*)buf.data();
        props->Wnode.BufferSize=(ULONG)buf.size();
        props->LoggerNameOffset=sizeof(EVENT_TRACE_PROPERTIES);
        ControlTraceW((TRACEHANDLE)session_, sessionName_.c_str(), props, EVENT_TRACE_CONTROL_STOP);
        session_=0;
    }
    if(traceHandle_){ CloseTrace((TRACEHANDLE)traceHandle_); traceHandle_=0; }
    if(th_.joinable()) th_.join();
    running_=false;
}

EtwProcessMonitor::~EtwProcessMonitor(){ stop(); }

} // namespace kw
