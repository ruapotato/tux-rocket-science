#!/usr/bin/env python3
# Tux Rocket Science
# Copyright (C) 2026 Tux Rocket Science contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Check (default) or apply (--fix) the AGPL header on every source file."""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARK = "SPDX-License-Identifier: AGPL-3.0-or-later"
SKIP_DIRS = {".git", ".godot", "build", "third_party", "bin", ".github"}
C_STYLE = {".c", ".h"}
HASH_STYLE = {".gd", ".py", ".sh", ".cmake", ".txt"}


def header_lines():
    with open(os.path.join(ROOT, "tools", "license_header.txt")) as f:
        return f.read().rstrip("\n").split("\n")


def wanted(path):
    name = os.path.basename(path)
    ext = os.path.splitext(name)[1]
    if name == "CMakeLists.txt":
        return "#"
    if ext in C_STYLE:
        return "c"
    if ext in HASH_STYLE and ext != ".txt":
        return "#"
    return None


def render(style):
    lines = header_lines()
    if style == "c":
        return "/*\n" + "".join((" * " + l).rstrip() + "\n" for l in lines) + " */\n"
    return "".join(("# " + l).rstrip() + "\n" for l in lines)


def main():
    fix = "--fix" in sys.argv
    missing = []
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            p = os.path.join(dirpath, fn)
            style = wanted(p)
            if style is None:
                continue
            with open(p, encoding="utf-8") as f:
                text = f.read()
            if MARK in text[:2000]:
                continue
            if not fix:
                missing.append(os.path.relpath(p, ROOT))
                continue
            head = render(style)
            if text.startswith("#!"):
                first, _, rest = text.partition("\n")
                text = first + "\n" + head + rest
            else:
                text = head + ("\n" if style == "c" else "") + text
            with open(p, "w", encoding="utf-8") as f:
                f.write(text)
            print("header added:", os.path.relpath(p, ROOT))
    if missing:
        print("Missing AGPL header:\n  " + "\n  ".join(missing))
        return 1
    if not fix:
        print("license headers OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
