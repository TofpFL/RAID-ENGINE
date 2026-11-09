Your own songs and mods
=======================

God's Hand plays charts made for Friday Night Funkin' style engines, runs
Psych Engine mods (Lua and HScript, characters, stages, events, shaders,
videos), full Kade / base-game builds, and can chart a song by itself from
its audio. Freeplay: 7 opens the chart editor, M the mod packs.

Option 1 - one folder per song inside "songs":

    songs/
      my-song/
        my-song-easy.json      <- one .json chart per difficulty
        my-song.json           (Normal)
        my-song-hard.json
        events.json            <- optional Psych events
        Inst.ogg               <- instrumental (ogg, mp3 or wav)
        Voices.ogg             <- optional vocals (muted when you miss)

Option 2 - just the audio: a folder with only Inst.ogg, or a lone
my-song.mp3 directly in "songs", gets Easy/Normal/Hard charts made by
AutoChart.

Option 3 - Psych Engine / FNF mods go in the "mods" folder next to this
one, either as a folder or as a .zip (unpacked automatically, the archive
is then moved to mods/.installed/). The usual Psych layout works:

    mods/<mod>/data/<song>/        charts, events.json, song scripts (.lua)
    mods/<mod>/songs/<song>/       Inst.ogg + Voices.ogg
                                   (Voices-Player / Voices-Opponent too)
    mods/<mod>/characters/         character .json (+ optional .lua)
    mods/<mod>/images/             atlases (.png + .xml), icons/icon-*.png
    mods/<mod>/stages/             stage .json + .lua
    mods/<mod>/scripts/            scripts for every song of the mod
    mods/<mod>/custom_events/      event scripts
    mods/<mod>/custom_notetypes/   note type scripts
    mods/<mod>/shaders/            .frag shaders
    mods/<mod>/videos/             cutscenes (.mpg plays directly; .mp4 and
                                   .webm need ffmpeg installed, or ffmpeg
                                   next to the game, and convert once)

Scripts can be .lua or .hx (HScript). Only the chosen mod's scripts run,
sandboxed (no file or system access); script save data goes to saves/.

Full game builds work too (a folder with assets/data, assets/songs,
assets/shared, assets/week1...), as do mods inside extra folders and loose
chart files: those are grouped by song and the audio is looked up in the
other mods' songs/ folders.

Supported chart formats: legacy FNF / Psych Engine 0.x, Psych Engine 1.0
("psych_v1"), Kade and Dave & Bambi ("mania") and V-Slice
(<song>-chart.json + <song>-metadata.json). Extra-key charts from 1 to 9
keys work; "Hurt Note" types become mines. Charts of any size load: big
spam charts are streamed.

Mod packs: the game's Releases page has a "mods" release with ready-made
packs. In Freeplay press M to download and install them, or download the
zips yourself and drop them into "mods".

Press R in Freeplay to rescan without restarting the game.
