#!/usr/bin/env python3
"""A small telnet BBS for testing the MEGA65 BBS client's transfers.

    python3 tools/bbs_test_server.py [--port 6400] [--mode petscii|ansi]
                                     [--file test_files/TEST20K.BIN]
                                     [--inject-errors] [--hex-headers] [--raw]

Telnet framing is real (IAC doubled and collapsed, options answered) so
the client is tested as a board would treat it; --raw turns that off.
ZMODEM follows lrzsz: 0xFF escaped as ZDLE 'm', escapes checked, every
subpacket CRC verified with ZRPOS on failure, ZDATA and ZFILE as binary
headers (--hex-headers for the old form), 1024-byte subpackets or what
the client's ZRINIT allows, ZRINIT flags in ZF0. XMODEM: 128-byte and 1K
downloads, CRC or checksum as the client asks, uploads to test_uploads.
--inject-errors corrupts one subpacket or block per transfer so the
client's recovery is exercised.

Menu:  1 echo   2 ZMODEM download   3 ZMODEM upload   4 list uploads
       5 XMODEM download   6 XMODEM-1K download   7 XMODEM upload   Q
"""
import argparse
import os
import socket
import sys
import time

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
OPT_BINARY, OPT_ECHO, OPT_SGA, OPT_TTYPE, OPT_NAWS = 0, 1, 3, 24, 31

ZPAD, ZDLE, ZBIN, ZHEX = ord('*'), 0x18, ord('A'), ord('B')
ZCRCE, ZCRCG, ZCRCQ, ZCRCW, ZRUB0, ZRUB1 = ord('h'), ord('i'), ord('j'), ord('k'), ord('l'), ord('m')
ZRQINIT, ZRINIT, ZSINIT, ZACK, ZFILE, ZSKIP, ZNAK, ZABORT, ZFIN, ZRPOS, ZDATA, ZEOF = range(12)
CANFDX, CANOVIO, CANFC32 = 0x01, 0x02, 0x20
SOH, STX, EOT, ACK, NAK, CAN = 1, 2, 4, 6, 0x15, 0x18

OPTIONS = {"inject": False, "hex_headers": False}


def z_crc16(crc, b):
    crc ^= (b << 8)
    for _ in range(8):
        crc = ((crc << 1) ^ 0x1021) & 0xffff if crc & 0x8000 else (crc << 1) & 0xffff
    return crc


class ProtocolError(Exception):
    pass


class TelnetLink:
    """The socket with telnet framing: doubling out, collapsing and
    answering options in. With raw=True it is a plain byte pipe."""

    def __init__(self, sock, raw=False):
        self.sock = sock
        self.raw = raw
        self.state = 0
        self.cmd = 0
        self.pending = bytearray()      # data bytes already filtered
        self.sb = bytearray()
        self.sb_opt = None
        self.ttype = None

    def send(self, data):
        data = bytes(data)
        if not self.raw:
            data = data.replace(b"\xff", b"\xff\xff")
        self.sock.sendall(data)

    def send_text(self, text):
        self.send(text.encode("latin1"))

    def negotiate(self):
        if self.raw:
            return
        self.sock.sendall(bytes([IAC, WILL, OPT_ECHO, IAC, WILL, OPT_SGA, IAC, DO, OPT_TTYPE, IAC, DO, OPT_NAWS,
                                 IAC, WILL, OPT_BINARY, IAC, DO, OPT_BINARY]))
        self.sock.sendall(bytes([IAC, SB, OPT_TTYPE, 1, IAC, SE]))

    def _filter(self, raw):
        if self.raw:
            self.pending.extend(raw)
            return
        for b in raw:
            if self.state == 0:
                if b == IAC:
                    self.state = 1
                else:
                    self.pending.append(b)
            elif self.state == 1:
                if b == IAC:
                    self.pending.append(0xff)
                    self.state = 0
                elif b in (DO, DONT, WILL, WONT):
                    self.cmd = b
                    self.state = 2
                elif b == SB:
                    self.state = 3
                    self.sb = bytearray()
                    self.sb_opt = None
                else:
                    self.state = 0
            elif self.state == 2:
                if self.cmd == DO:
                    self.sock.sendall(bytes([IAC, WILL if b in (OPT_BINARY, OPT_SGA, OPT_ECHO) else WONT, b]))
                elif self.cmd == WILL:
                    self.sock.sendall(bytes([IAC, DO if b in (OPT_BINARY, OPT_SGA, OPT_TTYPE, OPT_NAWS) else DONT, b]))
                self.state = 0
            elif self.state == 3:
                if b == IAC:
                    self.state = 4
                elif self.sb_opt is None:
                    self.sb_opt = b
                else:
                    self.sb.append(b)
            elif self.state == 4:
                if b == SE:
                    if self.sb_opt == OPT_TTYPE and self.sb[:1] == b"\x00":
                        self.ttype = bytes(self.sb[1:]).decode("latin1", "ignore")
                    self.state = 0
                elif b == IAC:
                    self.sb.append(0xff)
                    self.state = 3
                else:
                    self.state = 0

    def recv_byte(self, timeout=10.0):
        """One data byte, or None on timeout; ConnectionError when closed."""
        end = time.time() + timeout
        while not self.pending:
            left = end - time.time()
            if left <= 0:
                return None
            self.sock.settimeout(left)
            try:
                raw = self.sock.recv(4096)
            except socket.timeout:
                return None
            if not raw:
                raise ConnectionError("connection closed")
            self._filter(raw)
        b = self.pending[0]
        del self.pending[0]
        return b

    def unrecv_byte(self, b):
        self.pending.insert(0, b)

    def drain(self, quiet=0.3):
        while self.recv_byte(quiet) is not None:
            pass


class ZModemIO:
    """ZMODEM framing over a TelnetLink, as lrzsz does it."""

    def __init__(self, link):
        self.link = link
        self.peer_bufsize = 1024

    def send_raw(self, data):
        self.link.send(data)

    def recv_byte(self, timeout=10.0):
        b = self.link.recv_byte(timeout)
        if b is None:
            raise TimeoutError("timeout waiting for a byte")
        return b

    def unrecv_byte(self, b):
        self.link.unrecv_byte(b)

    def send_escaped(self, b):
        if b == 0xff:
            self.send_raw(bytes([ZDLE, ZRUB1]))
        elif b == 0x7f:
            self.send_raw(bytes([ZDLE, ZRUB0]))
        elif b in (ZDLE, 0x11, 0x13, 0x91, 0x93):
            self.send_raw(bytes([ZDLE, b ^ 0x40]))
        else:
            self.send_raw(bytes([b]))

    def recv_escaped(self, timeout=10.0):
        """('data', byte), ('delimit', ZCRCx), or ProtocolError for an
        escape a real receiver refuses; XON/XOFF are dropped."""
        while True:
            b = self.recv_byte(timeout)
            if b in (0x11, 0x13, 0x91, 0x93):
                continue
            if b != ZDLE:
                return 'data', b
            cans = 0
            while True:
                b2 = self.recv_byte(timeout)
                if b2 != ZDLE:
                    break
                cans += 1
                if cans >= 4:
                    raise ProtocolError("cancelled by the peer")
            if b2 in (ZCRCE, ZCRCG, ZCRCQ, ZCRCW):
                return 'delimit', b2
            if b2 == ZRUB0:
                return 'data', 0x7f
            if b2 == ZRUB1:
                return 'data', 0xff
            if (b2 & 0x60) == 0x40:
                return 'data', b2 ^ 0x40
            raise ProtocolError(f"bad escape sequence ZDLE {b2:02x}")

    def _header_bytes(self, frame_type, flags):
        return [frame_type, flags & 0xff, (flags >> 8) & 0xff, (flags >> 16) & 0xff, (flags >> 24) & 0xff]

    def send_hex_header(self, frame_type, flags=0):
        h = self._header_bytes(frame_type, flags)
        crc = 0
        for b in h:
            crc = z_crc16(crc, b)
        hdr = "**\x18B" + "".join(f"{b:02x}" for b in h) + f"{crc:04x}\r\n"
        if frame_type not in (ZACK, ZFIN):
            hdr += "\x11"
        self.send_raw(hdr.encode('latin1'))

    def send_bin_header(self, frame_type, flags=0):
        h = self._header_bytes(frame_type, flags)
        crc = 0
        self.send_raw(bytes([ZPAD, ZDLE, ZBIN]))
        for b in h:
            crc = z_crc16(crc, b)
            self.send_escaped(b)
        self.send_escaped(crc >> 8)
        self.send_escaped(crc & 0xff)

    def send_data_header(self, frame_type, flags=0):
        """ZFILE and ZDATA: binary as lrzsz sends them, hex on request."""
        if OPTIONS["hex_headers"]:
            self.send_hex_header(frame_type, flags)
        else:
            self.send_bin_header(frame_type, flags)

    def recv_header(self, timeout=10.0):
        """The next header of either form, scanning past anything else.
        Returns (type, flags)."""
        end = time.time() + timeout
        while time.time() < end:
            b = self.link.recv_byte(min(1.0, max(0.05, end - time.time())))
            if b is None:
                continue
            if b != ZPAD:
                continue
            b = self.recv_byte(2.0)
            while b == ZPAD:
                b = self.recv_byte(2.0)
            if b != ZDLE:
                continue
            fmt = self.recv_byte(2.0)
            if fmt == ZHEX:
                digits = "".join(chr(self.recv_byte(2.0)) for _ in range(14))
                try:
                    h = [int(digits[i:i + 2], 16) for i in range(0, 14, 2)]
                except ValueError:
                    continue
                crc = 0
                for x in h:
                    crc = z_crc16(crc, x)
                if crc != 0:
                    print("[zmodem] hex header with a bad CRC", flush=True)
                    continue
                while True:
                    c = self.link.recv_byte(0.05)
                    if c is None:
                        break
                    if c not in (10, 13, 0x8a, 0x11):
                        self.unrecv_byte(c)
                        break
            elif fmt == ZBIN:
                h = []
                crc = 0
                for _ in range(7):
                    kind, v = self.recv_escaped(2.0)
                    if kind != 'data':
                        break
                    h.append(v)
                    crc = z_crc16(crc, v)
                if len(h) != 7 or crc != 0:
                    print("[zmodem] binary header with a bad CRC", flush=True)
                    continue
            else:
                continue
            return h[0], h[1] | (h[2] << 8) | (h[3] << 16) | (h[4] << 24)
        raise TimeoutError("timeout waiting for a ZMODEM header")

    recv_hex_header = recv_header

    def send_subpacket(self, data, delim, corrupt=False):
        crc = 0
        for i, b in enumerate(data):
            crc = z_crc16(crc, b)
            self.send_escaped(b ^ 0x55 if corrupt and i == len(data) // 2 else b)
        self.send_raw(bytes([ZDLE, delim]))
        crc = z_crc16(crc, delim)
        self.send_escaped(crc >> 8)
        self.send_escaped(crc & 0xff)

    def recv_subpacket(self, timeout=10.0):
        """(data, delim), or ProtocolError on a bad CRC."""
        data = bytearray()
        crc = 0
        while True:
            kind, v = self.recv_escaped(timeout)
            if kind == 'delimit':
                break
            data.append(v)
            crc = z_crc16(crc, v)
            if len(data) > 8192:
                raise ProtocolError("subpacket too long")
        crc = z_crc16(crc, v)
        for _ in range(2):
            kind, c = self.recv_escaped(timeout)
            if kind != 'data':
                raise ProtocolError("delimiter inside a CRC")
            crc = z_crc16(crc, c)
        if crc != 0:
            raise ProtocolError("subpacket CRC error")
        return bytes(data), v

    def header_pending(self):
        """A ZPAD waiting from the peer between subpackets (a ZRPOS)."""
        b = self.link.recv_byte(0.0)
        if b is None:
            return False
        self.unrecv_byte(b)
        return b == ZPAD


def zmodem_send_file(link, filename, data):
    """Sends a file to the client, as sz does."""
    zio = ZModemIO(link)
    print(f"[zmodem] sending {filename} ({len(data)} bytes)", flush=True)
    link.send(b"rz\r")
    zio.send_hex_header(ZRQINIT, 0)
    ftype, flags = zio.recv_header(15.0)
    while ftype == ZRQINIT:
        ftype, flags = zio.recv_header(15.0)
    if ftype != ZRINIT:
        raise ProtocolError(f"expected ZRINIT, got {ftype}")
    bufsize = flags & 0xffff
    chunk = min(1024, bufsize) if bufsize else 1024
    print(f"[zmodem] client ZRINIT: buffer {bufsize}, flags {(flags >> 24) & 0xff:#x}; {chunk}-byte subpackets", flush=True)

    for attempt in range(10):
        zio.send_data_header(ZFILE, 0)
        zio.send_subpacket(filename.encode('latin1') + b"\x00" + f"{len(data)} 0".encode('latin1'), ZCRCW)
        ftype, pos = zio.recv_header(15.0)
        if ftype == ZRPOS:
            break
        if ftype == ZSKIP:
            zio.send_hex_header(ZFIN, 0)
            raise ProtocolError("the receiver skipped the file")
        if ftype not in (ZRINIT, ZNAK):
            raise ProtocolError(f"expected ZRPOS, got {ftype}")
    else:
        raise ProtocolError("no ZRPOS")

    corrupted = False
    while True:
        zio.send_data_header(ZDATA, pos)
        interrupted = False
        while pos < len(data):
            n = min(chunk, len(data) - pos)
            corrupt = OPTIONS["inject"] and not corrupted and pos >= 2048
            zio.send_subpacket(data[pos:pos + n], ZCRCE if pos + n == len(data) else ZCRCG, corrupt)
            if corrupt:
                corrupted = True
                print("[zmodem] injected a bad subpacket", flush=True)
            pos += n
            if zio.header_pending():
                ftype, newpos = zio.recv_header(5.0)
                if ftype == ZRPOS:
                    print(f"[zmodem] ZRPOS {newpos} from the client", flush=True)
                    pos = newpos
                    interrupted = True
                    break
        if interrupted:
            continue
        zio.send_hex_header(ZEOF, len(data))
        ftype, newpos = zio.recv_header(15.0)
        if ftype == ZRINIT:
            break
        if ftype == ZRPOS:
            print(f"[zmodem] ZRPOS {newpos} from the client after ZEOF", flush=True)
            pos = newpos
            continue
        raise ProtocolError(f"expected ZRINIT after ZEOF, got {ftype}")
    zio.send_hex_header(ZFIN, 0)
    ftype, _ = zio.recv_header(10.0)
    if ftype == ZFIN:
        link.send(b"OO")
    print(f"[zmodem] sent {filename}", flush=True)


def zmodem_recv_file(link, output_dir="test_uploads"):
    """Receives a file from the client, as rz does. Returns (name, size)."""
    os.makedirs(output_dir, exist_ok=True)
    zio = ZModemIO(link)
    print("[zmodem] waiting for the client's ZRQINIT", flush=True)
    rinit = (CANFDX | CANOVIO) << 24 | 1024
    ftype, _ = zio.recv_header(60.0)
    if ftype != ZRQINIT:
        raise ProtocolError(f"expected ZRQINIT, got {ftype}")
    zio.send_hex_header(ZRINIT, rinit)

    filename, file_size, file_data, pos = "UPLOAD.BIN", 0, bytearray(), 0
    errors = 0
    while True:
        try:
            ftype, flags = zio.recv_header(15.0)
        except TimeoutError:
            errors += 1
            if errors > 10:
                raise
            zio.send_hex_header(ZRPOS if file_size else ZRINIT, pos if file_size else rinit)
            continue
        if ftype == ZRQINIT:
            zio.send_hex_header(ZRINIT, rinit)
        elif ftype == ZFILE:
            try:
                info, _ = zio.recv_subpacket()
            except ProtocolError as e:
                print(f"[zmodem] ZFILE: {e}", flush=True)
                zio.send_hex_header(ZNAK, 0)
                continue
            parts = info.split(b"\x00")
            raw_name = parts[0].decode('latin1', 'ignore')
            filename = "".join(c for c in os.path.basename(raw_name) if c.isalnum() or c in "._-") or "UPLOAD.BIN"
            try:
                file_size = int(parts[1].split()[0]) if len(parts) > 1 else 0
            except ValueError:
                file_size = 0
            print(f"[zmodem] receiving {filename} ({file_size} bytes)", flush=True)
            zio.send_hex_header(ZRPOS, pos)
        elif ftype == ZDATA:
            if flags != pos:
                zio.send_hex_header(ZRPOS, pos)
                continue
            while True:
                try:
                    data, delim = zio.recv_subpacket()
                except ProtocolError as e:
                    print(f"[zmodem] {e} at {pos}: ZRPOS", flush=True)
                    zio.send_hex_header(ZRPOS, pos)
                    break
                file_data.extend(data)
                pos += len(data)
                if delim in (ZCRCQ, ZCRCW):
                    zio.send_hex_header(ZACK, pos)
                if delim in (ZCRCE, ZCRCW):
                    break
        elif ftype == ZEOF:
            if flags != pos:
                zio.send_hex_header(ZRPOS, pos)
                continue
            zio.send_hex_header(ZRINIT, rinit)
        elif ftype == ZFIN:
            zio.send_hex_header(ZFIN, 0)
            zio.link.recv_byte(1.0)          # the client's "OO"
            zio.link.recv_byte(0.5)
            break
        else:
            zio.send_hex_header(ZNAK, 0)

    out_path = os.path.join(output_dir, filename)
    with open(out_path, "wb") as f:
        f.write(file_data)
    print(f"[zmodem] saved {out_path} ({len(file_data)} bytes)", flush=True)
    return filename, len(file_data)


def x_crc16(data):
    crc = 0
    for b in data:
        crc = z_crc16(crc, b)
    return crc


def xmodem_send_file(link, data, block=128):
    """Sends a file with XMODEM, as sx does: CRC when the client says 'C'."""
    print(f"[xmodem] sending {len(data)} bytes in {block}-byte blocks", flush=True)
    end = time.time() + 60
    use_crc = None
    while time.time() < end and use_crc is None:
        b = link.recv_byte(3.0)
        if b == ord('C'):
            use_crc = True
        elif b == NAK:
            use_crc = False
    if use_crc is None:
        raise ProtocolError("the client never asked for the file")
    corrupted = False
    blk = 1
    for off in range(0, len(data), block):
        chunk = data[off:off + block].ljust(block, b"\x1a")
        pkt = bytes([STX if block == 1024 else SOH, blk & 0xff, (~blk) & 0xff]) + chunk
        pkt += bytes([x_crc16(chunk) >> 8, x_crc16(chunk) & 0xff]) if use_crc else bytes([sum(chunk) & 0xff])
        for attempt in range(10):
            corrupt = OPTIONS["inject"] and not corrupted and blk == 3
            if corrupt:
                corrupted = True
                print("[xmodem] injected a bad block", flush=True)
                link.send(pkt[:10] + bytes([pkt[10] ^ 0x55]) + pkt[11:])
            else:
                link.send(pkt)
            resp = None
            end = time.time() + 10
            while time.time() < end:
                b = link.recv_byte(10.0)
                if b in (ACK, NAK, CAN):
                    resp = b
                    break
            if resp == ACK:
                break
            if resp == CAN:
                raise ProtocolError("cancelled by the client")
        else:
            raise ProtocolError(f"block {blk} never acknowledged")
        blk += 1
    for attempt in range(10):
        link.send(bytes([EOT]))
        b = link.recv_byte(3.0)
        while b is not None and b not in (ACK, NAK):
            b = link.recv_byte(3.0)
        if b == ACK:
            print("[xmodem] sent", flush=True)
            return
    raise ProtocolError("EOT never acknowledged")


def xmodem_recv_file(link, output_dir="test_uploads", name="XUPLOAD.BIN"):
    """Receives a file with XMODEM, as rx does. Returns (name, size)."""
    os.makedirs(output_dir, exist_ok=True)
    print("[xmodem] waiting for the client's first block", flush=True)
    data = bytearray()
    expected = 1
    eot_seen = False
    got = False
    link.drain(0.3)                 # the board's text, or the first block is missed
    for attempt in range(10):
        link.send(b"C")
        b = link.recv_byte(3.0)
        if b in (SOH, STX, EOT):
            got = True
            break
    if not got:
        raise ProtocolError("the client never started")
    errors = 0
    while True:
        if b is None:
            b = link.recv_byte(10.0)
            if b is None:
                errors += 1
                if errors > 10:
                    raise ProtocolError("timeout")
                link.send(bytes([NAK]))
                continue
        if b == EOT:
            if eot_seen:
                link.send(bytes([ACK]))
                break
            eot_seen = True
            link.send(bytes([NAK]))
            b = None
            continue
        if b == CAN:
            b = link.recv_byte(1.0)
            if b == CAN:
                raise ProtocolError("cancelled by the client")
            continue
        if b not in (SOH, STX):
            b = None
            continue
        eot_seen = False
        n = 1024 if b == STX else 128
        pkt = bytearray()
        while len(pkt) < n + 4:
            c = link.recv_byte(3.0)
            if c is None:
                break
            pkt.append(c)
        ok = len(pkt) == n + 4 and (pkt[0] + pkt[1]) & 0xff == 0xff and x_crc16(pkt[2:2 + n]) == (pkt[2 + n] << 8 | pkt[3 + n])
        if ok and pkt[0] == expected & 0xff:
            data.extend(pkt[2:2 + n])
            expected += 1
            link.send(bytes([ACK]))
        elif ok and pkt[0] == (expected - 1) & 0xff:
            link.send(bytes([ACK]))
        else:
            errors += 1
            if errors > 10:
                raise ProtocolError("too many errors")
            link.drain()
            link.send(bytes([NAK]))
        b = None
    out_path = os.path.join(output_dir, name)
    with open(out_path, "wb") as f:
        f.write(data)
    print(f"[xmodem] saved {out_path} ({len(data)} bytes)", flush=True)
    return name, len(data)


# ---- the board ----------------------------------------------------------

def to_petscii(data):
    if isinstance(data, str):
        data = data.encode('latin1')
    res = bytearray()
    for b in data:
        if 0x61 <= b <= 0x7a:
            res.append(b - 0x20)
        elif 0x41 <= b <= 0x5a:
            res.append(b + 0x80)
        else:
            res.append(b)
    return bytes(res)


def from_petscii(data):
    res = bytearray()
    for b in data:
        if 0x41 <= b <= 0x5a:
            res.append(b + 0x20)
        elif 0xc1 <= b <= 0xda:
            res.append(b - 0x80)
        else:
            res.append(b)
    return bytes(res)


MENU = (
    "  [1] Echo Test / Interactive Chat\r\n"
    "  [2] ZMODEM Download (auto-detected by the client)\r\n"
    "  [3] ZMODEM Upload (start it from the client's transfer menu)\r\n"
    "  [4] List Received Uploads\r\n"
    "  [5] XMODEM Download, 128-byte blocks\r\n"
    "  [6] XMODEM-1K Download\r\n"
    "  [7] XMODEM Upload\r\n"
    "  [Q] Disconnect\r\n\r\n"
)


def build_menu(mode, fname, size):
    head = "MEGA65 BBS CLIENT TEST SERVER"
    info = f"  Mode: {mode.upper()}   File: {fname} ({size} bytes)   Errors: {'injected' if OPTIONS['inject'] else 'none'}\r\n\r\n"
    if mode == "ansi":
        return (b"\x1b[2J\x1b[H\x1b[1;36m" + f"+{'-' * 76}+\r\n|{head:^76}|\r\n+{'-' * 76}+\r\n".encode('latin1')
                + b"\x1b[1;33m" + info.encode('latin1') + b"\x1b[1;32m" + MENU.encode('latin1')
                + b"\x1b[0;37mSelect option: ")
    return (b"\x93\x0e\x9f" + to_petscii(f"{'=' * 80}\r\n{head:^80}\r\n{'=' * 80}\r\n")
            + b"\x9e" + to_petscii(info) + b"\x1e" + to_petscii(MENU) + b"\x05" + to_petscii("Select option: "))


def make_test_file(path):
    """The deterministic 20 KB file the C tests use, with every byte that
    needs escaping somewhere in it."""
    x = 12345
    d = bytearray()
    for _ in range(20480):
        x = (x * 1103515245 + 12345) & 0xffffffff
        d.append((x >> 16) & 0xff)
    d[0] = ord('Z')
    d[100:112] = b"\xff" * 12
    d[200:206] = b"\x18" * 6
    d[300:304] = bytes([0x11, 0x13, 0x91, 0x93])
    d[400] = 0x7f
    d[500:504] = b"\x04" * 4
    d[600:608] = b"\x1a" * 8
    d[700:706] = b"**\x18B00"
    d[-3:] = b"\xff" * 3
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as f:
        f.write(d)


def handle_client(conn, addr, args):
    print(f"\n[server] connection from {addr}", flush=True)
    link = TelnetLink(conn, raw=args.raw)
    mode = args.mode
    with open(args.file, "rb") as f:
        file_bytes = f.read()
    fname = os.path.basename(args.file)

    def say(text):
        link.send(to_petscii(text) if mode == "petscii" else text.encode('latin1'))

    def wait_enter():
        link.drain(1.0)
        while True:
            b = link.recv_byte(600.0)
            if b is None or b in (13, 10):
                return

    def transfer(fn, *a, **kw):
        try:
            result = fn(link, *a, **kw)
            say(f"\r\nTransfer complete{': ' + str(result) if result else ''}.\r\nPress Return for the menu...")
        except (ProtocolError, TimeoutError, ConnectionError) as e:
            print(f"[server] transfer failed: {e}", flush=True)
            say(f"\r\nTransfer failed: {e}\r\nPress Return for the menu...")
        wait_enter()

    try:
        link.negotiate()
        time.sleep(0.3)
        link.recv_byte(0.3)             # let the options in
        if link.ttype:
            mode = "ansi" if "ansi" in link.ttype.lower() else "petscii"
        print(f"[server] emulation {mode} (terminal type {link.ttype!r})", flush=True)

        while True:
            link.send(build_menu(mode, fname, len(file_bytes)))
            choice = None
            while choice is None:
                b = link.recv_byte(600.0)
                if b is None:
                    return
                c = from_petscii(bytes([b]))[0] if mode == "petscii" else b
                if chr(c).upper() in "1234567Q":
                    choice = chr(c).upper()
            print(f"[server] option {choice}", flush=True)

            if choice == "Q":
                say("\r\nGoodbye.\r\n")
                return
            if choice == "1":
                say("\r\n--- Echo ---\r\nType lines; /download, /upload, /menu.\r\nEnter text: ")
                line = bytearray()
                while True:
                    b = link.recv_byte(600.0)
                    if b is None:
                        return
                    if b in (13, 10):
                        link.send(b"\r\n")
                        text = (from_petscii(bytes(line)) if mode == "petscii" else bytes(line)).decode('latin1', 'ignore').strip()
                        line.clear()
                        if text.lower() in ("/menu", "/quit", "quit", "exit"):
                            break
                        if text.lower() == "/download":
                            transfer(zmodem_send_file, fname, file_bytes)
                            break
                        if text.lower() == "/upload":
                            transfer(zmodem_recv_file)
                            break
                        say(f"Echo: {text}\r\nEnter text: ")
                    elif b in (8, 20, 127):
                        if line:
                            line.pop()
                            link.send(b"\x08 \x08")
                    elif 32 <= b <= 126 or b >= 160:
                        line.append(b)
                        link.send(bytes([b]))
            elif choice == "2":
                say(f"\r\nSending {fname} with ZMODEM; the client detects it.\r\n")
                time.sleep(0.5)
                transfer(zmodem_send_file, fname, file_bytes)
            elif choice == "3":
                say("\r\nReady for a ZMODEM upload: choose ZMODEM Upload in the client's transfer menu.\r\n")
                transfer(zmodem_recv_file)
            elif choice == "4":
                files = sorted(os.listdir("test_uploads")) if os.path.isdir("test_uploads") else []
                say("\r\n--- Uploads ---\r\n" + ("".join(f"  {fn:<20} {os.path.getsize(os.path.join('test_uploads', fn)):>7} bytes\r\n" for fn in files) or "  (none)\r\n")
                    + "\r\nPress Return for the menu...")
                wait_enter()
            elif choice in ("5", "6"):
                say(f"\r\nSending {fname} with XMODEM{'-1K' if choice == '6' else ''}: choose XMODEM Download in the client's transfer menu.\r\n")
                transfer(xmodem_send_file, file_bytes, 1024 if choice == "6" else 128)
            elif choice == "7":
                say("\r\nReady for an XMODEM upload: choose XMODEM Upload in the client's transfer menu.\r\n")
                transfer(xmodem_recv_file)
    except (ConnectionError, OSError) as e:
        print(f"[server] {e}", flush=True)
    finally:
        conn.close()
        print(f"[server] closed {addr}", flush=True)


def get_local_ips():
    ips = []
    try:
        import subprocess
        out = subprocess.check_output(["ifconfig"]).decode("latin1")
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("inet ") and not line.startswith("inet 127."):
                ips.append(line.split()[1])
    except Exception:
        pass
    return ips or ["127.0.0.1"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=6400)
    ap.add_argument("--mode", choices=["petscii", "ansi"], default="petscii")
    ap.add_argument("--file", default="test_files/TEST20K.BIN")
    ap.add_argument("--inject-errors", action="store_true", help="corrupt one subpacket or block per transfer")
    ap.add_argument("--hex-headers", action="store_true", help="ZFILE and ZDATA as hex headers")
    ap.add_argument("--raw", action="store_true", help="no telnet framing")
    args = ap.parse_args()
    OPTIONS["inject"] = args.inject_errors
    OPTIONS["hex_headers"] = args.hex_headers
    if not os.path.exists(args.file):
        make_test_file(args.file)
        print(f"created {args.file}")

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", args.port))
    s.listen(5)
    print("MEGA65 BBS client test server")
    print(f"port {args.port}, {args.mode}, file {args.file}, errors {'injected' if args.inject_errors else 'none'}")
    for ip in get_local_ips():
        print(f"  -> {ip}:{args.port}")
    print("Ctrl+C to stop.", flush=True)
    try:
        while True:
            conn, addr = s.accept()
            handle_client(conn, addr, args)
    except KeyboardInterrupt:
        print("\n[server] stopped")
    finally:
        s.close()


if __name__ == "__main__":
    main()
