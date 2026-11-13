#pragma once
#include <stdint.h>
// Divide first: multiplying an uptime counter by 1e9 overflows after minutes.
static uint64_t workspace_counter_ms(uint64_t counter,uint64_t frequency) {
    return counter/frequency*1000+(counter%frequency)*1000/frequency;
}
static unsigned workspace_delay_ms(uint64_t last,uint64_t now,unsigned interval) {
    // A reset/backward clock must never turn a 16 ms throttle into a long sleep.
    if(now<last || now-last>=interval)return 0;
    return interval-(unsigned)(now-last);
}
static uint64_t workspace_clock_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER counter,frequency;
    if(QueryPerformanceFrequency(&frequency) && frequency.QuadPart>0 &&
       QueryPerformanceCounter(&counter) && counter.QuadPart>=0)
        return workspace_counter_ms((uint64_t)counter.QuadPart,(uint64_t)frequency.QuadPart);
    return GetTickCount64();
#else
    return WINPR_TIME_NS_TO_MS(winpr_GetTickCount64NS());
#endif
}
