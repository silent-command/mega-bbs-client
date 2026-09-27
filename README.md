# MEGA65 BBS Client

A native, high-performance BBS client for the MEGA65 built on the `mega-net` TCP/IP stack.

## Highlights & Features

* **Three Standard Screen Resolutions**: Full native support for **40x25**, **80x25**, and **80x50** text modes via direct VIC-IV register control and DMAgic block operations.
* **Dual-Engine Graphics (PETSCII & ANSI)**:
  * **PETSCII Mode (Default)**: Full support for Commodore color codes, reverse video, cursor addressing, and character set switching (`$0E`/`$8E`) using the ROM character generator.
  * **ANSI Mode (CP437)**: Authentic IBM PC Code Page 437 8x8 font loaded into Bank 1 RAM (`$11000`) for pixel-perfect box-drawing and shaded characters, driven by a full VT100/ANSI.SYS escape sequence parser.
  * **24-bit PC VGA Palette**: Programs VIC-IV palette registers (`$D100-$D3FF`, Bank 1) to match authentic 16-color PC VGA RGB values in ANSI mode, and switches to Commodore palette (Bank 0) in PETSCII mode.
  * **Standard Startup Palette**: Guaranteed clean launch with Border = Black (0), Background = Black (0), and Foreground = White (1).
* **Keyboard Shortcuts & Function Keys**:
  * **Dialing Directory**:
    * **[Return]**: Connect to highlighted BBS site
    * **[A]**: Add new bookmark
    * **[E]**: Edit selected bookmark (Host, Port, Emulation, Resolution, Drive)
    * **[D]**: Delete selected bookmark
    * **[J]**: Jump directly to entry number
    * **[Q]** / **F2**: Quick-Dial (direct host:port connection)
    * **F3**: Cycle screen text mode (**40x25** -> **80x25** -> **80x50**)
    * **[X]** / **[Run/Stop]**: Exit to BASIC
  * **Online / Terminal Mode**:
    * **F1**: Disconnect / Hang Up (returns to Dialing Directory)
    * **F3**: Cycle text mode on the fly (**40x25** -> **80x25** -> **80x50**)
    * **F5**: File Transfer Menu & Disk File Picker (Upload / Manual Download)
* **Bookmarks & Dialing Directory**: Save, add, edit, and delete BBS connections (name, host, port, emulation, default resolution, target drive) persisted to `BBSCFG` on disk. Ships pre-populated with popular CBM and ANSI BBSes.
* **ZModem File Transfer**:
  * **Streaming Direct-to-Disk**: Streams incoming and outgoing file blocks directly to/from floppy or D81 disk images using `m65_cbmdos`, bypassing RAM limits and supporting multi-megabyte transfers.
  * **Auto-Detect**: Automatically intercepts BBS `ZRQINIT` / `rz` download handshakes and starts downloading seamlessly.
  * **Drive Selection**: User-selectable target drive (Drive 8 or 9).
  * **Visual HUD**: On-screen transfer status display showing filename, progress bar, percentage, and CPS.
* **Telnet Negotiation**: RFC 854 IAC handler with RFC 1073 NAWS (window size updates sent dynamically when resolution changes) and RFC 856 8-bit Binary Mode.

---

## Directory Structure

```
├── build.py                # Python build and test driver
├── assets/
│   ├── cp437.bin           # 2,048-byte IBM PC CP437 8x8 font
│   └── bbscfg.txt          # Default bookmarks configuration
├── bin/
│   ├── bbs.prg             # Compiled MEGA65 client binary
│   └── BBS.D81             # Bootable D81 disk image containing bbs, meganet, cp437, bbscfg
├── include/                # mega65-libc headers
├── llvm-mos/               # llvm-mos toolchain
├── mega-net/               # Native MEGA65 TCP/IP stack
├── src/
│   ├── platform/           # Hardware platform drivers (screen, font, F011, CBM DOS, boot, exit)
│   ├── ansi.c / .h         # ANSI.SYS / VT100 escape parser & SGR color mapper
│   ├── petscii.c / .h      # PETSCII control codes & screencode translator
│   ├── telnet.c / .h       # Telnet IAC engine & NAWS window sizing
│   ├── bookmarks.c / .h    # Bookmarks storage & persistence
│   ├── zmodem.c / .h       # Streaming ZModem transfer engine (download/upload)
│   ├── ui.c / .h           # Dialing directory UI, menus, and progress overlays
│   └── main.c              # Application bootstrap and terminal event loop
├── tests/                  # Host unit tests (screen, ANSI, PETSCII, Telnet, bookmarks, ZModem)
└── tools/
    ├── orphan_rmw.py       # llvm-mos code generation validator
    └── bbs_test_server.py  # Local mock BBS server for testing
```

---

## Commands

### 1. Build Client and Disk Image
```bash
python3 build.py
```
Compiles `bin/bbs.prg` using `mos-mega65-clang` (`-Oz`) and creates `bin/BBS.D81` containing:
* `bbs` (the client executable)
* `meganet` (the mega-net TCP/IP stack image)
* `cp437` (the IBM PC CP437 font binary)
* `bbscfg` (the bookmarks sequential file)

### 2. Run Host Test Suite
```bash
python3 build.py test
```
Runs all 6 unit tests covering screen modes, ANSI parsing, PETSCII processing, Telnet negotiation, bookmarks persistence, and ZModem framing.

### 3. Run Local Mock BBS Server
```bash
python3 tools/bbs_test_server.py --port 6400 --mode petscii
# Or for ANSI testing:
python3 tools/bbs_test_server.py --port 6400 --mode ansi
```

---

## Hardware Execution

Deploy `bin/BBS.D81` to your MEGA65 via SD card, or inject using the `m65` cross-development tool:
```bash
source mega-net/tools/m65lib.sh
put_d81 bin/BBS.D81 BBS.D81
boot_prg bbs.d81 bbs
```
