#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Merge named topology sections without replacing decoded base compounds."""

import pathlib
import re
import struct
import sys


def merge(base, delta):
    sections = re.findall(
        r'^Section(\w+)\."([^"\n]+)"\s*\{(.*?)^\}', delta, re.M | re.S
    )
    assert sections, "No named sections in backend fragment"
    for kind, name, body in sections:
        pattern = re.compile(r"^Section" + kind + r"\s*\{(.*?)^\}", re.M | re.S)
        match = pattern.search(base)
        entry = '\n\t"' + name + '" {' + body + '\n\t}\n'
        if match:
            # Reject duplicates instead of silently modifying an existing graph.
            assert not re.search(r"^[ \t]*['\"]" + re.escape(name) + r"['\"]\s*\{",
                                 match.group(1), re.M), (kind, name)
            position = match.end() - 1
            base = base[:position] + entry + base[position:]
        else:
            base += "\nSection" + kind + " {" + entry + "}\n"
    return base


if __name__ == "__main__":
    original, fragment, destination = map(pathlib.Path, sys.argv[1:4])
    assert destination.resolve() not in (original.resolve(), fragment.resolve())
    base = original.read_text()
    if base.startswith("SectionManifest.manifest.data.0 'manifest:data0'\n"):
        # alsatplg 1.2.16 emits a dangling compound for an empty manifest,
        # nesting the next widget's data under it. Prove the binary manifest
        # has no private data before removing that decoder artifact.
        binary = pathlib.Path(sys.argv[4]).read_bytes()
        header = struct.unpack_from("<9I", binary)
        assert header[0] == 0x41536f43 and header[3] == 8
        assert header[6] == 112
        assert struct.unpack_from("<I", binary, header[4] + 108)[0] == 0
        base = base.split("\n", 1)[1].replace("'manifest:data0'.", "", 1)
    destination.write_text(merge(base, fragment.read_text()))
