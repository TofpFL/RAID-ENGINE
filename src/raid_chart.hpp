// Bridge between the engine's Chart model and the RAID format: encode a
// Chart to .raid, decode a .raid back to a Chart, and the metadata JSON the
// format stores. See raid.hpp for the format itself.
#pragma once

#include <cstdint>
#include <deque>
#include <future>
#include <string>
#include <vector>

#include "chart.hpp"
#include "raid.hpp"

namespace gh::raid {

// Chart -> raid::Event list (time ms -> us, pitch unused by Chart -> -1).
std::vector<Event> chartEvents(const Chart& c);

// Writes a .raid file for `chart`. statsOut receives encoder statistics
// (logical vs literal events, stored vs raw bytes — the honest compression
// report). metaJson is built from the chart automatically.
bool saveChart(const std::string& path, const Chart& chart,
               const EncodeOptions& opt, Info* statsOut, std::string& err);

// Reads a .raid file into a Chart. Decodes every chunk (full
// materialization); refuses charts whose logical event count cannot fit in
// memory — playback code should use Reader::range instead.
bool loadChart(const std::string& path, Chart& out, std::string& err);

// Parses the raid meta JSON into chart fields (no events). Used by
// loadChart and by callers that only need headers.
bool chartFromMeta(const std::string& metaJson, Chart& out, Info& info);

// ------------------------------------------------------- streaming --------
// ChartStream is the playback-side adapter for .raid charts: it owns a
// Reader plus the metadata-only Chart (notes stay empty; lengthMs,
// playerNotes/opponentNotes/rawNoteCount are filled from the file). Notes
// are pulled in time windows via pump(), which wraps Reader::range with
// global event indices, and prefetch() warms upcoming chunks on worker
// threads. Nothing is materialized beyond the requested window.
class ChartStream {
public:
    // Opens the file and fills `meta` with chart metadata (title, tempo,
    // sections, note types, totals). meta.notes stays empty.
    bool open(const std::string& path, Chart& meta, std::string& err);
    void close();
    bool isOpen() const { return reader_.isOpen(); }

    const Chart& chart() const { return chart_; }
    uint64_t logicalEvents() const { return reader_.logicalEvents(); }
    // Chart length in ms (last event time), 0 when the file is empty.
    double lengthMs() const { return double(reader_.durationUs()) / 1000.0; }

    // All events with timeUs in [startUs, endUs) plus their global indices.
    // Windows may be requested in any order; the reader's chunk cache makes
    // sequential playback cheap. `out`/`indices` are appended to.
    bool pump(int64_t startUs, int64_t endUs, std::vector<Event>& out,
              std::vector<uint64_t>& indices, std::string& err);

    // Warms the chunks overlapping [startUs, endUs) on worker threads so a
    // later pump() hits the LRU cache. Futures are harvested on the next
    // pump()/close(); buffers stay alive until their decode finishes.
    void prefetch(int64_t startUs, int64_t endUs);
    void harvestPrefetch();

    // Decode bounds forwarded to the underlying Reader.
    void setMaxConcurrentDecodes(int n) { reader_.setMaxConcurrentDecodes(n); }
    void setMemoryBudgetBytes(uint64_t bytes) { reader_.setMemoryBudgetBytes(bytes); }

private:
    Reader reader_;
    Chart chart_;
    // Buffers owned by in-flight prefetch decodes; futures reference them.
    std::deque<std::vector<Event>> prefetchBufs_;
    std::vector<std::future<bool>> prefetchFuts_;
};

}  // namespace gh::raid
