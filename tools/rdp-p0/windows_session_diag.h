// Independent watchdog: never calls FreeRDP or records key codes, text or pixels.
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
enum { DIAG_CONNECT=1, DIAG_CLIPBOARD, DIAG_WAIT, DIAG_MESSAGES,
       DIAG_EVENTS, DIAG_INPUT, DIAG_GRAPHICS, DIAG_PRESENT, DIAG_FOCUS, DIAG_SELECTION, DIAG_DISCONNECT };
static struct {
    HANDLE stop, thread;
    wchar_t path[32768];
    volatile LONG stage, active, frames, dirty, inputs, sent, checks;
    volatile LONG result, server_error;
    volatile LONG64 stage_at, frame_at, action_at;
    unsigned attempt;
} session_diag;
#define diag_read(v) InterlockedCompareExchange(&(v),0,0)
#define diag_read64(v) InterlockedCompareExchange64(&(v),0,0)
static void diag_stage(LONG stage) {
    InterlockedExchange64(&session_diag.stage_at, GetTickCount64());
    InterlockedExchange(&session_diag.stage,stage);
}
static void diag_frame(void) {
    InterlockedIncrement(&session_diag.frames);
    InterlockedExchange64(&session_diag.frame_at,GetTickCount64());
}
static void diag_input(int meaningful) {
    InterlockedIncrement(&session_diag.inputs);
    // Preserve the first unanswered action; repeated input cannot postpone detection.
    if(meaningful && diag_read64(session_diag.action_at)<=diag_read64(session_diag.frame_at))
        InterlockedExchange64(&session_diag.action_at,GetTickCount64());
}
static unsigned diag_flags(ULONGLONG now,ULONGLONG stage_at,ULONGLONG frame_at,
                           ULONGLONG action_at,int active,int stage) {
    unsigned flags=0;
    (void)stage;
    if(stage_at && now>=stage_at && now-stage_at>=15000)
        flags|=1; // Includes slow connect/disconnect: phase identifies which.
    if(active && frame_at && action_at>frame_at && now>=action_at && now-action_at>=15000)
        flags|=2; // Suspicion only: input is not guaranteed to change the picture.
    return flags;
}
static DWORD WINAPI diag_watch(void *unused) {
    (void)unused;
    FILE *log=_wfopen(session_diag.path,L"a");
    if(!log)return 0;
    unsigned previous=0;
    const char *names[]={"none","connect","clipboard_poll","event_wait","window_messages",
                         "rdp_event_check","input_send","graphics_sync","frame_present","focus_send","selection_poll","disconnect"};
    do {
        ULONGLONG now=GetTickCount64(),stage_at=diag_read64(session_diag.stage_at),
            frame_at=diag_read64(session_diag.frame_at),action_at=diag_read64(session_diag.action_at);
        LONG stage=diag_read(session_diag.stage),active=diag_read(session_diag.active);
        unsigned flags=diag_flags(now,stage_at,frame_at,action_at,active,stage);
        SYSTEMTIME utc;GetSystemTime(&utc);
        fprintf(log,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ tick=%llu pid=%lu attempt=%u event=%s flags=%u phase=%s phase_age_ms=%llu active=%ld frame_age_ms=%llu action_age_ms=%llu dirty=%ld frames=%ld input_read=%ld input_sent=%ld event_checks=%ld\n",
            utc.wYear,utc.wMonth,utc.wDay,utc.wHour,utc.wMinute,utc.wSecond,utc.wMilliseconds,
            now,(unsigned long)GetCurrentProcessId(),session_diag.attempt,
            flags?"stall_suspected":previous?"stall_cleared":"heartbeat",flags,
            stage>=1&&stage<=DIAG_DISCONNECT?names[stage]:"none",now>=stage_at?now-stage_at:0,active,
            frame_at&&now>=frame_at?now-frame_at:0,action_at&&now>=action_at?now-action_at:0,
            diag_read(session_diag.dirty),diag_read(session_diag.frames),diag_read(session_diag.inputs),
            diag_read(session_diag.sent),diag_read(session_diag.checks));
        fflush(log);previous=flags;
        if(_ftelli64(log)>2*1024*1024){
            fclose(log);
            wchar_t backup[32768];
            if(swprintf(backup,32768,L"%ls.previous.log",session_diag.path)>0){
                _wremove(backup);_wrename(session_diag.path,backup);
            }
            log=_wfopen(session_diag.path,L"w");if(!log)return 0;
        }
    }while(WaitForSingleObject(session_diag.stop,5000)==WAIT_TIMEOUT);
    fprintf(log,"session_end pid=%lu attempt=%u result=0x%08lx server_error=0x%08lx phase=%ld frames=%ld input_read=%ld input_sent=%ld\n",
        (unsigned long)GetCurrentProcessId(),session_diag.attempt,(unsigned long)diag_read(session_diag.result),
        (unsigned long)diag_read(session_diag.server_error),diag_read(session_diag.stage),
        diag_read(session_diag.frames),diag_read(session_diag.inputs),diag_read(session_diag.sent));
    fclose(log);return 0;
}
static void diag_stop(void) {
    if(session_diag.thread){SetEvent(session_diag.stop);WaitForSingleObject(session_diag.thread,INFINITE);CloseHandle(session_diag.thread);}
    if(session_diag.stop)CloseHandle(session_diag.stop);
    session_diag.thread=session_diag.stop=NULL;
}
static void diag_start(unsigned attempt) {
    diag_stop();memset(&session_diag,0,sizeof(session_diag));
    const wchar_t *path=_wgetenv(L"CALABI_RDP_DIAG_LOG");
    if(!path || wcslen(path)>=32768)return;
    wcscpy(session_diag.path,path);session_diag.attempt=attempt;diag_stage(DIAG_CONNECT);
    session_diag.stop=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(session_diag.stop)session_diag.thread=CreateThread(NULL,0,diag_watch,NULL,0,NULL);
}
#else
#define diag_stage(x) ((void)0)
#define diag_frame() ((void)0)
#define diag_input(x) ((void)0)
#define diag_start(x) ((void)0)
#define diag_stop() ((void)0)
#endif
