// Gameplay rules: judgement, scoring, health, holds and misses.
// Pure logic, no rendering or audio, so it is deterministic and testable.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "chart.hpp"

namespace gh {

enum Rating : uint8_t { kDivine = 0, kGrace, kMortal, kSinful, kRatingCount };

struct RatingInfo {
    const char* name;
    double windowMs;  // real-time window (scaled by playback rate in song time)
    int score;
    double accuracy;
    double health;
};
const RatingInfo& ratingInfo(Rating r);
constexpr double kMissWindowMs = 166.0;

enum NoteState : uint8_t { kPending = 0, kHit, kMissed, kHolding, kHeldDone, kDropped, kMineHit, kMinePassed };

struct LaneNote {
    double t = 0;
    double len = 0;
    uint8_t kind = kNormal;
    uint8_t flags = 0;
    uint8_t state = kPending;
};

struct GameEvent {
    enum Type : uint8_t { PlayerHit, PlayerMiss, PlayerMine, GhostMiss, HoldDrop, HoldDone, OpponentHit } type;
    uint8_t lane = 0;
    Rating rating = kDivine;
    double time = 0;     // note time (ms)
    double errMs = 0;    // real-time error (negative = early)
    double length = 0;   // sustain length (ms)
    uint8_t flags = 0;
};

struct SessionConfig {
    bool ghostTapping = true;
    bool botplay = false;
    bool practice = false;
    double rate = 1.0;  // playback rate; windows are constant in real time
};

class Session {
public:
    void init(const Chart& chart, const SessionConfig& cfg);
    void press(int lane, double songMs);
    void release(int lane, double songMs);
    void update(double songMs);

    // Accessors used for rendering.
    int keys() const { return keys_; }
    const std::vector<LaneNote>& lane(int side, int lane) const { return lanes_[side][lane]; }
    double maxSustain(int side, int lane) const { return maxSus_[side][lane]; }
    bool isHolding(int side, int lane) const { return activeHold_[side][lane] >= 0; }
    bool keyHeld(int lane) const { return held_[lane]; }

    std::vector<GameEvent> events;  // consumed (cleared) by the caller

    // Stats
    long long score = 0;
    int combo = 0, maxCombo = 0, misses = 0;
    std::array<int, kRatingCount> counts{};
    double health = 1.0;  // 0..2
    bool dead = false;
    int judged() const;
    double accuracy() const;   // 0..1 (1 when nothing judged)
    std::string rank() const;  // S+, S, A, B, C, D
    std::string clearLabel() const;
    struct HitError { float t, err; };
    std::vector<HitError> hitErrors;
    size_t totalPlayerNotes = 0;
    bool allResolved() const;

private:
    void hitNote(int lane, size_t idx, double songMs, bool bot);
    void addHealth(double v);
    void missNote(int lane, size_t idx);

    SessionConfig cfg_;
    int keys_ = 4;
    std::vector<LaneNote> lanes_[2][9];
    size_t cursor_[2][9]{};
    long long activeHold_[2][9];
    double maxSus_[2][9]{};
    bool held_[9]{};
    double lastMs_ = -1e18;
    double accSum_ = 0;
};

}  // namespace gh
