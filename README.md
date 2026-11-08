# God's Hand of Principality and Demise

A rhythm duel in the style of Friday Night Funkin', written from scratch in **C++20**, now merged with the best of **PulseForge**:
it plays Psych Engine mods (Lua scripts, characters, stages, events), charts any song by itself, records replays and installs mods from `.zip`.

*Um duelo de ritmo ao estilo de Friday Night Funkin', escrito de raiz em C++20 e agora unido ao melhor do PulseForge.*

**Downloads (Windows / Linux / macOS):** [Releases](https://github.com/deatuamae67-crypto/Gods-Hand-of-Principality-And-Demise/releases) — extract the zip and run `godshand` (`godshand.exe` on Windows).

---

## Português

### O que é
- **Jogo de ritmo tipo FNF**: o adversário canta, tu respondes. Setas a subir (ou a descer), notas longas (*holds*), barra de vida, combo, pontuação, precisão e ranks.
- **4 músicas originais** sintetizadas em C++ no arranque (bateria, baixo, pads, sinos e vozes "cantadas"), com charts gerados das próprias melodias. Personagens e cenários desenhados em código.
- **Rápido e preciso**: o relógio da música vem do mixer de áudio, o input é lido ~1000 vezes por segundo com *timestamp* e o julgamento não depende dos FPS.

### Vindo do PulseForge (reescrito sobre o núcleo do God's Hand)
- **Mods do Psych Engine a sério**: personagens (`characters/*.json` + atlas Sparrow), ícones, cores da barra de vida, cenários (`stages/*.json`, incluindo objetos do editor 0.7.3) e **scripts Lua 5.4** com a API do Psych — sprites, textos, tweens, timers, câmaras, propriedades, grupos de notas e strums (modcharts), sons, `Function_Stop` — e também modcharts do Kade Engine. Os scripts correm isolados (sem `io`, `os`, `require`) e só os do mod escolhido são carregados.
- **Eventos do Psych**: Hey!, Add Camera Zoom, Play Animation, Camera Follow Pos, Alt Idle Animation, Screen Shake, Change Character, Change Scroll Speed, Set Property, Play Sound, `events.json` e eventos V-Slice; tipos de nota GF Sing, Alt Animation, No Animation, Hey!, Hurt.
- **AutoChart**: uma pasta só com o áudio (ou um `.mp3`/`.ogg` solto em `songs/`) ganha charts Easy/Normal/Hard gerados a partir da música (onsets, BPM exato, notas longas, troca de vez entre cantores).
- **Replays**: cada jogada é gravada; no ecrã de resultados carrega `R` para a rever. O melhor replay de cada música fica em `replays/`.
- **Instalar mods por `.zip`**: põe o `.zip` em `mods/` e ele é extraído na próxima procura (`R` no Freeplay).
- **Controlos táteis** (Opções): cada pista do jogador é uma coluna onde se toca.
- **Charts de spam enormes** (≈190 mil notas por lado) carregam em ~0,1 s e mantêm a fluidez.

### Conteúdo
| Música | Adversário | BPM | Dificuldades |
|---|---|---|---|
| Vigil (tutorial) | The Principality | 96 | Easy, Normal, Hard |
| Principality | The Principality | 118 | Easy, Normal, Hard |
| Godshand | The Hand | 146 | Easy, Normal, Hard, **Infernal** |
| Demise | Demise | 176 | Easy, Normal, Hard, **Infernal** |

Modo **História**, **Freeplay** com pré-visualização, **Opções**, **calibração do offset**, resultados com gráfico de timing e **modificadores** (velocidade 0.5×–2×, botplay, prática, jogar como o adversário).

### Controlos
| Ação | Teclas |
|---|---|
| Setas (4 teclas) | `D F J K` ou `← ↓ ↑ →` (configurável), comando: D-pad / botões |
| Pausa | `Enter` / `Esc` |
| Menus | setas, `Enter`, `Esc`; `Tab` no Freeplay = modificadores; `R` = procurar músicas e mods novos |
| Rever replay | `R` no ecrã de resultados |
| Ecrã inteiro | `F11` |

### Mods e músicas
- **Mods do Psych Engine**: copia a pasta do mod (ou o `.zip`) para `mods/`. A estrutura é a do Psych: `data/<música>/`, `songs/<música>/Inst.ogg`, `characters/`, `images/`, `stages/`, `scripts/`, `custom_events/`, `custom_notetypes/`.
- **Músicas soltas**: `songs/<música>/` com `.json` e `Inst.ogg`, ou só o áudio (AutoChart).
- Mais detalhes em [`songs/README.txt`](songs/README.txt).

### Compilar
Precisas de CMake ≥ 3.20 e de um compilador C++20. O raylib 5.5, o Lua 5.4.7 e o miniz são descarregados automaticamente.
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/godshand          # Windows: build\Release\godshand.exe (MSVC) ou build\godshand.exe
```
Linux precisa de: `libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev libasound2-dev`.
Para gerar um `.exe` de Windows a partir de Linux: `-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake`.

### Porquê assim
O PulseForge cresceu demasiado (SDK do Discord, streaming de biliões de notas, conteúdo de terceiros em LFS) e ficou difícil de manter. Aqui o núcleo pequeno e testado do God's Hand fica como base, e o que o PulseForge tinha de melhor — a compatibilidade com mods, o autochart, os replays, a instalação de mods — foi reescrito por cima dele, com testes. Não há conteúdo com direitos de autor no repositório: os mods são teus e ficam na tua pasta `mods/`.

---

## English

**Gameplay** – FNF-style call-and-response. Ratings are **DIVINE / GRACE / MORTAL / SINFUL** (45 / 90 / 135 / 166 ms); a full combo of DIVINEs is a **SANCTIFIED** clear.

**From PulseForge, rebuilt on this engine**
- Psych Engine mods run in a Flixel-style scene: Sparrow/Packer atlases with trimming, Psych and V-Slice characters, stages (including 0.7.3 stage objects), cameras with zoom/shake/flash/fade, FlxEase tweens, timers, and Lua 5.4 with the Psych API (≈200 functions and every common callback) plus Kade modchart helpers. Scripts are sandboxed and stopped if they loop forever.
- Psych and V-Slice events, note types, `events.json`.
- AutoChart for songs without charts (tempo exact to 0.001 BPM on the built-in songs).
- Deterministic replays, `.zip` mod installs, touch controls.

**Engine** – the song position comes from the audio mixer's frame counter; input is polled at ~1 kHz with timestamps. Pure-logic core (`json`, `chart`, `session`, `synth`, `composer`, `psych`, `scene`, `lua_host`, `autochart`, `replay`, `mod_install`) with a headless test suite:
```sh
build/gh_tests                          # all tests (add --audio to render the songs too)
build/gh_tests --chart a.json b.json    # parse/benchmark charts
build/gh_tests --psych-corpus mods/     # load every atlas, character, stage and Lua script in a mod folder
```

**Layout**
```
src/        engine + game (C++20, raylib for window/audio/drawing, Lua 5.4, miniz)
tests/      headless tests, chart benchmark and mod corpus checker
assets/     fonts (SIL OFL)
songs/      your own songs (charts, or just audio for AutoChart)
mods/       Psych Engine / FNF mods, folders or .zip (created next to the executable)
replays/    recorded runs (created when you play)
```

**Credits** – Created for deatuamae67-crypto, merging God's Hand with ideas and features from PulseForge (same author). Built with [raylib](https://www.raylib.com) (zlib), [Lua](https://www.lua.org) (MIT) and [miniz](https://github.com/richgel999/miniz) (MIT). Fonts: Cinzel Decorative (Natanael Gama) and Rajdhani (Indian Type Foundry), both SIL OFL. All characters, music and art are original. Code under the MIT license.
