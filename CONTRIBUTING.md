# Contributing

This guide covers how to change the mod, test it safely, record each version in this repository, and publish it to the [Windhawk mod catalogue](https://windhawk.net/mods).

## Who can publish

Windhawk only accepts updates to a mod from the GitHub account in its `@github` metadata, which is [stoilms](https://github.com/stoilms). Anyone is welcome to suggest changes by opening an issue or a pull request here. Accepted changes are then published to Windhawk by the maintainer.

## Repository layout

| Path | Purpose |
|---|---|
| `explorer-size-on-disk-column.wh.cpp` | The mod: metadata, readme, settings and source code in one file. This is the only file submitted to Windhawk. |
| `images/` | Screenshots used in the mod's readme and in `README.md`. |
| `README.md` | The repository's front page. |
| `CONTRIBUTING.md` | This guide. |
| `LICENSE` | GNU General Public License v3.0. |

## Development setup

You need Windows 11 24H2 or later (x64) and [Windhawk](https://windhawk.net/).

1. If the catalogue version of the mod is installed, disable it while you develop. Two copies would hook the same Explorer functions.
2. In Windhawk, choose **Create a new mod** and replace the template with the contents of `explorer-size-on-disk-column.wh.cpp`. Windhawk saves it as a local mod with the ID `local@explorer-size-on-disk-column`.
3. Click **Compile** to build and load it into Explorer.
4. To see the mod's log, enable logging in the mod's **Advanced** tab and open Windhawk's log viewer (or a tool such as DebugView). Turn on the mod's **Diagnostics** setting for extra detail, such as which code paths Explorer uses and the column layouts being changed.

## If Explorer breaks while testing

Changes to this mod can stop Explorer windows from opening. Keep these recovery steps handy:

1. Press **Ctrl+Shift+Esc** to open Task Manager.
2. Use **Run new task** to start Windhawk (normally `C:\Program Files\Windhawk\windhawk.exe`) and disable the mod.
3. On the **Details** tab, end every `explorer.exe` process, then use **Run new task** with `explorer.exe`.

Disabling the mod alone may not be enough: if Explorer is already stuck, it has to be restarted.

## Making a change

1. Create a branch for the change.
2. Edit `explorer-size-on-disk-column.wh.cpp`, then copy it into the Windhawk editor and compile.
3. Test the change (see the checklist below).
4. Bump `@version` in the metadata. Use a patch version for fixes (0.5.0 to 0.5.1) and a minor version for new features or behaviour changes (0.5.1 to 0.6.0). Every version submitted to Windhawk must be new.
5. Update the readme block in the mod file and `README.md` if behaviour or settings changed.
6. Commit using the message format below, then open a pull request or merge to `main`.

## Code guidelines

These rules come from problems found during development. The commit history explains each one in detail.

- **Never do slow work on a thread that owns windows.** Explorer calls the column's getter on its window threads, and walking a folder tree there froze Explorer. Folder sizes are calculated on background threads and Explorer is told to redraw the item afterwards.
- **Only ask Explorer to redraw an item when its value changed.** Redrawing unchanged items made Explorer request them again, which caused a recalculation loop.
- **Don't add the column to non-folder layouts.** Adding it to the Home page layout stopped Explorer windows from opening. The template change only applies to layouts that include `System.ItemNameDisplay` and excludes Home markers. Keep it that way, and test Home after any change to it.
- **Leave copy, move and delete operations alone.** The `CRecursiveFolderOperation` guards stop the mod from changing values Windows uses during file operations.
- **Don't trigger cloud downloads.** Open files with `FILE_FLAG_OPEN_REPARSE_POINT` or query them by name, and don't list cloud folders that aren't on the PC yet (`FILE_ATTRIBUTE_RECALL_ON_OPEN`).
- **Mark guessed symbols as optional.** If a hooked function's name is uncertain, make its `SYMBOL_HOOK` optional so the mod still loads when the symbol doesn't exist. Required hooks should only use names confirmed from symbol data.
- **Keep unloading fast.** Long-running work must check the stop flag so disabling the mod never waits on a folder walk.
- **Formatting:** follow the Windhawk repository's style (Chromium style with 4-space indentation, as in its `.clang-format`).
- **Spelling:** user-facing text and comments use British English.

## Testing checklist

Test every change against this list before committing a new version.

| Area | Check |
|---|---|
| Start-up | Restart Explorer from Task Manager. The taskbar appears within a few seconds, and the log shows `Init took ... ms`. |
| Windows | **Win+E** opens Explorer, and **Home** and **Gallery** open normally. |
| Files | Several files match **Size on disk** in their Properties dialog, including a tiny file (under about 700 bytes, which should show 0 bytes). |
| Compressed files | Files in `C:\Windows\System32` match Properties. Many are compressed by Windows. |
| Folders | A small folder and a large folder with many subfolders match Properties. Folder contents appear immediately and values fill in afterwards. |
| Cloud folders | OneDrive folders with online-only and downloaded files match Properties, and opening them doesn't trigger downloads. |
| Sorting | Sorting by Size on disk works in a large folder, with folders kept together unless that setting is changed. |
| Column picker | **Size on disk** appears in the column header menu and under **More...**. |
| Default layouts | After **Reset Folders** in Folder Options, new folder views include the column. |
| Disabling | Disabling the mod while folders are being calculated doesn't freeze Explorer. |

## Commit messages

Each version is a single commit, so the history records how the mod developed. Use this format:

```
vX.Y.Z: Short summary of the change (72 characters or fewer)

- What changed, and why. One bullet per change.
- Mention the problem each change fixes, if any.

Test results:
- What was tested and what happened, including anything still wrong.
```

Commits that don't change the mod's code, such as documentation or screenshots, don't need a version number. Describe them plainly, for example "Add screenshot to readme".

## Screenshots

Windhawk only shows readme images hosted on `raw.githubusercontent.com` or `i.imgur.com`, and they must use Markdown image syntax (`![description](url)`), not HTML `<img>` tags.

1. Add the image to `images/` with a simple file name (no spaces or special characters). Use test folders with neutral names, as screenshots are public.
2. In the mod's readme block, link to it with its full URL, for example `https://raw.githubusercontent.com/stoilms/windhawk-explorer-size-on-disk-column/main/images/screenshot1.png`. In `README.md`, use the relative path `images/screenshot1.png`.
3. Push the image before submitting to Windhawk, as its checks download every image.

Windhawk archives each image once and flags a submission if an archived image has changed. Never replace an existing image file; add a new one under a new name (for example `screenshot1-v0.6.png`) and update the links.

## Publishing to Windhawk

The mod is published through pull requests to [ramensoftware/windhawk-mods](https://github.com/ramensoftware/windhawk-mods). Each pull request must change only one file: `mods/explorer-size-on-disk-column.wh.cpp`.

1. Make sure the new version is committed and pushed here first, so this repository stays the source of truth.
2. In your fork of `windhawk-mods` (signed in as `stoilms`), sync it with the upstream repository using **Sync fork**.
3. Create a branch and replace `mods/explorer-size-on-disk-column.wh.cpp` with the file from this repository.
4. Commit it with a message describing the new version. Windhawk shows this in the mod's changelog, so write it for users, for example:
   ```
   Size on disk column in Explorer details v0.5.1

   - Fixed ...
   - Improved ...
   ```
5. Open a pull request to `ramensoftware/windhawk-mods`. Keep the template intact and list the changes between the changelog markers:
   ```
   <!-- changelog:start -->

   * Fixed ...
   * Improved ...

   <!-- changelog:end -->
   ```
   The "Mod authorship" section only applies to new mods, so leave it as it is for updates.
6. Wait for the automated checks. They verify the metadata (including that `@version` is new and `@github` matches the submitter), the readme and settings blocks, and that every readme image can be downloaded and archived. Fix any warnings by updating this repository first, then the pull request.
7. Respond to reviewer feedback in the same way. Once the pull request is merged, the new version appears in the Windhawk catalogue and installed copies are offered the update.

## Reporting problems

When opening an issue, include:

- your Windows version and build (run `winver`)
- the mod version
- the steps that cause the problem
- the mod's log with **Diagnostics** turned on, covering the moment the problem happens

## Licence

The mod includes code based on [Better file sizes in Explorer details](https://windhawk.net/mods/explorer-details-better-file-sizes) by m417z, which is licensed under the GPL-3.0, so this mod is too. By contributing, you agree that your contributions are licensed under the [GNU General Public License v3.0](LICENSE).