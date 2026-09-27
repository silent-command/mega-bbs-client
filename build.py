#!/usr/bin/env python3
"""Build driver for mega-bbs-client: Python driver for macOS, Linux, and Windows.

    python3 build.py            build the client and bin/BBS.D81
    python3 build.py test       build and run the host test suite
    python3 build.py clean      clean build and bin directories

Requires: llvm-mos (mos-mega65-clang), c1541 from VICE, and mega-net.
"""
import os, platform, re, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / "build"
BIN = ROOT / "bin"
IS_WINDOWS = platform.system() == "Windows"
MEGANET = ROOT / "mega-net"
LIBC_A = BUILD / "libc" / "libmega65libc.a"


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def run(cmd, **kw):
    print("  " + " ".join(Path(c).name if os.sep in str(c) else str(c) for c in cmd))
    if subprocess.run([str(c) for c in cmd], **kw).returncode != 0:
        die("command failed")


def find_tool(exe, roots=(), env=None):
    if env and os.environ.get(env):
        return os.environ[env]
    name = exe + (".exe" if IS_WINDOWS else "")
    for r in roots:
        cand = Path(r) / "bin" / name
        if cand.is_file():
            return str(cand)
    found = shutil.which(name)
    if found:
        return found
    for cand in ("/opt/homebrew/bin/" + name, "/usr/local/bin/" + name):
        if Path(cand).is_file():
            return cand
    die(f"{exe} not found")


def mos_clang():
    roots = [os.environ["LLVM_MOS_DIR"]] if os.environ.get("LLVM_MOS_DIR") else []
    roots += [ROOT / "llvm-mos", Path.home() / "llvm-mos", "/opt/llvm-mos", "/usr/local/llvm-mos"]
    return find_tool("mos-mega65-clang", roots)


def check_rmw(elf):
    """Scans linked ELF for the llvm-mos loop miscompile pattern."""
    if not elf.exists():
        return
    objdump = find_tool("llvm-objdump", [Path(mos_clang()).parent.parent])
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "orphan_rmw.py"), str(elf), objdump])
    if r.returncode:
        die(f"{elf.name}: orphan RMW miscompile detected; rewrite loop")


def check_headroom(mapfile):
    """The soft stack grows down from $D000 into whatever the program leaves
    above its data, and the linker never reserves any. This client once
    ended its data at $CF52: 174 bytes, and main's frame reached back into
    .bss, so the boot drive number was overwritten by the config text and
    every save went to a drive that was not there. At least 1 KB is
    required here."""
    end = 0
    for line in mapfile.read_text().splitlines():
        m = re.match(r"^\s*([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+\d+\s+\.(bss|noinit|data)$", line)
        if m and int(m.group(1), 16) < 0xD000:
            end = max(end, int(m.group(1), 16) + int(m.group(2), 16))
    room = 0xD000 - end
    print(f"  data ends at ${end:04x}: {room} bytes for the soft stack")
    if room < 1024:
        die("less than 1 KB between the program's data and $D000: the stack will overwrite the data; find the bytes")


def ensure_meganet():
    image = MEGANET / "build" / "m65" / "meganet.bin"
    tramp = MEGANET / "build" / "gen" / "meganet_tramp.c"
    if not (image.is_file() and tramp.is_file()):
        print("building mega-net ABI:")
        run([sys.executable, "build.py", "abi"], cwd=MEGANET)
    return image, tramp


def ensure_libc():
    if LIBC_A.is_file():
        return LIBC_A
    alt_libc = ROOT.parent / "mega-irc" / "build" / "libc" / "src" / "libmega65libc.a"
    if alt_libc.is_file():
        BUILD.mkdir(parents=True, exist_ok=True)
        (BUILD / "libc").mkdir(parents=True, exist_ok=True)
        shutil.copy(alt_libc, LIBC_A)
        return LIBC_A
    die("libmega65libc.a not found")


def build_client():
    clang = mos_clang()
    libc = ensure_libc()
    meganet_bin, tramp = ensure_meganet()

    BIN.mkdir(exist_ok=True)
    srcs = sorted(str(p) for p in (ROOT / "src").glob("*.c"))
    platform_srcs = sorted(str(p) for p in (ROOT / "src" / "platform").glob("*.c"))
    vectors_c = str(MEGANET / "src" / "abi" / "meganet_vectors.c")

    cflags = [
        "-std=c99", "-Oz",
        "-I", str(ROOT / "include"),
        "-I", str(ROOT / "src"),
        "-I", str(ROOT / "src" / "platform"),
        "-I", str(MEGANET / "src" / "abi"),
        "-I", str(MEGANET / "build" / "gen"),
        "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"
    ]

    prg = BIN / "bbs.prg"
    map_file = BIN / "bbs.map"
    print("building MEGA65 BBS client (mos-mega65-clang):")
    run([clang] + cflags + srcs + platform_srcs + [str(tramp), vectors_c, str(libc),
         f"-Wl,-Map={map_file}", "-o", str(prg)])
    print(f"  {prg.name}: {prg.stat().st_size} bytes")

    check_rmw(prg.with_suffix(".prg.elf"))
    check_headroom(map_file)

    # Disk creation
    c1541 = find_tool("c1541")
    d81 = BIN / "BBS.D81"
    if d81.exists():
        d81.unlink()

    cp437_bin = ROOT / "assets" / "cp437.bin"
    bbscfg_txt = ROOT / "assets" / "bbscfg.txt"

    print("packaging BBS.D81:")
    cmd = [
        c1541,
        "-format", "bbs,bb", "d81", str(d81),
        "-write", str(prg), "bbs",
        "-write", str(meganet_bin), "meganet",
        "-write", str(cp437_bin), "cp437",
        "-write", str(bbscfg_txt), "bbscfg,s"
    ]
    run(cmd, stdout=subprocess.DEVNULL)
    run([c1541, "-attach", str(d81), "-dir"])
    print(f"\nBuild complete: {d81}")
    return 0


def build_test():
    BUILD.mkdir(exist_ok=True)
    cc = os.environ.get("CC", shutil.which("clang") or shutil.which("gcc") or shutil.which("cc"))
    if not cc:
        die("host C compiler (clang, gcc, or cc) not found")

    print("running host test suite:")

    tests = [
        ("test_screen", [
            str(ROOT / "src" / "platform" / "m65_screen.c"),
            str(ROOT / "src" / "platform" / "m65_font.c"),
            str(ROOT / "tests" / "test_screen.c")
        ], ["-I", str(ROOT / "src"), "-I", str(ROOT / "src" / "platform")]),

        ("test_ansi", [
            str(ROOT / "src" / "ansi.c"),
            str(ROOT / "src" / "platform" / "m65_screen.c"),
            str(ROOT / "src" / "platform" / "m65_font.c"),
            str(ROOT / "tests" / "test_ansi.c")
        ], ["-I", str(ROOT / "src"), "-I", str(ROOT / "src" / "platform")]),

        ("test_petscii", [
            str(ROOT / "src" / "petscii.c"),
            str(ROOT / "src" / "platform" / "m65_screen.c"),
            str(ROOT / "src" / "platform" / "m65_font.c"),
            str(ROOT / "tests" / "test_petscii.c")
        ], ["-I", str(ROOT / "src"), "-I", str(ROOT / "src" / "platform")]),

        ("test_telnet", [
            str(ROOT / "src" / "telnet.c"),
            str(ROOT / "tests" / "test_telnet.c")
        ], ["-I", str(ROOT / "src"), "-I", str(MEGANET / "src" / "abi")]),

        ("test_bookmarks", [
            str(ROOT / "src" / "bookmarks.c"),
            str(ROOT / "tests" / "test_bookmarks.c")
        ], ["-I", str(ROOT / "src")]),

        ("test_zmodem", [
            str(ROOT / "src" / "zmodem.c"),
            str(ROOT / "src" / "telnet.c"),
            str(ROOT / "src" / "netsend.c"),
            str(ROOT / "tests" / "test_zmodem.c")
        ], ["-I", str(ROOT / "src"), "-I", str(MEGANET / "src" / "abi")]),

        ("test_xmodem", [
            str(ROOT / "src" / "xmodem.c"),
            str(ROOT / "src" / "telnet.c"),
            str(ROOT / "src" / "netsend.c"),
            str(ROOT / "tests" / "test_xmodem.c")
        ], ["-I", str(ROOT / "src"), "-I", str(MEGANET / "src" / "abi")])
    ]

    for name, files, incs in tests:
        out = BUILD / name
        run([cc, "-Wall", "-Wextra", "-Werror"] + incs + files + ["-o", str(out)])
        run([str(out)])

    print("\nAll host tests passed (7/7)!")
    return 0


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "client"
    if target == "client":
        return build_client()
    if target == "test":
        return build_test()
    if target == "clean":
        shutil.rmtree(BUILD, ignore_errors=True)
        shutil.rmtree(BIN, ignore_errors=True)
        print("Cleaned.")
        return 0
    print(__doc__)
    die(f"unknown target '{target}'")


if __name__ == "__main__":
    sys.exit(main())
