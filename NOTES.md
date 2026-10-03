# Working notes

Kept out of the repository (`.git/info/exclude`), like the other projects' notes.

## Building and testing

- `python3 build.py` builds `bin/bbs.prg` and `bin/BBS.D81` and prints how much
  room the soft stack has below `$D000`. It refuses to link under 1 KB: the
  client once shipped with 174 bytes, and main's frame reached back into
  `.bss`, so the config text overwrote the boot drive number and every save
  went to a drive that was not there (about 20 s of retries per sector, which
  looked like a hang when adding a site).
- `python3 build.py test` runs the seven host tests. The transfer tests drive
  the real `zmodem.c` and `xmodem.c` through a scripted peer that streams
  subpackets, checks CRCs, corrupts one on request, cancels, stays silent, and
  accepts at most 100 bytes per `net_send` until the next poll (the shape of
  the stack's 3 KB send buffer).
- `tools/bbs_test_server.py` is a small telnet board with real IAC framing,
  ZMODEM as lrzsz does it, and XMODEM both ways. Options: `--inject-errors`
  (one bad subpacket or block per transfer), `--hex-headers`, `--raw`,
  `--file`. It creates `test_files/TEST20K.BIN`, a fixed 20 KB file with every
  byte that needs escaping in it. `tools/test_zmodem_loopback.py` drives the
  server from Python (start it with `--raw --mode ansi`).
- `tools/lrzsz_bridge.py` runs the real `sz`, `rz`, `lsz --xmodem` and
  `lrz --xmodem` (Homebrew lrzsz) behind telnet framing. `tests/zmodem_live.c`
  runs the real modules over TCP against it or the Python server; see its
  header for the arguments. Both were byte-identical in every mode.

## Memory layout that the code depends on

- Bank 1: screen `$10000`, CP437 font `$11000`, stored sites `$11800`
  (16 x 69), file picker listing `$11C50` (32 x 20), one transfer block
  `$12000` (1 KB) held until its CRC passes. Reached through `m65_far.h`.
- Low RAM: `$1400`/`$1500` shared by the config loader and the transfers
  (never live at the same time), `$1600` mega-net, `$1800`-`$1AFF` CBM DOS,
  `$1B00`-`$1CFF` F011 sector buffer, `$1D00` rx_buf, `$1D80` telnet pushback.

## Rendering

- Characters are collected into runs (`m65_screen_putc_buf`) and flushed with
  two DMA copies; every other screen primitive flushes first. The renderers
  flush at the end of each received packet (`ansi_flush`, `petscii_flush`).
- The colour byte's high nibble holds the VIC-III attributes (0x10 blink,
  0x20 reverse, 0x80 underline); ATTR is set in `$D031`. ANSI draws a coloured
  background as the cell reversed in that colour, since the text mode has one
  background for the whole screen.
- The cursor is one cell's reverse bit toggled by `m65_screen_cursor_tick`
  every 16 frames; anything drawn under it hides it first.
- Resolution 4 (40 in 80) is an 80x25 screen with PETSCII in a 40-column
  window at column 20 and NAWS reporting 40 columns; ANSI ignores the window.
- `tools/bbs_test_server.py` option 8 shows an ANSI rendering test screen.

## Protocol notes

- ZMODEM offers a 1024-byte subpacket limit and no CRC-32 in ZRINIT; a
  sender only uses CRC-32 when asked. 0xFF goes as ZDLE `m`, 0x7F as ZDLE `l`.
- Telnet keeps IAC doubled even in BINARY mode, so the transfer paths always
  collapse and double it; BINARY is requested so servers stop translating CR.
- XMODEM keeps the sender's 0x1A padding (the protocol carries no length).

## Modem speed emulation

- Per site (file field 6, a baud rate, 0 for unlimited) and F3 in a session.
  `meter_feed` in main.c feeds received bytes at the rate in sixteenths of a
  character per frame (10, 38, 77, 307 for 300..9600 at 50 Hz); keys and
  transfers are never metered. Measured on the MEGA65 against Retrocampus:
  the Files screen (about 250 characters to the marker) took 8 s at 300 baud.

## Boards

- Retrocampus (bbs.retrocampus.com 6510, Petscii BBS Builder) is a plain
  socket: no telnet negotiation at all. Its XMODEM sender waits for NAK only,
  checksum, 128-byte blocks, ten errors, and ends on the first non-ACK after
  EOT. It fetches the file from csdb after printing "please start", so the
  first block can come 15 s later. The client decides telnet or raw from the
  first bytes of a connection, ACKs EOT at once, and takes the check mode
  from the block. `tests/zmodem_live.c` can drive it: keys `4~1~r\r~1\r~ `
  ('~' pauses), prompt `ATRONS`, wait `LEASE START` (PETSCII lowercase reads
  as ASCII uppercase in the log).
- XMODEM has no length: a downloaded file carries up to 127 bytes of 0x1A
  padding (Retrocampus says 5955 bytes, the file is 6016).

## Driving the client from the Mac

- Names typed through the serial driver must be lowercase
  (`boot_prg bbs.d81 bbs 'dialing directory'`); uppercase letters are dropped.
- The file picker wants cursor keys; `m` then a typed name avoids them.
- The test server on this Mac is 192.168.1.232 port 6400; it is no longer in
  the default site list (add it with A, or Quick-Dial). F7 is matrix code 03
  for tap.py; ESC is 47.
- Sites carry no drive number since 2026-09-27; F7 toggles the working drive
  (footer in the directory, the transfer menu's title in a session).
