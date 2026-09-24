Resurrected Offline Bots Full - v2.1.2 compatibility fix

This build was tested in the normal packed installation on Crystal Lake Small.
The preceding compatibility build was also tested on Grendel. It supports
only the game EXE with
SHA-256 5541268C88B6C02BFB8BDA2D4B07E3E04BB6A03CEF1C5E89163B1E9FBC32A430.

Install: extract this package into the Resurrected game folder containing
SummerCamp.exe. Close the game and run INSTALL-RESURRECTED-OFFLINE-BOTS.bat.
Launch normally through Steam. Run VERIFY-OFFLINE-BOTS-INSTALL.bat and inspect
OfflineBots-Diagnostic.txt. The bootstrap log is in the user Temp folder.

The installer uses an application-local X3DAudio1_7.dll proxy, not the older
version.dll loader. No -NoPak option, unsigned menu PAK, game EXE replacement,
or manual injection is required. The original game PAK/SIG remain untouched.
If the Full Offline Bots menu does not appear, include the diagnostic report
and bootstrap log when requesting help.
