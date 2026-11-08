Your own songs and mods
=======================

God's Hand plays charts made for Friday Night Funkin' style engines, runs
Psych Engine mods (Lua scripts, characters, stages, events) and can chart a
song by itself from its audio.

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

Only the chosen mod's scripts run, sandboxed (no file or system access).
Videos, shaders and runHaxeCode are not supported; scripts that need them
keep working without those effects.

Supported chart formats: legacy FNF / Psych Engine 0.x, Psych Engine 1.0
("psych_v1") and V-Slice (<song>-chart.json + <song>-metadata.json).
Extra-key charts from 1 to 9 keys work; "Hurt Note" types become mines.

Press R in Freeplay to rescan without restarting the game.
