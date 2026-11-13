#include <assert.h>
#include "windows_session_diag.h"
int main(void) {
    // An idle desktop and background window must not be mistaken for a stalled response.
    assert(diag_flags(90000,89990,1000,0,1,DIAG_EVENTS)==0);
    assert(diag_flags(90000,89990,1000,2000,0,DIAG_EVENTS)==0);
    assert(diag_flags(90000,89990,3000,2000,1,DIAG_EVENTS)==0);
    assert(diag_flags(16999,16990,1000,2000,1,DIAG_EVENTS)==0);
    assert(diag_flags(17000,16990,1000,2000,1,DIAG_EVENTS)==2);
    assert(diag_flags(17000,1000,1000,0,1,DIAG_INPUT)==1);
    assert(diag_flags(17000,1000,1000,0,1,DIAG_WAIT)==1);
    diag_frame();diag_input(1);
    LONG64 first=diag_read64(session_diag.action_at);diag_input(1);
    assert(diag_read64(session_diag.action_at)==first);
    diag_frame();
    assert(diag_flags(GetTickCount64(),GetTickCount64(),diag_read64(session_diag.frame_at),first,1,DIAG_INPUT)==0);
    // The independent thread must keep logging while the calling thread is blocked.
    wchar_t dir[MAX_PATH],path[MAX_PATH];GetTempPathW(MAX_PATH,dir);
    assert(GetTempFileNameW(dir,L"rdp",0,path));
    _wputenv_s(L"CALABI_RDP_DIAG_LOG",path);
    diag_start(4);assert(session_diag.thread);
    InterlockedExchange64(&session_diag.stage_at,GetTickCount64()-16000);
    InterlockedExchange(&session_diag.stage,DIAG_INPUT);
    Sleep(5200);diag_stop();
    FILE *f=_wfopen(path,L"r");assert(f);char text[8192];size_t n=fread(text,1,sizeof(text)-1,f);text[n]=0;fclose(f);
    assert(strstr(text,"stall_suspected") && strstr(text,"phase=input_send") && strstr(text,"session_end"));
    assert(strstr(text,"attempt=4"));_wremove(path);
    puts("RDP diagnostic watchdog passed: idle/background/recovery, stalled phase and independent heartbeat.");
    return 0;
}
