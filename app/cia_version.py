"""Keep the ticket and TMD release version consistent in our homebrew CIA.

Some makerom 0.19 builds omit the minor version even when given -ver.
"""
from pathlib import Path
import struct
import sys


def align64(size):
    return (size + 63) & ~63


def title_version(release):
    major, minor, micro = map(int, release.strip().split("."))
    # Pre-release installations used title versions through 1.2.13. Reserve
    # the first two major values so official releases update the same title ID.
    if not (0 <= major < 62 and 0 <= minor < 64 and 0 <= micro < 16):
        raise ValueError("release version exceeds the remaining CIA version range")
    return ((major + 2) << 10) | (minor << 4) | micro


def main():
    action, version_file = sys.argv[1:]
    version = title_version(Path(version_file).read_text(encoding="ascii"))
    if action == "--print":
        print(version)
        return
    cia_path = Path(action)
    cia = bytearray(cia_path.read_bytes())
    header, _, _, certs, ticket, tmd = struct.unpack_from("<IHHIII", cia)
    ticket_at = align64(header) + align64(certs)
    tmd_at = ticket_at + align64(ticket)
    if header != 0x2020 or tmd_at + tmd > len(cia):
        raise ValueError("invalid CIA section sizes")

    changed = False
    # Both sections produced by makerom use an RSA-2048/SHA-256 header.
    for start, size, version_offset in ((ticket_at, ticket, 0xA6), (tmd_at, tmd, 0x9C)):
        if struct.unpack_from(">I", cia, start)[0] != 0x10004 or size < 0x140 + version_offset + 2:
            raise ValueError("unexpected ticket or TMD format")
        at = start + 0x140 + version_offset
        if struct.unpack_from(">H", cia, at)[0] != version:
            struct.pack_into(">H", cia, at, version)
            changed = True
    if changed:
        cia_path.write_bytes(cia)


if __name__ == "__main__":
    main()
