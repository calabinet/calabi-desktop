#include <windows.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include "workspace_clock.h"
int main(void) {
    // Actual WinPR 3.32.2 Windows formula: QPC * 1e9 / frequency.
    // At 10 MHz its uint64 multiply wraps about every 1844 seconds.
    const uint64_t frequency=10000000,wrap=UINT64_MAX/1000000000;
    const uint64_t before=(wrap-10000)*1000000000ull/frequency/1000000;
    const uint64_t after=(wrap+10000)*1000000000ull/frequency/1000000;
    assert(after<before);
    assert(before+16-after>1000000); // Reproduces the old multi-minute freeze.
    assert(workspace_delay_ms(before,after,16)==0);
    uint64_t a=workspace_counter_ms(wrap-10000,frequency),b=workspace_counter_ms(wrap+10000,frequency);
    assert(b>=a && b-a==2);
    assert(workspace_delay_ms(a,b,16)==14);
    assert(workspace_delay_ms(0,1000,16)==0);
    assert(workspace_delay_ms(1000,1001,400)==399);
    assert(workspace_delay_ms(UINT64_MAX-5,2,16)==0);
    assert(workspace_counter_ms(60ull*24*60*60*frequency,frequency)==60ull*24*60*60*1000);
    uint64_t previous=workspace_clock_ms();Sleep(20);
    assert(workspace_clock_ms()>=previous);
    puts("Workspace clock passed: reproduces WinPR overflow, bounds rollback waits, preserves 16 ms pacing and long uptime.");
}
