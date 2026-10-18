#pragma once
// Backend preference policy for camera_capture.h. Kept free of OpenCV and
// Media Foundation so the regression tests can exercise it directly.

namespace hfl_camera
{
enum class Backend { MSMF, DSHOW };

inline const char* BackendName(Backend b) { return b == Backend::MSMF ? "MSMF" : "DSHOW"; }

// A single DSHOW success must not demote MSMF: while another app holds the
// camera, MSMF opens but delivers no frames, and DSHOW may win only because
// the other app released the device. DSHOW opens were measured at 4.7-7 s on
// the lock screen, so a sticky demotion would undo the whole speedup.
constexpr int kFallbacksBeforeDshow = 2;      // consecutive MSMF-failed sessions
constexpr int kDshowSessionsBeforeRetry = 10; // then probe MSMF first again

struct Preference
{
    Backend backend = Backend::MSMF;
    bool known = false;
    // MSMF preferred: consecutive sessions that fell back to DSHOW.
    // DSHOW preferred: sessions since DSHOW became preferred.
    int streak = 0;
};

// Preference to store after a successful open with `used`.
inline Preference NextPreference(const Preference& pref, bool msmfMappable, Backend used)
{
    Preference next;
    next.known = true;
    if (!msmfMappable || used == Backend::MSMF)
    {
        next.backend = used;
        return next;
    }
    if (pref.backend == Backend::DSHOW)
    {
        const int sessions = pref.streak + 1;
        next.backend = sessions >= kDshowSessionsBeforeRetry ? Backend::MSMF : Backend::DSHOW;
        // Re-probing MSMF starts one fallback short of demotion: a device
        // where MSMF never works returns to DSHOW after a single slow session.
        next.streak = next.backend == Backend::MSMF ? kFallbacksBeforeDshow - 1 : sessions;
        return next;
    }
    const int fallbacks = pref.streak + 1;
    next.backend = fallbacks >= kFallbacksBeforeDshow ? Backend::DSHOW : Backend::MSMF;
    next.streak = next.backend == Backend::DSHOW ? 0 : fallbacks;
    return next;
}
} // namespace hfl_camera
