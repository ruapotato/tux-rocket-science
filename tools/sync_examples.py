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
"""The examples on the ship's disk that test-by-hand flies, kept the same.

The gate (scripts/gates/test_by_hand.gd) holds to_orbit.py and
krill_by_hand.py as constants; this writes them into os/machine/image.c
between the BEGIN/END markers, as C string literals. Run it after changing
either, then rebuild (ninja -C build/linux).
"""
import re
import sys

GATE = "scripts/gates/test_by_hand.gd"
IMAGE = "os/machine/image.c"

HEADS = {
    "to_orbit.py": "#   ship run /root/examples/to_orbit.py      (on the pad, in the Orbiter)\n#\n"
                   "# The pad to orbit with your own logic: the parts by name, a pitch program\n"
                   "# you can change, the maths for the burn at the top, and a look afterwards\n"
                   "# that goes round again if the orbit isn't round enough. Flown as it is by\n"
                   "# the game's test (test-by-hand) every build.\n",
    "krill_by_hand.py": "#   ship run /root/examples/krill_by_hand.py     (on the pad, in the Krill Lander)\n#\n"
                        "# Up by the Guidance Computer, then to Krill and down by you: the transfer\n"
                        "# worked out by when_to_leave, a course fix found by trying pushes with\n"
                        "# what_if (nothing is flown until it's found), a capture by the maths and a\n"
                        "# landing written out. Flown as it is by test-by-hand every build.\n",
}


def const(src, name):
    m = re.search(r'const %s := """(.*?)"""' % name, src, re.S)
    return m.group(1)


def c_lit(text):
    out = []
    for line in text.splitlines(True):
        esc = line.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
        out.append('        "%s"\n' % esc)
    return "".join(out)


def main():
    src = open(GATE).read()
    to_orbit = const(src, "TO_ORBIT").replace("TRIGGER1\n", "").replace("        TRIGGER2\n", "")
    krill = const(src, "KRILL")
    block = ["    /* BEGIN test-by-hand examples (tools/sync_examples.py writes these) */\n"]
    for name, text in [("to_orbit.py", to_orbit), ("krill_by_hand.py", krill)]:
        lines = text.split("\n", 1)
        body = lines[0] + "\n#\n" + HEADS[name] + lines[1]
        block.append('    else if (strcmp(path, "/root/examples/%s") == 0)\n        buf_puts(out,\n%s        );\n' % (name, c_lit(body)))
    block.append("    /* END test-by-hand examples */\n")
    img = open(IMAGE).read()
    a = img.find("    /* BEGIN test-by-hand examples")
    if a >= 0:
        b = img.index("    /* END test-by-hand examples */\n") + len("    /* END test-by-hand examples */\n")
        img = img[:a] + "".join(block) + img[b:]
    else:
        anchor = '    else if (strcmp(path, "/root/examples/hop.py") == 0)\n'
        assert img.count(anchor) == 1
        img = img.replace(anchor, "".join(block) + anchor)
    open(IMAGE, "w").write(img)
    print("wrote to_orbit.py (%d lines) and krill_by_hand.py (%d lines) into %s" % (to_orbit.count("\n"), krill.count("\n"), IMAGE))


if __name__ == "__main__":
    sys.exit(main())
