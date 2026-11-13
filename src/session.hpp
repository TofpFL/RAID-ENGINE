// Gameplay rules: judgement, scoring, health, holds and misses.
// Pure logic, no rendering or audio, so it is deterministic and testable.
#pragma once

#include <algorithm>
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
    uint16_t type = 0;
    uint16_t stack = 1;  // stacked notes of the file it stands for
    uint32_t id = 0;     // index of the note in the chart (script note id)
};

struct GameEvent {
    enum Type : uint8_t { PlayerHit, PlayerMiss, PlayerMine, GhostMiss, HoldDrop, HoldDone, OpponentHit } type;
    uint8_t lane = 0;
    Rating rating = kDivine;
    double time = 0;     // note time (ms)
    double errMs = 0;    // real-time error (negative = early)
    double length = 0;   // sustain length (ms)
    uint8_t flags = 0;
    uint16_t noteType = 0;
    uint32_t id = 0;
};

struct SessionConfig {
    bool ghostTapping = true;
    bool botplay = false;
    bool practice = false;
    double rate = 1.0;  // playback rate; windows are constant in real time
    // Psych's Sick!, Good and Bad hit windows and its Safe Frames window (the
    // furthest a note can be hit, 10 frames = 166.7 ms), in real-time ms; and
    // its Rating Offset (positive: you have to hit later to be on time).
    double windows[kRatingCount] = {45.0, 90.0, 135.0, kMissWindowMs};
    double ratingOffset = 0;
};

class Session {
public:
    void init(const Chart& chart, const SessionConfig& cfg);
    void press(int lane, double songMs);
    void release(int lane, double songMs);
    void update(double songMs);
    // Notes from `songMs` on can be played again (JS Engine loopTheSong); stats stay.
    void rewindTo(double songMs);

    // Accessors used for rendering.
    int keys() const { return keys_; }
    const std::vector<LaneNote>& lane(int side, int lane) const { return lanes_[side][lane]; }
    std::vector<LaneNote>& laneMut(int side, int lane) { return lanes_[side][lane]; }
    double maxSustain(int side, int lane) const { return maxSus_[side][lane]; }
    bool isHolding(int side, int lane) const { return activeHold_[side][lane] >= 0; }
    long long activeHold(int side, int lane) const { return activeHold_[side][lane]; }
    void setBotplay(bool b) { cfg_.botplay = b; }
    // Lanes the bot plays for the player (JS Engine bot energy), one bit per lane.
    void setAutoLanes(unsigned mask) { autoLanes_ = mask; }
    // Each player/opponent hit counts this many notes (JS Engine "Change Note Multiplier").
    void setNoteMultiplier(int side, long long m) { mult_[side & 1] = std::max(1LL, m); }
    void setPractice(bool b) { cfg_.practice = b; }
    bool botplay() const { return cfg_.botplay; }
    bool keyHeld(int lane) const { return held_[lane]; }

    std::vector<GameEvent> events;  // consumed (cleared) by the caller

    // Stats. Every note of the file counts, stacked ones included.
    // 64-bit everywhere: spam charts pass two billion notes.
    long long score = 0;
    long long combo = 0, maxCombo = 0, misses = 0;
    std::array<long long, kRatingCount> counts{};
    long long opponentHits = 0;
    double health = 1.0;  // 0..2
    void setHealth(double v) { health = 0; addHealth(v); }
    bool dead = false;
    long long judged() const;
    double accuracy() const;   // 0..1 (1 when nothing judged)
    std::string rank() const;  // S+, S, A, B, C, D
    std::string clearLabel() const;
    struct HitError { float t, err; };
    std::vector<HitError> hitErrors;
    uint64_t totalPlayerNotes = 0, totalOpponentNotes = 0;  // every note of the file, stacked ones included
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
    unsigned autoLanes_ = 0;
    long long mult_[2] = {1, 1};
    static constexpr size_t kMaxHitErrors = size_t(1) << 21;
    uint64_t errStride_ = 1, errTick_ = 0;
};

}  // namespace gh
