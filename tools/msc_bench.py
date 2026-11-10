#!/usr/bin/env python3
"""
Sequential write/read throughput of a USB Mass Storage drive on Windows.

The test file is written and read with FILE_FLAG_NO_BUFFERING (and WRITE_THROUGH for the
write), so the Windows file cache does not hide the device speed. Only the Python standard
library is used.

    python msc_bench.py E:                      # 1 GiB file, 1 MiB per I/O, 3 runs
    python msc_bench.py E: --size 4096 --block 4096 --runs 1
    python msc_bench.py E: --no-verify --keep

Rates are MB/s with 1 MB = 10^6 bytes (CrystalDiskMark uses the same unit).
"""
import argparse
import ctypes
import os
import struct
import sys
import time
from ctypes import wintypes

if sys.platform != "win32":
    sys.exit("This script uses the Win32 API and runs on Windows only.")

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x00000001
CREATE_ALWAYS = 2
OPEN_EXISTING = 3
FILE_ATTRIBUTE_NORMAL = 0x00000080
FILE_FLAG_NO_BUFFERING = 0x20000000
FILE_FLAG_WRITE_THROUGH = 0x80000000
FILE_FLAG_SEQUENTIAL_SCAN = 0x08000000
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000
PAGE_READWRITE = 0x04
DRIVE_REMOVABLE = 2
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                 wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
kernel32.CreateFileW.restype = wintypes.HANDLE
kernel32.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                               ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
kernel32.WriteFile.restype = wintypes.BOOL
kernel32.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                              ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
kernel32.ReadFile.restype = wintypes.BOOL
kernel32.FlushFileBuffers.argtypes = [wintypes.HANDLE]
kernel32.FlushFileBuffers.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL
kernel32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD]
kernel32.VirtualAlloc.restype = ctypes.c_void_p
kernel32.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD]
kernel32.VirtualFree.restype = wintypes.BOOL
kernel32.GetDriveTypeW.argtypes = [wintypes.LPCWSTR]
kernel32.GetDriveTypeW.restype = wintypes.UINT
kernel32.GetDiskFreeSpaceExW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_ulonglong),
                                         ctypes.POINTER(ctypes.c_ulonglong), ctypes.POINTER(ctypes.c_ulonglong)]
kernel32.GetDiskFreeSpaceExW.restype = wintypes.BOOL
kernel32.GetVolumeInformationW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD,
                                           ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD),
                                           ctypes.POINTER(wintypes.DWORD), wintypes.LPWSTR, wintypes.DWORD]
kernel32.GetVolumeInformationW.restype = wintypes.BOOL

MIB = 1024 * 1024


def win_error(what):
    err = ctypes.get_last_error()
    return OSError(err, f"{what} failed: {ctypes.FormatError(err).strip()} (error {err})")


def open_file(path, write):
    if write:
        access, share, disp = GENERIC_WRITE, 0, CREATE_ALWAYS
        flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH
    else:
        access, share, disp = GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING
        flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN
    h = kernel32.CreateFileW(path, access, share, None, disp, flags, None)
    if h is None or h == INVALID_HANDLE_VALUE:
        raise win_error(f"CreateFile({path})")
    return h


def stamp(view, block, base_offset):
    """Write the absolute file offset into the first 8 bytes of every 512-byte sector."""
    for o in range(0, block, 512):
        struct.pack_into("<Q", view, o, base_offset + o)


def expected_block(pattern, block, base_offset):
    out = bytearray(pattern[:block])
    for o in range(0, block, 512):
        struct.pack_into("<Q", out, o, base_offset + o)
    return bytes(out)


def run_once(path, size, block, buf, pattern, verify):
    n_blocks = size // block
    written = wintypes.DWORD(0)

    # ---- write: WriteFile calls + FlushFileBuffers ----
    ctypes.memmove(buf, pattern, block)
    view = (ctypes.c_char * block).from_address(buf)
    h = open_file(path, write=True)
    io = 0.0
    try:
        for i in range(n_blocks):
            if verify:
                stamp(view, block, i * block)
            t0 = time.perf_counter()
            ok = kernel32.WriteFile(h, buf, block, ctypes.byref(written), None)
            io += time.perf_counter() - t0
            if not ok or written.value != block:
                raise win_error(f"WriteFile at {i * block} B")
        t0 = time.perf_counter()
        if not kernel32.FlushFileBuffers(h):
            raise win_error("FlushFileBuffers")
        io += time.perf_counter() - t0
    finally:
        kernel32.CloseHandle(h)
    write_mbps = size / io / 1e6

    # ---- read: ReadFile calls only, then compare ----
    h = open_file(path, write=False)
    io = 0.0
    bad = 0
    first_bad = None
    try:
        for i in range(n_blocks):
            t0 = time.perf_counter()
            ok = kernel32.ReadFile(h, buf, block, ctypes.byref(written), None)
            io += time.perf_counter() - t0
            if not ok or written.value != block:
                raise win_error(f"ReadFile at {i * block} B")
            if verify:
                got = ctypes.string_at(buf, block)
                if got != expected_block(pattern, block, i * block):
                    bad += 1
                    if first_bad is None:
                        first_bad = i * block
    finally:
        kernel32.CloseHandle(h)
    read_mbps = size / io / 1e6
    return write_mbps, read_mbps, bad, first_bad


def drive_info(root):
    fs_name = ctypes.create_unicode_buffer(32)
    label = ctypes.create_unicode_buffer(64)
    serial, maxlen, flags = wintypes.DWORD(), wintypes.DWORD(), wintypes.DWORD()
    if not kernel32.GetVolumeInformationW(root, label, 64, ctypes.byref(serial), ctypes.byref(maxlen),
                                          ctypes.byref(flags), fs_name, 32):
        raise win_error(f"GetVolumeInformation({root})")
    free = ctypes.c_ulonglong()
    total = ctypes.c_ulonglong()
    if not kernel32.GetDiskFreeSpaceExW(root, ctypes.byref(free), ctypes.byref(total), None):
        raise win_error(f"GetDiskFreeSpaceEx({root})")
    return fs_name.value, label.value, total.value, free.value


def main():
    ap = argparse.ArgumentParser(description="USB MSC sequential throughput test (Windows, unbuffered I/O)")
    ap.add_argument("drive", help="drive letter of the MSC device, e.g. E:")
    ap.add_argument("--size", type=int, default=1024, help="test file size in MiB (default 1024)")
    ap.add_argument("--block", type=int, default=1024, help="bytes per WriteFile/ReadFile in KiB (default 1024)")
    ap.add_argument("--runs", type=int, default=3, help="write+read runs (default 3)")
    ap.add_argument("--no-verify", action="store_true", help="do not check the data read back")
    ap.add_argument("--keep", action="store_true", help="keep the test file")
    ap.add_argument("--force", action="store_true", help="allow a drive that is not reported as removable")
    args = ap.parse_args()

    letter = args.drive.rstrip(":\\/")
    if len(letter) != 1 or not letter.isalpha():
        ap.error("drive must be a letter such as E:")
    root = f"{letter.upper()}:\\"
    path = os.path.join(root, "msc_bench.tmp")
    block = args.block * 1024
    size = (args.size * MIB // block) * block
    if block < 4096 or block % 4096 or size <= 0 or args.runs < 1:
        ap.error("block must be a multiple of 4 KiB, size >= block, runs >= 1")

    if kernel32.GetDriveTypeW(root) != DRIVE_REMOVABLE and not args.force:
        sys.exit(f"{root} is not a removable drive. Check the drive letter (use --force to override).")

    fs_name, label, total, free = drive_info(root)
    print(f"Drive {root}  {fs_name}  label '{label}'  size {total / 1e9:.2f} GB  free {free / 1e9:.2f} GB")
    if fs_name.upper() == "FAT32" and size >= 4 * 1024 * MIB:
        sys.exit("FAT32 limits a file to 4 GiB - 1 B. Use a smaller --size or exFAT.")
    if free < size + 16 * MIB:
        sys.exit(f"Not enough free space for a {size / MIB:.0f} MiB test file.")

    print(f"Test file {path}: {size / MIB:.0f} MiB, {block // 1024} KiB per I/O, {args.runs} run(s), "
          f"verify {'off' if args.no_verify else 'on'}")
    print("Rates in MB/s (1 MB = 10^6 B). Do not use the drive during the test.\n")

    buf = kernel32.VirtualAlloc(None, block, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
    if not buf:
        raise win_error("VirtualAlloc")
    pattern = os.urandom(block)
    results = []
    try:
        for run in range(1, args.runs + 1):
            w, r, bad, first_bad = run_once(path, size, block, buf, pattern, not args.no_verify)
            results.append((w, r))
            status = "" if args.no_verify else ("  verify OK" if bad == 0 else
                                               f"  verify FAILED: {bad} blocks, first at {first_bad} B")
            print(f"run {run}: write {w:8.2f} MB/s   read {r:8.2f} MB/s{status}")
            if bad:
                break
    finally:
        kernel32.VirtualFree(buf, 0, MEM_RELEASE)
        if not args.keep and os.path.exists(path):
            os.remove(path)

    if len(results) > 1:
        ws = [w for w, _ in results]
        rs = [r for _, r in results]
        print(f"\naverage: write {sum(ws) / len(ws):8.2f} MB/s   read {sum(rs) / len(rs):8.2f} MB/s")
        print(f"best   : write {max(ws):8.2f} MB/s   read {max(rs):8.2f} MB/s")


if __name__ == "__main__":
    main()
