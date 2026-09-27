#!/usr/bin/env python3
"""A telnet front for lrzsz, so the client can be tested against the same
sz and rz that real boards use.

    python3 tools/lrzsz_bridge.py [--port 6500] [--file test_files/TEST20K.BIN]
                                  [--upload-dir test_uploads]

The client dials the port and gets a one-line menu. A key runs the
program on a pseudo-terminal and relays bytes between it and the
connection with telnet framing (IAC doubled on the way out, collapsed and
answered on the way in), as a board's telnet server does:

    D   sz --binary FILE        ZMODEM download to the client
    U   rz --binary             ZMODEM upload from the client
    X   lsz --xmodem FILE       XMODEM download (128-byte, CRC)
    K   lsz --xmodem -k FILE    XMODEM-1K download
    R   lrz --xmodem NAME       XMODEM upload from the client
    Q   hang up

Requires lrzsz (brew install lrzsz).
"""
import argparse
import os
import pty
import select
import socket
import sys
import termios
import tty

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
OPT_BINARY, OPT_SGA = 0, 3


class TelnetLink:
    """The connection with telnet framing handled."""

    def __init__(self, sock):
        self.sock = sock
        self.state = 0            # 0 data, 1 after IAC, 2 option byte follows, 3 in SB
        self.cmd = 0
        self.log = []

    def send(self, data):
        self.sock.sendall(bytes(data).replace(b"\xff", b"\xff\xff"))

    def send_text(self, text):
        self.send(text.encode("latin1"))

    def negotiate(self):
        self.sock.sendall(bytes([IAC, WILL, OPT_SGA, IAC, DO, OPT_SGA, IAC, WILL, OPT_BINARY, IAC, DO, OPT_BINARY]))

    def filter(self, raw):
        """Raw bytes from the socket to data bytes, answering options."""
        out = bytearray()
        for b in raw:
            if self.state == 0:
                if b == IAC:
                    self.state = 1
                else:
                    out.append(b)
            elif self.state == 1:
                if b == IAC:
                    out.append(0xff)
                    self.state = 0
                elif b in (DO, DONT, WILL, WONT):
                    self.cmd = b
                    self.state = 2
                elif b == SB:
                    self.state = 3
                else:
                    self.state = 0
            elif self.state == 2:
                self.log.append((self.cmd, b))
                if self.cmd == DO:
                    self.sock.sendall(bytes([IAC, WILL if b in (OPT_BINARY, OPT_SGA) else WONT, b]))
                elif self.cmd == WILL:
                    self.sock.sendall(bytes([IAC, DO if b in (OPT_BINARY, OPT_SGA) else DONT, b]))
                self.state = 0
            elif self.state == 3:
                if b == IAC:
                    self.state = 4
            elif self.state == 4:
                self.state = 0 if b == SE else 3
        return bytes(out)


def run_on_pty(link, argv, cwd):
    """Runs argv on a pty and relays until it exits. Returns the exit status."""
    pid, fd = pty.fork()
    if pid == 0:
        try:
            tty.setraw(0)
            os.chdir(cwd)
            null = os.open(os.devnull, os.O_WRONLY)
            os.dup2(null, 2)
            os.execvp(argv[0], argv)
        finally:
            os._exit(127)
    sock = link.sock
    sock.setblocking(False)
    try:
        while True:
            r, _, _ = select.select([sock, fd], [], [], 30.0)
            if not r:
                os.kill(pid, 15)
                break
            if fd in r:
                try:
                    data = os.read(fd, 4096)
                except OSError:
                    break
                if not data:
                    break
                link.send(data)
            if sock in r:
                try:
                    raw = sock.recv(4096)
                except BlockingIOError:
                    raw = b"\x00\x00"[:0]
                if not raw:
                    os.kill(pid, 15)
                    break
                data = link.filter(raw)
                if data:
                    os.write(fd, data)
    finally:
        sock.setblocking(True)
        os.close(fd)
    _, status = os.waitpid(pid, 0)
    return os.WEXITSTATUS(status) if os.WIFEXITED(status) else -1


def serve(conn, args):
    link = TelnetLink(conn)
    link.negotiate()
    link.send_text("\r\nlrzsz bridge. D download  U upload  X xmodem  K xmodem-1k  R xmodem upload  Q quit\r\n")
    fname = os.path.abspath(args.file)
    updir = os.path.abspath(args.upload_dir)
    os.makedirs(updir, exist_ok=True)
    while True:
        link.send_text("> ")
        key = b""
        while not key:
            raw = conn.recv(256)
            if not raw:
                return
            data = link.filter(raw)
            key = bytes(c for c in data if c in b"DUXKRQduxkrq")[:1]
        key = key.upper()
        if key == b"Q":
            link.send_text("bye\r\n")
            return
        if key == b"D":
            link.send_text("sz starts now\r\n")
            argv = ["sz", "--binary", "-q", fname]
        elif key == b"U":
            link.send_text("rz starts now\r\n")
            argv = ["rz", "--binary", "-q", "-y"]
        elif key == b"X":
            link.send_text("lsz starts now: start your XMODEM download\r\n")
            argv = ["lsz", "--xmodem", "--binary", "-q", fname]
        elif key == b"K":
            link.send_text("lsz -k starts now: start your XMODEM download\r\n")
            argv = ["lsz", "--xmodem", "--binary", "-q", "-k", fname]
        else:
            link.send_text("lrz starts now: start your XMODEM upload\r\n")
            argv = ["lrz", "--xmodem", "--binary", "-q", "-y", "XUPLOAD.BIN"]
        status = run_on_pty(link, argv, updir if key in (b"U", b"R") else os.path.dirname(fname))
        print(f"[bridge] {' '.join(argv)} exited {status}", flush=True)
        link.send_text(f"\r\n{argv[0]} finished with status {status}\r\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=6500)
    ap.add_argument("--file", default="test_files/TEST20K.BIN")
    ap.add_argument("--upload-dir", default="test_uploads")
    ap.add_argument("--once", action="store_true", help="serve one connection and exit")
    args = ap.parse_args()
    if not os.path.exists(args.file):
        sys.exit(f"{args.file} not found: run the test server once to create it")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", args.port))
    s.listen(2)
    print(f"[bridge] listening on {args.port}, file {args.file}", flush=True)
    try:
        while True:
            conn, addr = s.accept()
            print(f"[bridge] connection from {addr}", flush=True)
            try:
                serve(conn, args)
            except (ConnectionError, OSError) as e:
                print(f"[bridge] {e}", flush=True)
            finally:
                conn.close()
            if args.once:
                break
    except KeyboardInterrupt:
        pass
    finally:
        s.close()


if __name__ == "__main__":
    main()
