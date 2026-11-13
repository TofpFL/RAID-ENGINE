#include "session.hpp"

#include <algorithm>
#include <cmath>

namespace gh {

static const RatingInfo kRatings[kRatingCount] = {
    {"DIVINE", 45.0, 350, 1.00, 0.023},
    {"GRACE", 90.0, 200, 0.70, 0.016},
    {"MORTAL", 135.0, 100, 0.35, 0.004},
    {"SINFUL", kMissWindowMs, 50, 0.00, -0.020},
};

const RatingInfo& ratingInfo(Rating r) { return kRatings[r < kRatingCount ? r : kSinful]; }

void Session::init(const Chart& chart, const SessionConfig& cfg) {
    cfg_ = cfg;
    if (!(cfg_.rate > 0)) cfg_.rate = 1.0;
    for (double& w : cfg_.windows)
        if (!(w > 0)) w = 1.0;
    keys_ = chart.keys;
    for (int s = 0; s < 2; ++s)
        for (int l = 0; l < 9; ++l) {
            lanes_[s][l].clear();
            cursor_[s][l] = 0;
            activeHold_[s][l] = -1;
            maxSus_[s][l] = 0;
        }
    for (bool& h : held_) h = false;
    totalPlayerNotes = totalOpponentNotes = 0;
    for (size_t i = 0; i < chart.notes.size(); ++i) {
        const Note& n = chart.notes[i];
        if (n.lane >= keys_) continue;
        LaneNote ln;
        ln.type = n.type;
        ln.id = uint32_t(i);
        ln.t = n.time;
        ln.len = n.length;
        ln.kind = n.kind;
        ln.flags = n.flags;
        ln.stack = std::max<uint16_t>(1, n.stack);
        lanes_[n.side][n.lane].push_back(ln);
        maxSus_[n.side][n.lane] = std::max(maxSus_[n.side][n.lane], n.length);
        if (n.kind == kNormal) (n.side == 1 ? totalPlayerNotes : totalOpponentNotes) += ln.stack;
    }
    events.clear();
    score = 0;
    combo = maxCombo = misses = 0;
    opponentHits = 0;
    autoLanes_ = 0;
    mult_[0] = mult_[1] = 1;
    counts.fill(0);
    health = 1.0;
    dead = false;
    hitErrors.clear();
    hitErrors.reserve(size_t(std::min<uint64_t>(totalPlayerNotes, kMaxHitErrors)));
    errStride_ = 1;
    errTick_ = 0;
    lastMs_ = -1e18;
    accSum_ = 0;
}

void Session::addHealth(double v) {
    health = std::min(2.0, std::max(0.0, health + v));
    if (health <= 0.0 && !cfg_.practice && !cfg_.botplay) dead = true;
}

void Session::hitNote(int lane, size_t idx, double songMs, bool bot) {
    std::vector<LaneNote>& v = lanes_[1][lane];
    LaneNote& n = v[idx];
    // Psych judges |strumTime - songPosition + ratingOffset| / playbackRate.
    const double err = bot ? 0.0 : (songMs - n.t - cfg_.ratingOffset) / cfg_.rate;
    Rating r = kSinful;
    for (int i = 0; i < kRatingCount; ++i)
        if (std::fabs(err) <= cfg_.windows[i]) { r = Rating(i); break; }

    long long& ah = activeHold_[1][lane];
    if (ah >= 0 && size_t(ah) != idx) {
        v[size_t(ah)].state = kHeldDone;
        ah = -1;
    }
    if (n.len > 0) { n.state = kHolding; ah = (long long)idx; }
    else n.state = kHit;

    // A stack of notes is one press but every note in it counts.
    const RatingInfo& info = kRatings[r];
    const long long k = (long long)n.stack * mult_[1];
    score += (long long)info.score * k;
    combo += k;
    maxCombo = std::max(maxCombo, combo);
    counts[r] += k;
    accSum_ += info.accuracy * k;
    addHealth(info.health);
    // Timing errors for the results graph: past kMaxHitErrors hits only every
    // other one is kept (then every fourth...), so memory stays bounded.
    if (++errTick_ >= errStride_) {
        errTick_ = 0;
        hitErrors.push_back({float(n.t), float(err)});
        if (hitErrors.size() >= kMaxHitErrors) {
            size_t w = 0;
            for (size_t i = 0; i < hitErrors.size(); i += 2) hitErrors[w++] = hitErrors[i];
            hitErrors.resize(w);
            errStride_ *= 2;
        }
    }
    GameEvent e{};
    e.type = GameEvent::PlayerHit;
    e.lane = uint8_t(lane);
    e.rating = r;
    e.time = n.t;
    e.errMs = err;
    e.length = n.len;
    e.flags = n.flags;
    e.noteType = n.type;
    e.id = n.id;
    events.push_back(e);
}

void Session::missNote(int lane, size_t idx) {
    LaneNote& n = lanes_[1][lane][idx];
    n.state = kMissed;
    misses += n.stack;
    combo = 0;
    score -= 10LL * n.stack;
    addHealth(-0.0475);  // once per stack, like Psych removing the stacked duplicates
    GameEvent e{};
    e.type = GameEvent::PlayerMiss;
    e.lane = uint8_t(lane);
    e.time = n.t;
    e.flags = n.flags;
    e.noteType = n.type;
    e.id = n.id;
    events.push_back(e);
}

void Session::press(int lane, double songMs) {
    if (lane < 0 || lane >= keys_) return;
    held_[lane] = true;
    if (cfg_.botplay || (autoLanes_ >> lane & 1u)) return;
    std::vector<LaneNote>& v = lanes_[1][lane];
    const double win = cfg_.windows[kSinful] * cfg_.rate;  // Safe Frames
    const double mineWin = cfg_.windows[kDivine] * cfg_.rate;
    long long best = -1, mine = -1;
    for (size_t j = cursor_[1][lane]; j < v.size() && v[j].t <= songMs + win; ++j) {
        const LaneNote& n = v[j];
        if (n.state != kPending || n.t < songMs - win) continue;
        if (n.kind == kMine) {
            if (mine < 0 && std::fabs(n.t - songMs) <= mineWin) mine = (long long)j;
            continue;
        }
        best = (long long)j;
        break;
    }
    if (best >= 0) {
        hitNote(lane, size_t(best), songMs, false);
    } else if (mine >= 0) {
        LaneNote& n = v[size_t(mine)];
        n.state = kMineHit;
        combo = 0;
        addHealth(-0.3);
        GameEvent e{};
        e.type = GameEvent::PlayerMine;
        e.lane = uint8_t(lane);
        e.time = n.t;
        e.noteType = n.type;
        e.id = n.id;
        events.push_back(e);
    } else if (!cfg_.ghostTapping) {
        ++misses;
        combo = 0;
        score -= 10;
        addHealth(-0.04);
        GameEvent e{};
        e.type = GameEvent::GhostMiss;
        e.lane = uint8_t(lane);
        e.time = songMs;
        events.push_back(e);
    }
}

void Session::release(int lane, double songMs) {
    if (lane < 0 || lane >= keys_) return;
    held_[lane] = false;
    if (cfg_.botplay || (autoLanes_ >> lane & 1u)) return;
    long long& h = activeHold_[1][lane];
    if (h < 0) return;
    LaneNote& n = lanes_[1][lane][size_t(h)];
    const double end = n.t + n.len;
    GameEvent e{};
    e.lane = uint8_t(lane);
    e.time = n.t;
    e.length = n.len;
    e.flags = n.flags;
    e.noteType = n.type;
    e.id = n.id;
    if (songMs < end - 120.0 * cfg_.rate) {
        n.state = kDropped;
        ++misses;
        combo = 0;
        addHealth(-0.0475);
        e.type = GameEvent::HoldDrop;
    } else {
        n.state = kHeldDone;
        e.type = GameEvent::HoldDone;
    }
    h = -1;
    events.push_back(e);
}

void Session::update(double songMs) {
    const double dt = lastMs_ > -1e17 ? std::max(0.0, songMs - lastMs_) : 0.0;
    lastMs_ = songMs;
    const double win = cfg_.windows[kSinful] * cfg_.rate;
    for (int lane = 0; lane < keys_; ++lane) {
        // ---- player side
        std::vector<LaneNote>& v = lanes_[1][lane];
        long long& h = activeHold_[1][lane];
        if (h >= 0) {
            LaneNote& n = v[size_t(h)];
            if (songMs >= n.t + n.len) {
                n.state = kHeldDone;
                GameEvent e{};
                e.type = GameEvent::HoldDone;
                e.lane = uint8_t(lane);
                e.time = n.t;
                e.length = n.len;
                events.push_back(e);
                h = -1;
            } else {
                addHealth(0.00018 * dt / cfg_.rate);
            }
        }
        size_t& c = cursor_[1][lane];
        if (cfg_.botplay || (autoLanes_ >> lane & 1u)) {
            for (size_t j = c; j < v.size() && v[j].t <= songMs; ++j) {
                if (v[j].state != kPending) continue;
                if (v[j].kind == kMine || (v[j].flags & kFlagIgnore)) v[j].state = kMinePassed;
                else hitNote(lane, j, v[j].t, true);
            }
        }
        while (c < v.size() && v[c].state != kPending) ++c;
        for (size_t j = c; j < v.size() && v[j].t < songMs - win; ++j) {
            if (v[j].state != kPending) continue;
            if (v[j].kind == kMine || (v[j].flags & kFlagIgnore)) v[j].state = kMinePassed;
            else missNote(lane, j);
        }
        while (c < v.size() && v[c].state != kPending) ++c;

        // ---- opponent side (always perfect)
        std::vector<LaneNote>& o = lanes_[0][lane];
        long long& oh = activeHold_[0][lane];
        size_t& oc = cursor_[0][lane];
        while (oc < o.size() && o[oc].t <= songMs) {
            LaneNote& n = o[oc];
            if (n.state == kPending) {
                if (n.kind == kMine || (n.flags & kFlagIgnore)) {
                    n.state = kMinePassed;
                } else {
                    if (oh >= 0) o[size_t(oh)].state = kHeldDone;
                    oh = -1;
                    opponentHits += (long long)n.stack * mult_[0];
                    n.state = n.len > 0 ? kHolding : kHit;
                    if (n.len > 0) oh = (long long)oc;
                    GameEvent e{};
                    e.type = GameEvent::OpponentHit;
                    e.lane = uint8_t(lane);
                    e.time = n.t;
                    e.length = n.len;
                    e.flags = n.flags;
                    e.noteType = n.type;
                    e.id = n.id;
                    events.push_back(e);
                }
            }
            ++oc;
        }
        if (oh >= 0 && songMs >= o[size_t(oh)].t + o[size_t(oh)].len) {
            o[size_t(oh)].state = kHeldDone;
            oh = -1;
        }
    }
}

void Session::rewindTo(double songMs) {
    for (int s = 0; s < 2; ++s)
        for (int l = 0; l < keys_; ++l) {
            std::vector<LaneNote>& v = lanes_[s][l];
            const size_t first = size_t(std::lower_bound(v.begin(), v.end(), songMs,
                                                         [](const LaneNote& n, double t) { return n.t < t; }) - v.begin());
            for (size_t i = first; i < v.size(); ++i) v[i].state = kPending;
            cursor_[s][l] = first;
            activeHold_[s][l] = -1;
        }
    lastMs_ = -1e18;
}

long long Session::judged() const {
    long long n = 0;
    for (long long c : counts) n += c;
    return n;
}

double Session::accuracy() const {
    const long long total = judged() + misses;
    return total > 0 ? accSum_ / double(total) : 1.0;
}

std::string Session::rank() const {
    if (judged() + misses == 0) return "-";
    const double a = accuracy();
    if (a >= 0.995 && misses == 0) return "S+";
    if (a >= 0.95) return "S";
    if (a >= 0.90) return "A";
    if (a >= 0.80) return "B";
    if (a >= 0.70) return "C";
    return "D";
}

std::string Session::clearLabel() const {
    if (judged() == 0) return "";
    if (misses == 0) {
        if (counts[kGrace] + counts[kMortal] + counts[kSinful] == 0) return "SANCTIFIED";
        if (counts[kMortal] + counts[kSinful] == 0) return "BLESSED";
        return "FAITHFUL";
    }
    return misses < 10 ? "REPENTANT" : "CLEAR";
}

bool Session::allResolved() const {
    for (int s = 0; s < 2; ++s)
        for (int l = 0; l < keys_; ++l) {
            if (activeHold_[s][l] >= 0) return false;
            if (cursor_[s][l] < lanes_[s][l].size()) return false;
        }
    return true;
}

}  // namespace gh
