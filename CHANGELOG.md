# Changelog

## 0.3.1 - 2026-09-03

### Integrity fixes

- Creating a new empty archive now produces an openable file; `open` accepts a valid root block holding only the `.` self entry instead of rejecting it as a wrong password, and still rejects undecodable blocks as before.
- Deleting an entry no longer touches the source file before saving. Deletions are structural changes persisted through the `.bak`-backed full-rewrite path, so a reported "deleted and saved" is always durable and the input of `delete <src> <path> <dst>` is never modified.
- Failed `Save As` / defragment operations now remove their orphan `.tmp` scratch file instead of leaving it beside the archive.
- Added regression coverage for empty-archive round trips, delete persistence with `.bak`, and `.tmp` cleanup on save failure.

## 0.3.0 - 2026-07-24

### Highlights

- Added archive-wide search for files and folders by name or internal path.
- Added drag-and-drop opening of a `.pk2` file when no archive is loaded.
- Added an integrated text editor for `.txt` files stored inside PK2 archives.

### Search and text editing

- Run searches explicitly with the new **Search** button instead of searching after every typed character.
- Double-click a `.txt` entry to open, edit, and save it directly inside the archive.
- Save text changes and the containing PK2 automatically while retaining the existing `.bak` safety backup.
- Preserve ANSI, UTF-8, UTF-8 BOM, UTF-16 little-endian, and UTF-16 big-endian text encodings.
- Support editable text files up to 64 MB and reject null-containing files that cannot be safely handled as text.

### Workflow and reliability

- Open a PK2 by dragging a single `.pk2` file onto the empty application window.
- Keep the existing drag-and-drop import behavior when an archive is already open.
- Enable standard MSVC C++ exception-unwind semantics across Windows builds.

## 0.2.0 - 2026-07-13

### Highlights

- Added a built-in **Server Configuration** editor under `Tools > Server Configuration`.
- Added automatic saving after every successful archive modification.
- Improved compatibility with real Silkroad `Media.pk2` client configuration files.

### Server Configuration

- Edit the Content ID, client version, GatewayServer port, division names, and gateway IP addresses or host names without launching the legacy IPInput utility.
- Add, update, or remove multiple divisions and gateway addresses.
- Read and write the root-level `DIVISIONINFO.TXT`, `GATEPORT.TXT`, and encrypted `SV.T` files directly inside `Media.pk2`.
- Preserve unrelated bytes in `SV.T` while updating only the encrypted version block.
- Support the standard Silkroad little-endian version encryption and compatible alternate/legacy layouts.

### Automatic Saving

The open PK2 is now saved automatically after:

- Dragging files or folders into the archive.
- Importing a file or folder from the menu.
- Deleting an archive entry.
- Applying Server Configuration changes.

The existing `.bak` backup behavior remains enabled for every save. `File > Save` is still available as a manual fallback.

### Reliability

- Added in-memory archive file reading and replacement support.
- Added regression coverage for persisted byte replacement, multi-division server configuration, gateway ports, and both supported `SV.T` encryption byte orders.
- Updated the About dialog to display the application version.

### Archive integrity fixes

- Preserve the existing PK2 file size during normal saves when compacted content would otherwise make the archive unexpectedly smaller.
- Preserve the complete original 256-byte Joymax header, including the version, encryption flag, and launcher verification bytes.
- Reopen the rewritten archive after every Save and Save As operation so later automatic saves always use refreshed file offsets.
- Prevent repeated saves in the same session from reading unchanged payloads at stale positions.
- Added regression coverage for physical-size preservation, header identity, and consecutive saves after file-size changes.
- Validated two consecutive saves against a disposable copy of a real 732 MB `Media.pk2`, retaining all 19,632 entries and configuration payloads.

## 0.1.0 - 2026-07-12

- Initial public source release of PK2 Workbench PRO.
