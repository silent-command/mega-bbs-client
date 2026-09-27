#!/usr/bin/env python3
"""Drives the test server from a second Python process, so the server's
own ZMODEM and XMODEM can be checked without a client.

    python3 tools/bbs_test_server.py --port 6401 --raw --mode ansi &
    python3 tools/test_zmodem_loopback.py 6401

With --raw the server sends no telnet framing and this script speaks
only the transfer protocols; --mode ansi keeps the menu text in ASCII.
"""
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bbs_test_server import (TelnetLink, ZModemIO, ZRQINIT, ZRINIT, ZFILE, ZDATA, ZEOF, ZFIN, ZACK, ZRPOS,
                             ZCRCW, ZCRCG, ZCRCE, xmodem_recv_file, xmodem_send_file, CANFDX, CANOVIO)


def connect(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(("127.0.0.1", port))
    link = TelnetLink(s, raw=True)
    seen = b""
    while b"Select option: " not in seen:
        b = link.recv_byte(5.0)
        assert b is not None, "no menu"
        seen += bytes([b])
    return s, link


def wait_text(link, text):
    seen = b""
    while text.encode('latin1') not in seen:
        b = link.recv_byte(10.0)
        assert b is not None, f"never saw {text!r}"
        seen += bytes([b])


def test_download(port, expected):
    s, link = connect(port)
    link.send(b"2")
    zio = ZModemIO(link)
    ftype, _ = zio.recv_header(10.0)
    assert ftype == ZRQINIT, ftype
    zio.send_hex_header(ZRINIT, (CANFDX | CANOVIO) << 24 | 1024)
    ftype, _ = zio.recv_header(5.0)
    assert ftype == ZFILE, ftype
    info, _ = zio.recv_subpacket()
    name = info.split(b"\x00")[0].decode('latin1')
    zio.send_hex_header(ZRPOS, 0)
    data = bytearray()
    while True:
        ftype, pos = zio.recv_header(5.0)
        if ftype == ZEOF:
            assert pos == len(data)
            break
        assert ftype == ZDATA and pos == len(data), (ftype, pos, len(data))
        while True:
            chunk, delim = zio.recv_subpacket()
            data.extend(chunk)
            if delim == ZCRCW:
                zio.send_hex_header(ZACK, len(data))
            if delim in (ZCRCE, ZCRCW):
                break
    zio.send_hex_header(ZRINIT, (CANFDX | CANOVIO) << 24 | 1024)
    ftype, _ = zio.recv_header(5.0)
    assert ftype == ZFIN
    zio.send_hex_header(ZFIN, 0)
    link.send(b"OO")
    assert bytes(data) == expected, f"download differs: {len(data)} bytes"
    print(f"ZMODEM download of {name}: {len(data)} bytes, identical")
    link.send(b"\r")
    s.close()


def test_upload(port, payload):
    s, link = connect(port)
    link.send(b"3")
    wait_text(link, "Ready for a ZMODEM upload")
    zio = ZModemIO(link)
    zio.send_hex_header(ZRQINIT, 0)
    ftype, flags = zio.recv_header(5.0)
    assert ftype == ZRINIT and (flags >> 24) & CANOVIO, (ftype, flags)
    zio.send_hex_header(ZFILE, 0)
    zio.send_subpacket(b"LOOPBACK.BIN\x00" + f"{len(payload)} 0".encode('latin1'), ZCRCW)
    ftype, _ = zio.recv_header(5.0)
    assert ftype == ZRPOS
    zio.send_hex_header(ZDATA, 0)
    for off in range(0, len(payload), 1024):
        chunk = payload[off:off + 1024]
        zio.send_subpacket(chunk, ZCRCW if off + 1024 >= len(payload) else ZCRCG)
    ftype, pos = zio.recv_header(5.0)
    assert ftype == ZACK and pos == len(payload), (ftype, pos)
    zio.send_hex_header(ZEOF, len(payload))
    ftype, _ = zio.recv_header(5.0)
    assert ftype == ZRINIT
    zio.send_hex_header(ZFIN, 0)
    ftype, _ = zio.recv_header(5.0)
    assert ftype == ZFIN
    link.send(b"OO")
    wait_text(link, "Transfer complete")
    with open("test_uploads/LOOPBACK.BIN", "rb") as f:
        assert f.read() == payload, "upload differs"
    print(f"ZMODEM upload: {len(payload)} bytes, identical")
    link.send(b"\r")
    s.close()


def test_xmodem_download(port, expected, option):
    s, link = connect(port)
    link.send(option)
    wait_text(link, "choose XMODEM Download")
    name, size = xmodem_recv_file(link, output_dir="test_uploads", name="XLOOP.BIN")
    with open("test_uploads/XLOOP.BIN", "rb") as f:
        got = f.read()
    assert got[:len(expected)] == expected and set(got[len(expected):]) <= {0x1a}, "xmodem download differs"
    print(f"XMODEM download (option {option.decode()}): {size} bytes, identical")
    link.send(b"\r")
    s.close()


def test_xmodem_upload(port, payload):
    s, link = connect(port)
    link.send(b"7")
    wait_text(link, "choose XMODEM Upload")
    xmodem_send_file(link, payload, 128)
    wait_text(link, "Transfer complete")
    with open("test_uploads/XUPLOAD.BIN", "rb") as f:
        got = f.read()
    assert got[:len(payload)] == payload, "xmodem upload differs"
    print(f"XMODEM upload: {len(payload)} bytes, identical")
    link.send(b"\r")
    s.close()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 6401
    with open("test_files/TEST20K.BIN", "rb") as f:
        expected = f.read()
    test_download(port, expected)
    test_upload(port, expected)
    test_xmodem_download(port, expected, b"5")
    test_xmodem_download(port, expected, b"6")
    test_xmodem_upload(port, expected)
    print("Loopback tests passed.")
