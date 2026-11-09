# God's Hand of Principality and Demise

A rhythm duel in the style of Friday Night Funkin', written from scratch in **C++20**, merged with the best of **PulseForge**:
it plays Psych Engine mods (Lua and HScript, characters, stages, events, shaders, videos), full Kade/base-game builds and
spam charts of any size, has a chart editor, charts any song by itself, records replays and installs mods from `.zip`.

*Um duelo de ritmo ao estilo de Friday Night Funkin', escrito de raiz em C++20 e unido ao melhor do PulseForge.*

**Downloads (Windows / Linux / macOS):** [Releases](https://github.com/deatuamae67-crypto/Gods-Hand-of-Principality-And-Demise/releases) — extract the zip and run `godshand` (`godshand.exe` on Windows).
**Mod packs:** release [`mods`](https://github.com/deatuamae67-crypto/Gods-Hand-of-Principality-And-Demise/releases/tag/mods), or press `M` in Freeplay.

---

## Português

### O que é
- **Jogo de ritmo tipo FNF**: o adversário canta, tu respondes. Setas a subir (ou a descer), notas longas (*holds*), barra de vida, combo, pontuação, precisão e ranks.
- **4 músicas originais** sintetizadas em C++ no arranque (bateria, baixo, pads, sinos e vozes "cantadas"), com charts gerados das próprias melodias. Personagens e cenários desenhados em código.
- **Rápido e preciso**: o relógio da música vem do mixer de áudio, o input é lido ~1000 vezes por segundo com *timestamp* e o julgamento não depende dos FPS.

### Mods
- **Psych Engine a sério**: personagens (`characters/*.json` + atlas Sparrow), ícones, cores da barra de vida, cenários (`stages/*.json`, incluindo objetos do editor 0.7.3), eventos, tipos de nota e **scripts Lua 5.4** com a API do Psych (≈250 funções: sprites, textos, tweens, timers, câmaras, propriedades, grupos de notas e strums, sons com tempo e *pitch*, dados guardados, *substates*, definições do mod, `Function_Stop`) e modcharts do Kade Engine.
- **HScript**: `runHaxeCode`, `runHaxeFunction`, `createCallback` e ficheiros `.hx` (em `scripts/`, `stages/`, `data/<música>/`...) com `game`, `FlxG`, `FlxSprite`, `FlxText`, `FlxTween`, `FlxTimer`, `FlxColor`, `ShaderFilter`...
- **Shaders**: os `.frag` do Psych/Flixel (`#pragma header`, `flixel_texture2D`) são traduzidos para OpenGL 3.3 e funcionam em sprites (`setSpriteShader`) e em câmaras inteiras (`setFilters` em HScript).
- **Vídeos**: `startVideo` (cutscenes, ENTER salta) e `makeVideoSprite`. Ficheiros `.mpg` tocam diretamente; `.mp4`/`.webm` são convertidos uma vez com o **ffmpeg** (se estiver instalado ou ao lado do jogo) para `cache/videos/`.
- **Builds completas do Kade / jogo base** (pasta `assets/` com `shared/`, `week1`...): as personagens e cenários das semanas 1–6 são reconstruídos a partir das imagens do mod.
- **Qualquer formato de pasta**: mods do Psych, pastas dentro de pastas, charts soltos (agrupados por música, com o áudio procurado em todos os mods), V-Slice, 1 a 9 teclas (`mania` do Dave & Bambi / Kade).
- **Charts de spam gigantes**: um chart de 1,5 GB (25 milhões de notas) abre em ~5 s; as notas empilhadas no mesmo milissegundo juntam-se como no Psych.
- Os scripts correm isolados (sem `io`, `os`, `require`) e param se entrarem em ciclo infinito.

### Os teus mods
Os mods do PulseForge estão publicados como **pacotes** no release [`mods`](https://github.com/deatuamae67-crypto/Gods-Hand-of-Principality-And-Demise/releases/tag/mods) (14 pacotes, 1,25 GB). No jogo: **Freeplay → `M`** descarrega um pacote ou todos (usa o `curl` do sistema) e instala-os. Também podes descarregar os `.zip` à mão e pô-los em `mods/`.

### Também
- **Editor de charts** (Freeplay → `7`): grelha ao estilo do Psych, notas e *holds* com o rato, *snap* de 1/4 a 1/192, secções (lado da câmara, BPM), velocidade, tipos de nota, copiar/colar secções, desfazer/refazer, reprodução com *hitsounds*. Guarda em JSON do Psych (o original fica como `.bak`); `ENTER` guarda e joga.
- **AutoChart**: uma pasta só com o áudio (ou um `.mp3`/`.ogg` solto em `songs/`) ganha charts Easy/Normal/Hard gerados a partir da música; abre-os no editor para os afinar.
- **Replays** (`R` nos resultados), **mods por `.zip`** (põe em `mods/`, `R` no Freeplay), **controlos táteis** (Opções).

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
| Freeplay | `Tab` modificadores · `7` editor de charts · `M` pacotes de mods · `R` procurar músicas e mods novos |
| Rever replay | `R` no ecrã de resultados |
| Ecrã inteiro | `F11` |

### Compilar
Precisas de CMake ≥ 3.20 e de um compilador C++20. O raylib 5.5, o Lua 5.4.7, o miniz e o pl_mpeg são descarregados automaticamente.
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/godshand          # Windows: build\Release\godshand.exe (MSVC) ou build\godshand.exe
```
Linux precisa de: `libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev libasound2-dev`.
Para gerar um `.exe` de Windows a partir de Linux: `-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake`.

### Porquê assim
O PulseForge cresceu demasiado (SDK do Discord, streaming de biliões de notas, conteúdo em LFS no próprio repositório) e ficou difícil de manter. Aqui o núcleo pequeno e testado do God's Hand fica como base e o que o PulseForge tinha de melhor foi reescrito por cima, com testes. O conteúdo dos mods vive fora do código: pacotes no release `mods`, gerados uma vez a partir do LFS do PulseForge pelo workflow *Mod packs*.

---

## English

**Gameplay** – FNF-style call-and-response. Ratings are **DIVINE / GRACE / MORTAL / SINFUL** (45 / 90 / 135 / 166 ms); a full combo of DIVINEs is a **SANCTIFIED** clear.

**Mods**
- Psych Engine mods run in a Flixel-style scene: Sparrow/Packer atlases with trimming, Psych and V-Slice characters, stages (including 0.7.3 stage objects), cameras with zoom/shake/flash/fade, FlxEase tweens, timers, and Lua 5.4 with the Psych API (≈250 functions and every common callback, persistent save data, custom substates, mod settings, seekable sounds) plus Kade modchart helpers.
- HScript: `runHaxeCode`, `runHaxeFunction`, `createCallback` and `.hx` script files, bridged to `game`, `FlxG`, `FlxSprite`, `FlxText`, `FlxTween`, `FlxTimer`, `FlxColor`, `ShaderFilter`, `FlxRuntimeShader` and more.
- Shaders: OpenFL/Flixel fragment shaders are translated to GLSL 330 and run on sprites and whole cameras.
- Videos: `startVideo` cutscenes and `makeVideoSprite` with pl_mpeg; `.mp4`/`.webm` are converted once with ffmpeg when available.
- Full Kade/base-game builds: the week 1–6 characters, stages and stage behaviour are rebuilt from the mod's own images.
- Any folder layout: nested mod folders, loose charts grouped by song with audio found across mods, 1–9 keys (`mania`), V-Slice.
- Huge spam charts are streamed: 1.5 GB / 25 million notes load in about 5 s with bounded memory.
- Scripts are sandboxed and stopped if they loop forever.

**Also** – chart editor (Freeplay `7`), in-game mod pack downloader (Freeplay `M`), AutoChart for songs without charts, deterministic replays, `.zip` mod installs, touch controls.

**Engine** – the song position comes from the audio mixer's frame counter; input is polled at ~1 kHz with timestamps. Pure-logic core (`json`, `chart`, `chart_stream`, `session`, `synth`, `composer`, `psych`, `scene`, `lua_host`, `hscript`, `shader_glsl`, `basegame`, `autochart`, `replay`, `mod_install`) with a headless test suite:
```sh
build/gh_tests                          # all tests (add --audio to render the songs too)
build/gh_tests --chart a.json b.json    # parse/benchmark charts of any size
build/gh_tests --psych-corpus mods/     # load every atlas, character, stage and Lua script in a mod folder
build/godshand --list-songs --base dir/ # what the song library finds in dir/songs and dir/mods
```

**Layout**
```
src/        engine + game (C++20, raylib for window/audio/drawing, Lua 5.4, miniz, pl_mpeg)
tests/      headless tests, chart benchmark and mod corpus checker
assets/     fonts (SIL OFL)
songs/      your own songs (charts, or just audio for AutoChart)
modpacks/   the list of mod packs and the script that publishes them
mods/       Psych Engine / FNF mods, folders or .zip (created next to the executable)
replays/    recorded runs     saves/  script save data     cache/  converted videos
```

**Credits** – Created for deatuamae67-crypto, merging God's Hand with ideas and features from PulseForge (same author). Built with [raylib](https://www.raylib.com) (zlib), [Lua](https://www.lua.org) (MIT), [miniz](https://github.com/richgel999/miniz) (MIT) and [pl_mpeg](https://github.com/phoboslab/pl_mpeg) (MIT). Fonts: Cinzel Decorative (Natanael Gama) and Rajdhani (Indian Type Foundry), both SIL OFL. All built-in characters, music and art are original. Code under the MIT license; mod packs are the user's own content.
