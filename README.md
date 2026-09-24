# Resurrected Offline Bots - Full Edition, definitive packed source

This is the source-only review package for the frozen 2026-09-19 Full Edition
DLL player-tested in the normal packed Steam installation. It is **not**
Sandbox Lite or the older V1 AI.
No game executable, game PAK, mod DLL, launcher EXE, build artifacts, logs, or
discovery files are included.

The full Offline Play counselor route is integrated at runtime by the backend.
This compatibility revision uses the EXE's direct `X3DAudio1_7.dll` import as
the installed, application-local bootstrap. It forwards both audio exports to
the real system DLL and loads the adjacent backend after the game window is
stable. The prior `version.dll` loader remains buildable but is not required by
the installer. No unsigned menu PAK or `-NoPak` switch is required.

This revision was tested through the normal packed-game launch on Crystal Lake
Small and Grendel. A successful compile or installer copy does not prove that
Windows selected the local proxy on every system. Run
`VERIFY-OFFLINE-BOTS-INSTALL.bat` after a launch and inspect its timestamped
bootstrap log. The installer checks the exact supported EXE SHA-256:
`5541268C88B6C02BFB8BDA2D4B07E3E04BB6A03CEF1C5E89163B1E9FBC32A430`.

The backend includes the current Jason and counselor AI, multi-map spawning and
navigation, car pursuit and extraction, knives and traps, objectives,
spectator support, Pamela sweater and mask handling, Jason-kill cinematic, and
the corrected Jason-death match outro. Occupied driver seats remain extraction
priority after a counselor re-enters a stopped car. See [BUILDING.md](BUILDING.md) for a
reproducible build and [SOURCE-REVISION.txt](SOURCE-REVISION.txt) for the exact
source and released-binary hashes.

The installable binary ZIP and Nexus description are distributed separately.
This code targets offline play with the tested Resurrection executable/layout;
it does not include or license the original game files or unrelated third-party
gameplay tools.

If you enjoy the mod and want to support future development, optional donations are appreciated:
https://www.paypal.com/donate/?hosted_button_id=RA3XBB6VDUTAC 
The mod will remain free for everyone.

Made by Antagony
https://www.nexusmods.com/fridaythe13ththegame/mods/58
