"""A peer that holds ProtoBase's owner lock, for protoIO's lock interop test.

Usage: ownerlock_peer.py <lockpath> <readyfile> <stopfile>

Takes the exclusive lock on <lockpath> exactly as ProtoBase's
proto_db/storage/ownerlock.py does (fcntl.flock on POSIX, msvcrt.locking on
byte 0 on Windows), creates <readyfile>, then holds the lock until <stopfile>
appears (polled every 50 ms, at most 30 s) and exits 0. Exits 2 when the lock
is already held.
"""
import os
import sys
import time

try:
    import fcntl
except ImportError:  # Windows
    fcntl = None
try:
    import msvcrt
except ImportError:  # POSIX
    msvcrt = None


def try_os_lock(fd):
    if fcntl is not None:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return False
        return True
    os.lseek(fd, 0, os.SEEK_SET)
    try:
        msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
    except PermissionError:
        return False
    return True


def main():
    lock_path, ready_path, stop_path = sys.argv[1:4]
    fd = os.open(lock_path, os.O_RDWR | os.O_CREAT, 0o644)
    if not try_os_lock(fd):
        return 2
    with open(ready_path, "w"):
        pass
    deadline = time.monotonic() + 30.0
    while not os.path.exists(stop_path) and time.monotonic() < deadline:
        time.sleep(0.05)
    os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
