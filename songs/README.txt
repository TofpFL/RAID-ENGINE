Your own songs go here
======================

God's Hand can play charts made for Friday Night Funkin' style engines.

Option 1 - one folder per song inside "songs":

    songs/
      my-song/
        my-song-easy.json      <- one .json chart per difficulty
        my-song.json           (Normal)
        my-song-hard.json
        Inst.ogg               <- instrumental (ogg, mp3 or wav)
        Voices.ogg             <- optional vocals (muted when you miss)

Option 2 - drop whole Psych Engine / FNF mods into the "mods" folder
next to this one. Charts are read from mods/<mod>/data/<song>/ and audio
from mods/<mod>/songs/<song>/Inst.ogg + Voices.ogg
(Voices-Player.ogg / Voices-Opponent.ogg are supported too).

Supported chart formats: legacy FNF / Psych Engine 0.x, Psych Engine 1.0
("psych_v1") and V-Slice (<song>-chart.json + <song>-metadata.json).
Extra-key charts from 1 to 9 keys work; "Hurt Note" types become mines.

Press R in Freeplay to rescan without restarting the game.
