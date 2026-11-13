# RAID Engine

A binary chart format and streaming playback engine for rhythm games, built for
**extraordinarily dense FNF-style charts — trillions of note events without ever
loading them into memory**. It targets sustained 120–240 FPS playback, bounded
RAM regardless of chart length, lossless chart data, and random access.

RAID Engine powers **God's Hand of Principality and Demise** (`godshand`), a
C++20 Friday Night Funkin'-style rhythm duel with full Psych Engine mod support —
the game is the reference host; the engine is the point.

## The `.raid` format (`src/raid.{hpp,cpp}`)

Versioned 128-byte header, then independently addressable sections:

- **Chunk index** — fixed-size entries giving each chunk's time range, event
  range, offset, sizes and CRC32. Playback binary-searches by time without
  decoding anything.
- **Metadata** — deflate+CRC32 JSON: title, difficulty, tempo, sections, focus,
  note types, `lengthMs`, `playerNotes`/`opponentNotes` totals.
- **Pattern table** — deflate+CRC32 reusable event sequences with pitch/lane
  transforms and nesting.
- **Chunks** — independently compressed op streams. Three op kinds:
  - *Literal absolute*: columnar storage — delta-varint times, lengths, zigzag
    pitch, lanes, packed side/kind/flags, types, stacks.
  - *Literal relative*: the same columns inside pattern definitions, relative
    to the pattern origin.
  - *Pattern instance*: `patternId, repeatCount, anchorUs, pitchDelta,
    laneDelta` — the decoder re-emits the pattern per repeat, transformed.

The encoder detects repeated runs (≥8 events), extends consecutive identical
blocks into repeat counts, and transposes uniform pitch/lane shifts (the
keyboard-grid case). It reports honest statistics — `logicalEvents` vs
`literalEvents` vs `patternCount`, raw vs stored bytes — and never fakes a
compression ratio on incompressible data.

Safety: the encoder validates every timestamp, pitch, lane and transform;
the decoder bounds time accumulation, transform magnitudes, nesting depth,
chunk expansion (4M events), section sizes (512 MiB), and verifies CRC32 and
declared counts everywhere. Malformed input cannot cause unbounded allocation,
time overflow, or excessive recursion (400-iteration fuzz suite).

## Streaming decoder (`raid::Reader`)

- `open()` reads only the header + index — a few KB regardless of chart size.
- `range(startUs, endUs, out, err, indices)` decodes only overlapping chunks
  and returns events **with global expanded indices** — stable ids without
  materialization.
- `chunkEventsAsync` decodes on worker threads; an LRU chunk cache, a decode
  semaphore and a configurable memory budget bound concurrent work.
- Internally refcounted (`shared_ptr<Impl>`): async jobs survive `Reader`
  destruction, `FILE*` I/O is serialized, the cache is mutex-protected — safe
  for concurrent `range()` + prefetch + shutdown.
- `opLog()` walks the encoded op stream (not expanded events) and returns
  `OpSegment`s: literal runs and **leaf** pattern instances with time ranges,
  repeat counts and transforms — the only place pattern identity survives.
- `PatternLogger` ticks with the playhead and appends segments to a file,
  marking every pattern→pattern change.

## Chart bridge + rolling playback window

- `raid::ChartStream` (`src/raid_chart.*`): a metadata-only `Chart` (notes stay
  empty; totals and `lengthMs` come from the header/meta) plus `pump()` —
  windowed events with global indices — and `prefetch()` for worker-side
  chunk warming.
- `Session` (`src/session.*`): `appendLaneNote()` feeds events in global time
  order (per-lane order preserved, so cursors/judgement/holds are untouched);
  `gcBefore()` drops resolved notes >30 s behind the playhead — lane memory is
  bounded by note **density**, not chart length.
- `PlayState` (`src/state_play.cpp`): `.raid` charts pump events up to
  `songMs + 4 s`, prefetch 16 s ahead, flip sides under mirror mode, and GC
  each frame. Restart re-inits from the stream frontier.

## Tooling

**`raid_engine.exe`** — standalone CLI (icon embedded):

```text
raid_engine convert <chart|flp|fsc> <out.raid>   # encode (JSON, V-Slice,
                                                #  PulseForge, FL Studio)
raid_engine info <file.raid>                     # header + encoder stats
raid_engine verify <file.raid>                   # full decode, note count
raid_engine window <file.raid> <startMs> <endMs> # streaming window dump
raid_engine meta <file.raid>                     # raw metadata JSON
raid_engine patterns <file.raid> [startMs] [endMs]  # op-stream log: which
                                                    # pattern plays when
```

`gh_tests --raid <chart|flp|fsc> <out.raid>` does the same conversion with the
compression report.

**In-game pattern log** — Freeplay → TAB → "Pattern log (.raid)" writes
`<userDir>/raid_pattern_log.txt` during playback, marking every
pattern→pattern change. On `taimuresu.RAID` (75,819 events → 89 KB, 3.72×):
4,160 op segments, 3,003 pattern instances, 1,849 pattern→pattern changes.

`.RAID` files have a registered icon (`RAID_File_Icon.ico`, embedded in both
executables; `tools/register_raid_icon*.ps1` registers the association).

## Converting charts

Input goes through `loadChartFile`: Psych/FNF JSON, V-Slice, PulseForge
(`chart.json`, `.pfchart.json`, autosaves), and **FL Studio `.flp`/`.fsc`**
projects via SNIFF pattern detection — lossless conversion to `.raid`.

## Tests & build

Headless suite (`tests/`): roundtrip, pattern/pitch transforms, streaming
windows, `ChartStream`≡materialized equivalence, session streaming, encoder
validation, malformed-input fuzz, concurrency + memory budgets, opLog and
pattern-logger coverage. **6,756 tests, 0 failures.**

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release        # gh_tests + raid_engine
cmake -S . -B build_game -DGH_BUILD_GAME=ON # + godshand (raylib fetched)
cmake --build build_game --config Release
```

## The game — God's Hand of Principality and Demise

`godshand` is the playable engine: FNF-style call-and-response (DIVINE / GRACE /
MORTAL / SINFUL ratings), Psych Engine mods (Lua 5.4 + HScript, characters,
stages, events, shaders, videos), Kade/base-game builds, a chart editor
(`7`), AutoChart, deterministic replays, `.zip` mod installs, rendering mode,
and `.raid` streaming playback. See the release page for downloads
(Windows / Linux / macOS / Android).

## Honest limitations

- **Encoder input is still a materialized `Chart`** — trillion-event
  *authoring* is future work; the format/decoder/playback path supports it.
- **Script note access degrades on `.raid`**: `noteOrder_`/`noteRef`/
  `noteCount` return empty — Psych scripts that enumerate notes see none.
- **Rewind** (`loopSong`) works within the ~30 s GC margin; farther back,
  dropped notes can't replay. `restart()` always works.
- **`LaneNote::id` is `uint32_t`** — wraps past 4 G events (only affects
  script ids, which don't work on streams anyway).
- **Renderer unchanged** — drawing uses the existing per-note path over
  windowed lanes; GPU instancing is not implemented yet.
- The pattern log reports **encoded** pattern operations — it is not a map of
  visual note patterns.

## Credits

Created for deatuamae67-crypto / TofpFL, merging God's Hand with ideas and
features from PulseForge (same author). Event, effect and script behaviour
follow [JS Engine](https://github.com/JordanSantiagoYT/FNF-JS-Engine)
(Apache-2.0) and Psych Engine; the code and shaders are our own. Built with
[raylib](https://www.raylib.com) (zlib), [Lua](https://www.lua.org) (MIT),
[miniz](https://github.com/richgel999/miniz) (MIT) and
[pl_mpeg](https://github.com/phoboslab/pl_mpeg) (MIT). Fonts: Cinzel
Decorative and Rajdhani, both SIL OFL. Code under the MIT license; mod packs
are the user's own content.
