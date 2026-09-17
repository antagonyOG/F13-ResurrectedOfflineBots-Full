# Resurrected Offline Bots - Full Edition, definitive packed source

This is the source-only review package for the 2026-09-17 Full Edition DLL used
in the packed Resurrection test. It is **not** Sandbox Lite or the older V1 AI.
No game executable, game PAK, mod DLL, launcher EXE, build artifacts, logs, or
discovery files are included.

The full Offline Play counselor route is integrated at runtime by the backend;
the adjacent `version.dll` proxy loads that backend during a normal packed-game
startup. No unsigned menu PAK or `-NoPak` switch is required.

The backend includes the current Jason and counselor AI, multi-map spawning and
navigation, car pursuit and extraction, knives and traps, objectives,
spectator support, Pamela sweater and mask handling, Jason-kill cinematic, and
the corrected Jason-death match outro. See [BUILDING.md](BUILDING.md) for a
reproducible build and [SOURCE-REVISION.txt](SOURCE-REVISION.txt) for the exact
source and released-binary hashes.

The installable binary ZIP and Nexus description are distributed separately.
This code targets offline play with the tested Resurrection executable/layout;
it does not include or license the original game files.

Support Development

If you enjoy the mod and want to support future development, optional donations are appreciated:

PayPal:
https://www.paypal.com/donate/?hosted_button_id=RA3XBB6VDUTAC

The mod will remain free for everyone.

Made by Antagony
https://www.nexusmods.com/fridaythe13ththegame/mods/58
