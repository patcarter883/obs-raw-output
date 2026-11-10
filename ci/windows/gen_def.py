"""Emit a .def file from a PE image's export directory.

The OBS Windows release ships DLLs but no import libraries, and the plugin has
to link against them. `lib /def:` turns an exports list into the .lib the linker
wants, so all this needs to produce is that list -- read straight from the PE
export table rather than scraped from dumpbin output, which keeps the parsing
testable off a Windows machine.

Usage: gen_def.py <dll> <out.def> [--min-exports N]
"""

import struct
import sys


def _rva_to_offset(sections, rva):
    for va, vsize, raw_size, raw_ptr in sections:
        if va <= rva < va + max(vsize, raw_size):
            return raw_ptr + (rva - va)
    raise ValueError(f"rva 0x{rva:x} is not inside any section")


def parse_sections(data):
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew : e_lfanew + 4] != b"PE\0\0":
        raise ValueError("not a PE image")

    coff = e_lfanew + 4
    num_sections = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]

    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    # PE32+ has a 64-bit image base; the data directories sit 112 bytes in.
    dd_off = opt + (112 if magic == 0x20B else 96)

    sections = []
    sec = opt + opt_size
    for i in range(num_sections):
        base = sec + i * 40
        vsize = struct.unpack_from("<I", data, base + 8)[0]
        va = struct.unpack_from("<I", data, base + 12)[0]
        raw_size = struct.unpack_from("<I", data, base + 16)[0]
        raw_ptr = struct.unpack_from("<I", data, base + 20)[0]
        sections.append((va, vsize, raw_size, raw_ptr))
    return sections, dd_off


def exported_names(path):
    with open(path, "rb") as fh:
        data = fh.read()

    sections, dd_off = parse_sections(data)
    export_rva, export_size = struct.unpack_from("<II", data, dd_off)
    if export_rva == 0:
        return []

    edir = _rva_to_offset(sections, export_rva)
    num_names = struct.unpack_from("<I", data, edir + 24)[0]
    names_rva = struct.unpack_from("<I", data, edir + 32)[0]
    names_off = _rva_to_offset(sections, names_rva)

    names = []
    for i in range(num_names):
        str_rva = struct.unpack_from("<I", data, names_off + i * 4)[0]
        start = _rva_to_offset(sections, str_rva)
        end = data.index(b"\0", start)
        names.append(data[start:end].decode("ascii"))
    return names


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    dll, out = argv[1], argv[2]
    minimum = 50
    if "--min-exports" in argv:
        minimum = int(argv[argv.index("--min-exports") + 1])

    names = exported_names(dll)
    if len(names) < minimum:
        print(
            f"gen_def: only {len(names)} exports in {dll}, expected at least {minimum}",
            file=sys.stderr,
        )
        return 1

    with open(out, "w", encoding="ascii", newline="\r\n") as fh:
        fh.write(f"LIBRARY {dll.rsplit('/', 1)[-1].rsplit(chr(92), 1)[-1]}\n")
        fh.write("EXPORTS\n")
        for name in names:
            fh.write(f" {name}\n")

    print(f"{out}: {len(names)} exports")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
