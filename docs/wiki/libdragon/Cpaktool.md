# cpaktool - Complete Guide

[**DOWNLOAD BINARY**](https://github.com/DragonMinded/libdragon/actions/runs/17142158932/artifacts/3824090218) (requires GitHub login)

## Introduction

`cpaktool` is a command-line tool for manipulating Nintendo 64 Controller Pak (Memory Card) files. 

The Controller Pak is an accessory for the N64 controller that provides 32 KiB of
memory for saving game data. Each pak can contain up to 16 "notes" (save files)
from different games.

`cpaktool` is developed as part of the Libdragon SDK (as it uses its open source library
to parse pak files) but is not directly bound to Libdragon in any way. It can be
used as standalone tool to manipulate pak files created by commercial games.

## Features

- **Full compatibility** with official Nintendo 64 Controller Paks
- **DexDrive format support** (with automatic header detection)
- **Complete filesystem operations**: list, extract, add, update, delete
- **Verification and repair** of filesystem integrity: recover lost notes, broken FAT chains, etc.
- **Support for multi-bank paks**, up to ~2 MiB in a single controller pak.
- **Formatting** of new Controller Paks
- **JSON output** for integration with other tools
- **Advanced handling** of encodings and special Japanese characters

## Installation

**Binary download**: You can download a zip files containing all Libdragon tools
compiled for Windows, including cpaktool, at the **link at the top of the page**.
Within the zip, you will find `cpaktool.exe`, which is the tool you are looking
for. You can ignore or delete all other files.

**Build from source**: cpaktool is compiled automatically as part of the Libdragon
SDK. To compile manually, first download the Libdragon preview branch from GitHub,
and then build the tool from the command line:

```bash
cd tools/cpaktool
make -C ..
```

This should work out of the box on Linux, macOS, mingw64 / msys2
or WSL on Windows. In general, building cpaktool only requires a working
C++ compiler on your system, as the source code is completely self contained
and does not depend on external libraries.

## General Syntax

```bash
cpaktool [GLOBAL OPTIONS] <command> [COMMAND OPTIONS] [ARGUMENTS...]
```

### Global Options

```
Global options:
  -h, --help      Show this help
  -V, --version   Show version information
  -v, --verbose   Verbose output
  -f, --force     Force operation without confirmation
  -n, --dry-run   Show what would be done without actually doing it
  --skip-header N     Skip N bytes at the beginning of pak files (0=no skip)
                      If not specified, DexDrive format is auto-detected
```

These options are valid for all subcommands.

## Supported formats and `--skip-header`

DexDrive controller pak dumps are normally spread around with the `.n64`
extensions and they have a custom header prepended to them.  Given the ubiquity
of this format, cpaktool by default will autodetect if the DexDrive format is
used by the pak and will be able to use them transparently.

For more advanced custom header support, you can use `--skip-header N` to
tell cpaktool then number of bytes to skip at the beginning of each file. This
should allow you to also work with Mupen64 4-in-1 saves, where 4 pak dumps
for the 4 controllers are stored in the same file. For instance, to access the
third pak, use `--skip-header 65536` (skip 2 paks of 32768 bytes each one).

## Available Commands

### 1. list (l, ls) - List Contents

The `list` command displays the contents of a Controller Pak, showing information about each note (save file) stored within. This is useful for viewing what games have saved data and how much space each file occupies. You can sort the output by name or size, show detailed information including CRC checksums, or output the data in JSON format for programmatic processing.

The command supports pattern matching to filter specific files, making it easy to find saves from particular games. The `--human-readable` option displays file sizes in a more readable format (K, M, etc.), while `--only-names` provides a minimal output showing just filenames.

```
Usage: cpaktool list [OPTIONS] <pak_file> [patterns...]

List contents of a Controller Pak file.

Options:
  -1, --only-names        Show only the file names, one per line
  -H, --human-readable    Show file sizes in human-readable format (e.g., 1K, 2M)
  --crc                   Show CRC32 checksum of file contents
  -s, --sort <key>        Sort by 'name' (default) or 'size'
  -r, --reverse           Reverse sort order
  -j, --json              Output in JSON format
```

**Example:**
```bash
$ cpaktool list DexDrive/cheatcc/super_mario.n64
Game      Pub   Filename  Ext  Size
-------------------------------------
3BADD1E5  FADE  SMSM           512

$ cpaktool list DexDrive/cheatcc/turok_2_seeds_of_evil.n64
Game  Pub  Filename                      Ext  Size
-----------------------------------------------------
NT2E  51   TUROK 2<NUL>51<NUL><NUL>NT2E  A    23040
```

The `--json` option creates a JSON output that can be useful for integration with
other tools. This is an example of such json:

```json
[
  {
    "game_code": "NDNE",
    "pub_code": "4X",
    "filename": "DUKE NUKEM L 25",
    "extension": "",
    "full_name": "NDNE.4X-DUKE NUKEM L 25",
    "size": 512
  }
]
```

### 2. extract (x) - File Extraction

The `extract` command retrieves files from a Controller Pak and saves them to your local filesystem. This is essential for backing up save data or transferring files between different Controller Paks. You can extract all files at once or use pattern matching to extract only specific files from certain games.

By default, files are extracted to the current directory using their Controller Pak filename format (`GAME.PUB-FILENAME.EXT`). The `--directory` option allows you to specify a different target location, while `--stdout` lets you pipe file contents directly to other programs for processing. The `--overwrite` option is required if you want to replace existing files.

```
Usage: cpaktool extract [OPTIONS] <pak_file> [patterns...]

Extract files from a Controller Pak file.

Options:
  -o, --overwrite         Overwrite existing files
  -d, --directory <dir>   Extract files to the specified directory
  -s, --stdout            Extract file contents to stdout instead of files
```

**Example:**
```bash
$ cpaktool extract -d temp_extract DexDrive/cheatcc/super_mario.n64
$ ls -la temp_extract/
total 8
drwxr-xr-x   3 rasky  staff    96 Aug 21 01:26 ./
drwxr-xr-x  62 rasky  staff  1984 Aug 21 01:26 ../
-rw-r--r--   1 rasky  staff   512 Aug 21 01:26 3BADD1E5.FADE-SMSM
```

### 3. add (a) - Add Files

The `add` command allows you to add new files to a Controller Pak or update existing ones. Files must follow the Controller Pak naming convention: `GAME.PUB-FILENAME.EXT`, where GAME is a 4-character game code, PUB is a 2-character publisher code, FILENAME is the note name (1-16 characters), and EXT is the extension (1-4 characters). See below for more information.

If the Controller Pak doesn't exist, you can use `--create` to make a new one, optionally specifying its size with `--size`. The `--update` flag allows overwriting existing files, while `--gamecode` provides a default game/publisher code for files that don't follow the naming convention. The tool will automatically manage the filesystem structure and update the file allocation table.

```
Usage: cpaktool add [OPTIONS] <pak_file> <files...>

Add files to a Controller Pak file.

Options:
  -c, --create            Create a new pak file if it doesn't exist
  -s, --size <KB>         Size of the new pak file in kilobytes (default: 32)
  -u, --update            Update existing files instead of erroring
  -g, --gamecode <id>     Default game/publisher code (e.g., 'DRAG.ON')
      --partial           Keep partially written files on error
```

**Example:**
```bash
$ echo "Hello world!" > TEST.01-SAMPLE.TXT
$ cpaktool add test_wiki.pak TEST.01-SAMPLE.TXT
$ cpaktool list test_wiki.pak
Game  Pub  Filename  Ext  Size
--------------------------------
TEST  01   SAMPLE    TXT  13
```

### 4. delete (d, rm) - Delete Files

The `delete` command removes files from a Controller Pak, freeing up space for new saves. It supports pattern matching, so you can delete all files from a specific game or files matching certain criteria. This is useful for cleaning up old saves or removing corrupted data.

The `--interactive` option provides a safety mechanism by prompting for confirmation before each deletion, which is recommended when using broad patterns to avoid accidentally removing important saves.

```
Usage: cpaktool delete [OPTIONS] <pak_file> <patterns...>

Delete files from a Controller Pak file.

Options:
  -i, --interactive       Prompt before every removal
```

**Example:**
```bash
$ cpaktool delete test_wiki.pak TEST.01-SAMPLE.TXT
Deleted: TEST.01-SAMPLE.TXT
$ cpaktool list test_wiki.pak
Game  Pub  Filename  Ext  Size
--------------------------------

Found 0 files
```

### 5. info (i) - Pak Information

The `info` command provides detailed information about a Controller Pak's structure and contents. It shows the total capacity, used space, number of files, and filesystem health. This command is useful for getting an overview of a pak's status and understanding its storage layout.

The `--json` option outputs structured data that can be easily parsed by scripts or other tools for automated analysis and reporting.

```
Usage: cpaktool info [OPTIONS] <pak_file>

Show information about a Controller Pak file.

Options:
  -j, --json              Output in JSON format
```

**Example:**
```bash
$ cpaktool info DexDrive/cheatcc/super_mario.n64
Pak file: DexDrive/cheatcc/super_mario.n64
Header bytes: 4160
Banks: 1
Pages: 2 used / 123 total (1.63%)
Notes: 1 used / 16 total (6.25%)

Bank statistics:
  Bank 0: 2 pages used (1.57%), 1 notes, fragmentation: 0.032

Notes:
  3BADD1E5.FADE-SMSM
    512 bytes
    CRC32: 0C9DCCE3
    FAT: 00:05-00:06
    Fragmentation: pages: 0.00%, banks: 0.00%

Weighted fragmentation index: 0.64% (span=0.00%, banks=0.00%, free=3.20%)

FSCK issues: 7
  [2] INFO: ID sector has a known-corrupted checksum generated by DexDrive
  [3] INFO: ID sector backup is corrupted (can be recovered from main)
  [3] INFO: ID sector backup is corrupted (can be recovered from main)
  [3] INFO: ID sector backup is corrupted (can be recovered from main)
  [7] INFO: Reserved page 1 has an invalid FAT entry (0x0013)
  [7] INFO: Reserved page 2 has an invalid FAT entry (0x803f)
  [7] INFO: Reserved page 3 has an invalid FAT entry (0x7540)
```

### 6. test (t, fsck) - Integrity Check

The `test` command performs filesystem integrity checks on a Controller Pak, similar to fsck on Unix systems. It verifies the consistency of the file allocation table, checks for corruption in the note database, and validates file chains. This is essential for diagnosing problems with corrupted Controller Paks.

The `--repair` option attempts to fix any issues found, such as repairing broken file chains or correcting checksum errors. The `--level` option controls the verbosity of reporting, from showing only errors to displaying detailed information about every check performed.

```
Usage: cpaktool test [OPTIONS] <pak_file>

Test and optionally repair the filesystem integrity of a Controller Pak file.

Options:
  -r, --repair            Attempt to fix any issues found
  --level <level>         Set report level: INFO, WARNING, ERROR (default: WARNING)
```

**Example:**
```bash
$ cpaktool test DexDrive/cheatcc/super_mario.n64
No issues found
```

### 7. format (fmt) - Formatting

The `format` command creates a new, empty Controller Pak file with a properly initialized filesystem. This is necessary when creating new Controller Paks from scratch or when an existing pak is too corrupted to repair.

You can specify the size of the pak using `--size` (in kilobytes) or `--banks` (where each bank is 32KB). Standard Controller Paks are 32KB (1 bank), but some third-party paks support larger sizes. The `--force` option is required to overwrite an existing file.

```
Usage: cpaktool format [OPTIONS] <pak_file>

Create and format a new Controller Pak file.

Options:
  -s, --size <KB>         Size of the new pak file in kilobytes (default: 32)
  -b, --banks <num>       Number of banks for the new pak file (1 bank = 32 KB)
  -f, --force             Overwrite the file if it already exists
```

**Example:**
```bash
$ cpaktool format test_wiki.pak
Format completed
$ cpaktool list test_wiki.pak
Game  Pub  Filename  Ext  Size
--------------------------------

Found 0 files
```

### 3. Filesystem Structure
Each "note" (file) in the Controller Pak has:
- **Game Code**: 4 characters identifying the game
- **Publisher Code**: 2 characters identifying the publisher
- **Filename**: 1-16 characters for the file name
- **Extension**: 1-4 characters for the extension
- **Data**: Actual file content (maximum 123 blocks = ~3.8KB)

## Note names

As seen before, each note in the pak has four components that contribute to its
full name:
 
  * The game code. This is a 4-letter code which is also present in the
    ROM header (like eg: NTMC). All commercial games only used uppercase letters
    for this field, but nothing prevents other bytes from being used. Notably,
    Datel Gameshark saved notes with game code containing binary values.
  * The publisher code. This is a 2-letter code which was assigned by Nintendo
    to all game publishers, normally a progressive two digit number. Like the
    game code, nothing prevents binary values to be used here.
  * The filename. This was the main component of the note name, up to 16 letters,
    in a custom codepage that contained only a small subset of ASCII, plus
    some Kanji glyphs. Normally, the game name was put here.
  * The extension. An optional field, up to 4 letters, in the same custom
    codepage of the filename. This was sometimes used to disambiguate multiple
    saves of the same game (using progressive numbers or letters).

Pak managers would normally show only the filename and extension of each note,
while the game code and publisher code were treated as internal "tags" and not
displayed. This is also the reason why there is a well defined codepath for
fileame and extension: to allow mapping bytes to a font glyphs.

Internally, instead, games could use a game-specific logic to find notes.
Some games only search for specific filenames, others only search for game code
and publisher code, and then open all notes matching those two ignoring the
filename.

## Note names and filenames on PC

cpaktool allows you to extract and add notes to a pak file. In doing so, it needs
a way to map the four name components (game code, publisher code, filename and
extensions) to a filename that is valid on modern PCs and filesystems.

The PC filename will be something like `NB7E.01-SAVEGAME.A`, where:

* `NB7E` is the game code. If the gamecode contains anything which is not a
letter or a number, it will be automatically encoded in hex (eg: `267EC768`),
allowing also binary data to be used.
* `01` is the publisher code. Like the gamecode, if it contains anything whic
is not a letter o a number, it will be automatically encoded in hex (eg: `DA4F`).
* `SAVEGAME` and `A` are the filename and optional extensions. They are converted 
automatically to Unicode so that also Kanjis can be used (`DOUB.01-どうぶつの森.A`).
The special codepage used in paks allow to also include characters that are not
valid in filenames, such as `/`. In that case, we escape them using percent
encoding (eg: `DRAG.ON-FILE%2F%3F%2A.BIN` will be created to extract a note
whose filename is `FILE/?*.BIN`).

This encoding is valid for both the `extract` and `add` commands, so you will
be able to extract a note with any name, and then add it back to the same or
another pak, reproducing exactly the original name.

**Examples of valid names:**
- `MARIO.01-SAVE.A` (standard English)
- `DOUB.01-どうぶつの森.A` (with kanji)
- `TEST.01-FOO%00BAR.TXT` (with NUL character in the filename)
- `DEADBEEF.FADE-GAME.BIN` (with binary gamecode and pubcode, encoded in hex)

## Pattern Matching

cpaktool supports shell-style patterns for file selection:

- `*` matches any sequence of characters
- `?` matches any single character  
- `[abc]` matches any character in the brackets
- `[a-z]` matches any character in the range

Examples: `NO7P.*` (all GoldenEye files), `SM?E.01-*`, `[NS]M*.*`

## Integration with Other Tools

cpaktool can be easily integrated into scripts and workflows:

```bash
#!/bin/bash
# Automatic backup script

PAK_FILE="controller_pak.n64"
BACKUP_DIR="./backups/$(date +%Y%m%d_%H%M%S)"

# Verify integrity
if cpaktool test "$PAK_FILE"; then
    echo "✓ Controller Pak OK"
    
    # Extract with JSON output for processing
    cpaktool list -j "$PAK_FILE" > pak_info.json
    cpaktool extract -d "$BACKUP_DIR" "$PAK_FILE"
    
    echo "✓ Backup completed in $BACKUP_DIR"
else
    echo "✗ Controller Pak damaged, attempting repair..."
    cpaktool test -r "$PAK_FILE"
fi
```

## Contributions and Development

cpaktool is part of the open source Libdragon project. To contribute:

1. **Repository**: https://github.com/DragonMinded/libdragon
2. **Issues**: Report bugs or request features
3. **Pull Requests**: Contribute with code and documentation

## License

cpaktool is released into the public domain (Unlicense). You can use it freely
for any purpose.
