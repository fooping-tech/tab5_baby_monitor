#pragma once
#include <cmath>
#include <cstdint>

namespace baby_edge {
enum class Presence { Unknown, Present, Absent };
enum class Posture { Unknown, Supine, NotSupine };

inline const char *name(Presence state)
{
    switch (state) {
    case Presence::Present: return "present";
    case Presence::Absent: return "absent";
    default: return "unknown";
    }
}

struct PresenceConfig {
    float threshold = 0.5f;
    int64_t enter_ms = 2000;
    int64_t exit_ms = 5000;
    int64_t stale_ms = 10000;
};

class PresenceFilter {
public:
    explicit PresenceFilter(PresenceConfig config = {}) : config_(config) {}

    Presence update(int64_t frame_ms, int64_t now_ms, bool valid, float target_score)
    {
        if (!valid_config() || !valid || frame_ms < 0 || now_ms < frame_ms ||
            now_ms - frame_ms > config_.stale_ms ||
            (last_frame_ms_ >= 0 && frame_ms <= last_frame_ms_) ||
            !std::isfinite(target_score) || target_score < 0 || target_score > 1) {
            reset();
            return state_;
        }
        if (last_frame_ms_ >= 0 && frame_ms - last_frame_ms_ > config_.stale_ms) {
            reset();
        }
        last_frame_ms_ = frame_ms;
        const auto candidate = target_score >= config_.threshold ? Presence::Present : Presence::Absent;
        if (candidate != candidate_) {
            candidate_ = candidate;
            candidate_since_ms_ = frame_ms;
        }
        const auto dwell = candidate == Presence::Present ? config_.enter_ms : config_.exit_ms;
        if (frame_ms - candidate_since_ms_ >= dwell) {
            state_ = candidate;
        }
        return state_;
    }

    Presence current(int64_t now_ms)
    {
        if (last_frame_ms_ < 0 || now_ms < last_frame_ms_ || now_ms - last_frame_ms_ > config_.stale_ms) {
            reset();
        }
        return state_;
    }

    void reset()
    {
        state_ = candidate_ = Presence::Unknown;
        last_frame_ms_ = candidate_since_ms_ = -1;
    }

private:
    bool valid_config() const
    {
        return std::isfinite(config_.threshold) && config_.threshold > 0 && config_.threshold <= 1 &&
               config_.enter_ms >= 0 && config_.exit_ms >= 0 && config_.stale_ms > 0;
    }
    PresenceConfig config_;
    Presence state_ = Presence::Unknown;
    Presence candidate_ = Presence::Unknown;
    int64_t last_frame_ms_ = -1;
    int64_t candidate_since_ms_ = -1;
};
}
