# Building the definitive packed Full Edition

Requirements: Windows x64, Visual Studio with MSBuild, the v143 C++ platform
toolset, Windows 10 SDK, and the Desktop development with C++ workload.

From the extracted source folder, run `build-all.bat`. It verifies the frozen
AI/engine file hashes, builds the current Release/x64 backend through
`ResurrectedOfflineBots.sln`, then builds both proxy options. Outputs:

Extract to a short path such as `C:\src\ResurrectedOfflineBots` if Windows
long-path support is disabled; MSBuild's tracking-file paths can otherwise
exceed the legacy path limit.

- `bin\ResurrectedOfflineBots.dll`
- `bootstrap\build\version.dll`
- `bootstrap\build\X3DAudio1_7.dll`

You can run `build-backend.bat` or `build-loader.bat` independently. The backend
project includes `FrozenJasonBridge.cpp`, which incorporates the current
`Features.cpp` AI source. The installed X3DAudio proxy exports both functions
directly imported by the supported EXE and forwards to the system DLL by
absolute path. The older version API proxy remains available for comparison.

This archive is source-only. Generated `bin`, `obj`, `build`, `.exe`, `.dll`,
`.lib`, `.exp`, `.pdb`, and `.tlog` files should not be committed to the source
repository. A rebuild can be functionally identical without a byte-identical
SHA-256 because compiler/toolset versions and linker metadata may differ.

For a packed game install, copy the backend and X3DAudio proxy beside
`SummerCamp\Binaries\Win64\SummerCamp-Win64-Shipping.exe` only after closing
the game and backing up same-named files. Do not replace the original game PAK
or SIG and do not use an unsigned menu PAK. The separate binary package has a
backup-aware installer, verifier, and uninstaller. Runtime testing through
Steam is required before publishing the binary package.
