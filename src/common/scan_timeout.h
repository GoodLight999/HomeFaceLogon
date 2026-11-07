#pragma once
#include <windows.h>

namespace hfl_timing
{
// A recognition budget measures time the machine was awake, not time asleep.
// Keep the clock next to the budget so callers cannot accidentally use uptime.
class ScanTimeout
{
public:
    explicit ScanTimeout(ULONGLONG milliseconds)
        : _started(Now()), _budget(milliseconds) {}
    bool Expired() const { return Now() - _started >= _budget; }
private:
    static ULONGLONG Now()
    {
        ULONGLONG ticks = 0;
        if (QueryUnbiasedInterruptTime(&ticks)) return ticks / 10000ULL;
        return GetTickCount64();
    }
    ULONGLONG _started;
    ULONGLONG _budget;
};
}
