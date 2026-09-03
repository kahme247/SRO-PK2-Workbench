# PK2 Workbench PRO

PK2 Workbench PRO is a clean Windows-native rebuild of the old Joymax/ZeraPain PK2 editor and extractor workflow.

The original folder only contains binaries, so this project is a fresh implementation:

- `pk2core`: reusable C++17 library for PK2 tree parsing, extraction, import, delete, and safe rewrite.
- `pk2cli`: command-line tool for automation and regression testing.
- `pk2win`: classic Win32 GUI with tree/list browsing and menu actions.

The final tool does not require `GFXFileManager.dll` and does not redistribute Joymax binaries or assets.

## Build

Install Visual Studio 2022 with the Desktop C++ workload and CMake, then run:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

For 32-bit compatibility builds:

```powershell
cmake -S . -B build-x86 -A Win32
cmake --build build-x86 --config Release
```

## CLI Examples

```powershell
build\Release\pk2cli.exe list Media.pk2 --password=169841
build\Release\pk2cli.exe extract Media.pk2 server_dep output --children --overwrite --password=169841
build\Release\pk2cli.exe import-file Media.pk2 .\new.txt server_dep\new.txt Media.edited.pk2
build\Release\pk2cli.exe md5 "private server blowfish key"
```

## GUI Notes

- Use the search box and **Search** button to find files or folders anywhere in the open archive by name or internal path.
- When no archive is loaded, drag one `.pk2` file onto the window to open it.
- Double-click a `.txt` entry to edit and save it directly inside the PK2 while preserving its original text encoding.
- File sizes are shown with readable units: `B`, `KB`, `MB`, and `GB`.
- `Extract Shown` writes into a folder named after the archive stem. For example, extracting from `Data.pk2` into `C:\out` writes under `C:\out\Data`.
- Files and folders can be dragged onto the window to import them into the currently selected archive folder.
- Imports, drag-and-drop changes, deletions, and Server Configuration updates are saved automatically. Structural saves (deletions, new folders, defragment, Save As) rewrite the archive and write a `.bak` safety backup first; plain content replacements are appended in place without a fresh `.bak`.
- In-place saves only grow the archive, so normal saves never shrink it unexpectedly; use `Tools > Defragment` (which takes a `.bak` backup) to compact orphaned bytes.
- Files and folders can be dragged out of the list/tree into Explorer to extract them by shell copy.
- Opening, extracting, and importing run on background worker threads with a status-bar progress bar, so large operations should not freeze the window.
- Windows filesystem paths are converted through UTF-8 helpers, so non-ANSI folder and file names can be used without code-page conversion errors.
- Flat PK2 archives with root-level files and no folders show a `[root files]` item in the tree and a count summary in the status bar.
- `Tools > Server Configuration` provides an integrated IPInput-style editor for the root-level `DIVISIONINFO.TXT`, `GATEPORT.TXT`, and encrypted `SV.T` files in `Media.pk2`. It supports multiple divisions and gateway URLs plus Content ID, gateway port, and client version.

## Current Compatibility Notes

The PK2 reader/writer uses the standard Joymax header marker, 256-byte header, 20-entry encrypted directory blocks, and 128-byte directory entries inferred from the provided tools. Official Joymax PK2s use the directory block at `0x100` as the root; header offset scanning is only used as a fallback. It supports plaintext blocks, standard Blowfish fallbacks, and the Joymax-compatible masked Blowfish key schedule used by the original `GFXFileManager.dll`.

Large archives are opened by streaming header/directory metadata blocks instead of reading the full PK2 into memory. The Windows GUI also loads archives on a background thread and lazily expands the folder tree, so opening a large `Media.pk2` should keep the window responsive. Extraction copies data in 1 MB chunks, reuses source readers, and uses a capped worker pool for many-file extracts.

The local `Data.pk2` was verified for listing and extracting with the default `169841` password. A redistributable legal PK2 fixture is still needed for checked-in regression coverage.
