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

// Detect a real sleep boundary even on desktops where power broadcasts are not
// delivered. CPU load or a stalled UI increases both clocks and is not a resume.
class ResumeClock
{
public:
    bool Poll()
    {
        const ULONGLONG before = GetTickCount64();
        ULONGLONG awake = 0;
        if (!QueryUnbiasedInterruptTime(&awake)) return false;
        const ULONGLONG after = GetTickCount64();
        // Do not mistake preemption between clock reads for suspended time.
        if (after - before > 100) return false;
        const ULONGLONG awakeMs = awake / 10000ULL;
        const ULONGLONG bias = after >= awakeMs ? after - awakeMs : 0;
        const bool resumed = _initialized && bias > _bias && bias - _bias >= 1000;
        _bias = bias;
        _initialized = true;
        return resumed;
    }
private:
    ULONGLONG _bias = 0;
    bool _initialized = false;
};
}
