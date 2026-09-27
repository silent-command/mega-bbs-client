#!/usr/bin/env python3
import sys
import subprocess

def main():
    if len(sys.argv) < 3:
        print("Usage: run_timeout.py <timeout_seconds> <command...>")
        sys.exit(1)

    timeout = float(sys.argv[1])
    cmd = sys.argv[2:]

    try:
        res = subprocess.run(cmd, timeout=timeout)
        sys.exit(res.returncode)
    except subprocess.TimeoutExpired:
        print(f"\n[ERROR] Command timed out after {timeout} seconds: {' '.join(cmd)}", file=sys.stderr)
        sys.exit(124)

if __name__ == "__main__":
    main()
