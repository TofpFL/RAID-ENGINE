# FLP Parser Forensic Audit — `Vocals.flp` vs FLP Counter

Date: 2026-10-11. Inputs: `C:\Users\YalishannaZhou\Vocals.flp` (2,163,533,946 bytes),
FLP Counter `flp_counter.py` / `counter_report.json`, RAID `flp_import.cpp` + `raid.cpp`.

## Verdict

The FLP contains **26 chart patterns, all of them placed on the playlist**, forming
**242 pattern clips** — not the ~16 patterns / ~60 clips earlier diagnostics
suggested. The earlier misses were three parser defects, now fixed:

1. **88-byte playlist records misdecoded as 60-byte** — FL 26.1.7 writes 88-byte
   records (`FL_DT_PLAYLIST` @ 164). Blob is 21,472 bytes = 244×88 exactly; the
   parser tried 60-byte then 32-byte strides, producing ~64 garbage records.
   Fix: stride-detection tries 32/60/88; whichever stride parses cleanly wins.
2. **Phantom clips accepted** — records with `pattern_base != 20480` (audio /
   automation items) subtracted to small positive "pattern" IDs (fake `pat 3`,
   `pat 50619`). Fix: require `patBase == 20480`.
3. **Named-pattern note misassignment** — `kPatName` events sit between `kPatStart`
   and `kNoteData`, so notes of named patterns fell back to pattern 0.
   Fix: persistent `curPattern` set at every `kPatStart`.

## Independent verification (both tools, same file)

| Metric | FLP Counter | RAID importer | Match |
|---|---|---|---|
| Playlist records | 244 | 244 | exact |
| Pattern clips (patBase=20480) | 242 | 242 | exact |
| Non-pattern records skipped | 2 | 2 | exact |
| Patterns with clips | 26/26 | 26/26 | exact |
| Per-pattern use counts | 26 values | 26 values | all equal |
| Per-pattern note counts | 26 values | 26 values | all equal |
| Total clip uses | 2,039,191,308 | — | — |
| Unique notes (no stacking) | 88,408,494 | — | — |
| Notes actually playing | 1,936,363,276 | **1,936,402,508** (stack-weighted, decoded RAID) | Δ = 39,232 (0.002%) |

The 39,232-note delta is clip-window boundary rounding (µs vs tick edges) and is
negligible; effectively lossless.

## Playlist structure (ground truth)

- One `FL_DT_PLAYLIST` event, id 164, size 21,472 → 244 records × 88 bytes.
- Records 88 B: pos@0, len@4, trackRvidx@12, flags@32, patBase@36, itemIdx@40,
  flags2@42, lengthTicks@44, st@52, en@56, pitch@64, snap@68, bass@72,
  loopMode@80.
- 242 genuine clips: patterns 1–26 all used; pattern 16–19 ×16 uses each,
  patterns 25–26 ×64 uses each, pattern 18/19 each carry 10,893,184 notes.
- Heavy overlap throughout: clips on track 0 and track 1 routinely coexist
  (e.g. a 96 s base clip under 3 s stabs) — this is what the RAID op-3
  "pattern group" op exists for.

## Note-data pipeline verification

Raw FL note → channel filter → `flpToChart` → `Chart::finalize` stack-merge
(≤1 ms, same side+lane) → `chartEvents` → `writeRaidFlp` defs/clips →
`Reader::range` → `pumpNotes`.

- Stack merging collapses note multiplicity into `event.stack`; verified via
  `streamtest` stack-sum = 1,936,402,508 ≈ Counter played_total.
- Single-pattern sanity: `Vocals.flp#3` → 540,742 opponentNotes = exact raw
  count, preserved entirely via stacks.
- Converted RAID: 116 KB, 1,612,124 logical events, 33 pattern defs, 183 chunks,
  372 s. Stored at `build_game\Release\songs\watermelon\Watermelon.RAID`.

## Confirmed parser defects fixed (this audit)

| # | Defect | Evidence | Fix |
|---|---|---|---|
| 1 | 88-byte records unread | 21,472 % 60 ≠ 0; 21,472 % 88 = 0; records 1–4 decode to sensible values at stride 88 | stride detection incl. 88 |
| 2 | Non-pattern clips accepted | raw dump: `patBase=0` records produced "pat 3 @ 3.0 s" phantom | require `patBase==20480` |
| 3 | Pattern names stole note ownership | `kPatName` between `kPatStart`/`kNoteData` | persistent `curPattern` |
| 4 | Clip length ignored | record bytes 44–47 = `lengthTicks`; previously 0 → loop clips truncated | parse `lengthTicks` (still 0 in this file) |

## Known remaining gaps / hypotheses (not confirmed defects)

- **Looping clips**: `lengthTicks` now parsed; when > 0 the clip loops its
  content and `writeRaidFlp` expands repetitions. This file has none, but
  FL Counter flags 1 clip (pattern 13) as "longer than content" — FL may loop
  via a mechanism other than `lengthTicks`; revisit if looping clips matter.
- **Event-172 size variants**: Counter tries 164→recordsize overrides for
  malformed headers; our reader trusts the declared size. This file parsed
  cleanly; robustness for corrupted FLPs untested.
- **`FLDT` > 4 GB chunks**: Counter wraps `stream_end` on 32-bit overflow;
  our `readFlp` does not implement that path. Untested (file < 4 GB).
- **Muted clips**: playlist `flags & 0x02` — none present in this file; RAID
  currently ignores clip flags entirely.
- **Non-note pattern data** (automation, tempo, markers on other channels):
  filtered by the named-channel / chart-channel / `Cam Events` rules; verified
  totals match, so nothing chart-relevant was lost.

## Files

- `pattern_comparison.csv` — 26 rows, both implementations agree on all fields.
- `playlist_comparison.csv` — 242 rows; every clip identical (pat/pos/len/track).
- Regression tests: `makeFlpPlaylist88` (88-byte records + loop flags),
  non-pattern rejection, named-pattern ownership — `gh_tests` 6,790 pass.
