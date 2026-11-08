# God's Hand of Principality and Demise

A rhythm duel in the style of Friday Night Funkin', written from scratch in **C++20**.
Answer the songs of the heavens note for note: four arrows, two singers, one health bar.

*Um duelo de ritmo ao estilo de Friday Night Funkin', escrito de raiz em C++20.*

**Downloads (Windows / Linux / macOS):** [Releases](https://github.com/deatuamae67-crypto/Gods-Hand-of-Principality-And-Demise/releases) — extract the zip and run `godshand` (`godshand.exe` on Windows).

---

## Português

### O que é
- **Jogo de ritmo tipo FNF**: o adversário canta, tu respondes. Setas a subir (ou a descer), notas longas (*holds*), barra de vida, combo, pontuação, precisão e ranks.
- **Tudo original**: as 4 músicas são **sintetizadas em C++ no arranque** (bateria, baixo, pads, sinos e vozes "cantadas" com filtros de formantes) e os charts são gerados a partir das próprias melodias, por isso cada nota corresponde a algo que se ouve. Personagens e cenários são desenhados em código. Não há ficheiros de música nem sprites copiados.
- **Rápido e preciso**: o relógio da música vem do próprio mixer de áudio (frames enviados à placa de som), o input é lido ~1000 vezes por segundo com *timestamp*, independente dos FPS, e o julgamento usa esse tempo exato.
- **Lê charts de FNF**: Psych Engine 0.x/1.0, FNF legacy e V-Slice, de 1 a 9 teclas, "Hurt Notes" viram minas. Testado com charts de spam reais de até 24 MB (até ≈190 mil notas por lado): carregam em ~0,1 s e mantêm ~50 FPS até em renderização por software, sem placa gráfica.

### Conteúdo
| Música | Adversário | BPM | Dificuldades |
|---|---|---|---|
| Vigil (tutorial) | The Principality | 96 | Easy, Normal, Hard |
| Principality | The Principality | 118 | Easy, Normal, Hard |
| Godshand | The Hand | 146 | Easy, Normal, Hard, **Infernal** |
| Demise | Demise | 176 | Easy, Normal, Hard, **Infernal** |

Modo **História** (*Week I – The Ascension*: Principality → Godshand → Demise), **Freeplay** com pré-visualização das músicas, **Opções** (downscroll, middlescroll, ghost tapping, velocidade, offset, teclas, FPS, volumes), **calibração do offset**, ecrã de resultados com gráfico de timing, e **modificadores** (velocidade 0.5×–2×, botplay, prática sem morte, jogar como o adversário).

### Controlos
| Ação | Teclas |
|---|---|
| Setas (4 teclas) | `D F J K` ou `← ↓ ↑ →` (configurável), comando: D-pad / botões |
| Pausa | `Enter` / `Esc` |
| Menus | setas, `Enter`, `Esc`; `Tab` no Freeplay = modificadores; `R` = procurar músicas novas |
| Ecrã inteiro | `F11` |

### As tuas músicas de FNF
Coloca-as na pasta `songs/` (uma pasta por música com os `.json` e `Inst.ogg`/`Voices.ogg`) ou copia mods inteiros do Psych Engine para `mods/`. Instruções em [`songs/README.txt`](songs/README.txt).

### Compilar
Precisas de CMake ≥ 3.20 e de um compilador C++20 (o raylib 5.5 é descarregado automaticamente).
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/godshand          # Windows: build\Release\godshand.exe (MSVC) ou build\godshand.exe
```
Linux precisa de: `libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev libasound2-dev`.
Para gerar um `.exe` de Windows a partir de Linux: `-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake`.

### Porquê de raiz (e não sobre o PulseForge)
O PulseForge cresceu demasiado (Lua, SDK do Discord, streaming de biliões de notas, conteúdo de terceiros em LFS). Este projeto começa limpo: um núcleo pequeno e testável (`gh_core`), um único ficheiro de jogo por ecrã, sem conteúdo com direitos de autor, e builds automáticos para Windows, Linux e macOS.

---

## English

**Gameplay** – FNF-style call-and-response: the opponent sings a phrase, you answer it. Ratings are **DIVINE / GRACE / MORTAL / SINFUL** (45 / 90 / 135 / 166 ms), health drains on misses, holds must be held to the end, and a full combo of DIVINEs is a **SANCTIFIED** clear.

**Engine highlights**
- Song position comes from the audio mixer's own frame counter (smoothed, monotonic), so judgement never depends on frame rate.
- The main loop polls input at ~1 kHz and renders at the frame limit; every key press carries its own timestamp.
- Pure-logic core (`src/json`, `chart`, `session`, `synth`, `composer`) with a headless test suite: `build/gh_tests` (add `--audio` to also render all songs, `--chart file.json …` to benchmark real charts).
- Chart import: legacy/Psych 0.x (`mustHitSection` lane swapping), Psych 1.0 (`psych_v1`), V-Slice (`-chart.json` + `-metadata.json`), 1–9 keys, BPM changes, camera focus, mines.
- Level-of-detail note rendering keeps extreme "spam" charts smooth.

**Layout**
```
src/        engine + game (C++20, raylib for window/audio/drawing)
tests/      headless tests and chart benchmark
assets/     fonts (SIL OFL)
songs/      your own FNF-style songs
mods/       Psych Engine / FNF mods (created next to the executable)
```

**Credits** – Created for deatuamae67-crypto. Built with [raylib](https://www.raylib.com) (zlib). Fonts: Cinzel Decorative (Natanael Gama) and Rajdhani (Indian Type Foundry), both SIL OFL. Inspired by the rhythm-duel genre; all characters, music and art are original. Code under the MIT license.
