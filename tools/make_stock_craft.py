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
"""Generates the three stock craft from the part catalog's attach nodes.

The hangar is the real authoring tool. This script only bootstraps the stock
craft so the gates have something to fly before the editor exists, and it
keeps them consistent if part dimensions change in the data.
"""
import json
import math
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAT = json.load(open(os.path.join(ROOT, "data", "parts", "stock_parts.json")))
DEFS = {p["id"]: p for p in CAT["parts"]}


def node(pid, name):
    for n in DEFS[pid]["nodes"]:
        if n["name"] == name:
            return n["pos"]
    raise KeyError(f"{pid}.{name}")


def yrot(deg):
    a = math.radians(deg) / 2
    return [0.0, round(math.sin(a), 15), 0.0, round(math.cos(a), 15)]


class Craft:
    def __init__(self, name, desc):
        self.name, self.desc, self.parts, self.sym = name, desc, [], 0

    def add(self, pid, parent=-1, node_="", parent_node="", pos=(0, 0, 0), rot=(0, 0, 0, 1), stage=-1, sym=0):
        self.parts.append({"part": pid, "parent": parent, "node": node_, "parent_node": parent_node,
                           "pos": [round(float(x), 12) for x in pos], "rot": list(rot), "stage": stage, "sym": sym})
        return len(self.parts) - 1

    def below(self, parent, pid, stage=-1):
        """Stack-attach pid's top node to parent's bottom node."""
        pp = self.parts[parent]["pos"]
        pb = node(self.parts[parent]["part"], "bottom")
        ct = node(pid, "top")
        pos = [pp[i] + pb[i] - ct[i] for i in range(3)]
        return self.add(pid, parent, "top", "bottom", pos, stage=stage)

    def above(self, parent, pid, stage=-1):
        """Stack-attach pid's bottom node to parent's top node."""
        pp = self.parts[parent]["pos"]
        pt = node(self.parts[parent]["part"], "top")
        cb = node(pid, "bottom")
        pos = [pp[i] + pt[i] - cb[i] for i in range(3)]
        return self.add(pid, parent, "bottom", "top", pos, stage=stage)

    def radial(self, parent, pid, count, y, offset_deg=0, stage=-1, outward=0.0):
        """Surface-attach `count` copies around parent at height y (symmetry)."""
        self.sym += 1
        r = DEFS[self.parts[parent]["part"]]["shape"].get("radius", 0.625) + outward
        out = []
        pp = self.parts[parent]["pos"]
        for k in range(count):
            ang = offset_deg + 360.0 * k / count
            a = math.radians(ang)
            pos = [pp[0] + r * math.sin(a), pp[1] + y, pp[2] + r * math.cos(a)]
            out.append(self.add(pid, parent, "", "", pos, yrot(ang), stage, self.sym))
        return out

    def boosters(self, tank, count, y, dec_stage, srb_stage):
        """Radial decoupler + solid booster (+ nosecone) per symmetry slot."""
        decs = self.radial(tank, "decoupler_radial", count, y, 90, dec_stage, outward=0.15)
        self.sym += 1
        sym = self.sym
        for d in decs:
            dp = self.parts[d]["pos"]
            rot = self.parts[d]["rot"]
            # outward unit vector from the decoupler's Y rotation
            ang = 2 * math.atan2(rot[1], rot[3])
            ox, oz = math.sin(ang), math.cos(ang)
            off = 0.15 + DEFS["srb"]["shape"]["radius"]
            pos = [dp[0] + ox * off, dp[1], dp[2] + oz * off]
            b = self.add("srb", d, "", "outer", pos, rot, srb_stage, sym)
            nt = node("srb", "top")
            nb = node("nosecone", "bottom")
            self.add("nosecone", b, "bottom", "top", [pos[0], pos[1] + nt[1] - nb[1], pos[2]], rot, -1, sym + 1)
        self.sym += 1

    def save(self, fname):
        doc = {"format": "trs-craft", "version": 1, "name": self.name, "description": self.desc,
               "parts": self.parts}
        path = os.path.join(ROOT, "data", "craft", fname)
        with open(path, "w") as f:
            json.dump(doc, f, indent=2)
            f.write("\n")
        print("wrote", os.path.relpath(path, ROOT), len(self.parts), "parts")


def hopper():
    c = Craft("Hopper", "Suborbital hop: straight up, then press P for the chute on the way down.")
    pod = c.add("pod_1")
    c.above(pod, "parachute")
    dec = c.below(pod, "decoupler_stack", stage=0)
    tank = c.below(dec, "tank_m")
    eng = c.below(tank, "engine_lifter", stage=1)
    c.radial(tank, "fin", 3, -0.6)
    c.save("hopper.craft.json")


def orbiter():
    c = Craft("Orbiter", "Two stages to orbit around Floe if you fly a clean gravity turn; shield first on the way home, then P for the chute.")
    pod = c.add("pod_1")
    c.above(pod, "parachute")
    rw = c.below(pod, "reaction_wheel")
    hs = c.below(rw, "heat_shield")
    dec1 = c.below(hs, "decoupler_stack", stage=0)
    t2 = c.below(dec1, "tank_s")
    e2 = c.below(t2, "engine_vac", stage=1)
    dec2 = c.below(e2, "decoupler_stack", stage=2)
    t1a = c.below(dec2, "tank_l")
    t1b = c.below(t1a, "tank_m")
    c.below(t1b, "engine_lifter", stage=3)
    c.radial(t1b, "fin", 3, -0.6)
    c.save("orbiter.craft.json")


def lander():
    c = Craft("Krill Lander", "Boosters, transfer stage, and a lander that can touch Krill and come home; drop the lander and come in shield first, then P for the chute.")
    pod = c.add("pod_1")
    c.above(pod, "parachute")
    rw = c.below(pod, "reaction_wheel")
    hs = c.below(rw, "heat_shield")
    d0 = c.below(hs, "decoupler_stack", stage=0)
    ls = c.below(d0, "tank_s")
    lt = c.below(ls, "tank_m")
    c.radial(lt, "landing_leg", 4, -0.5, 45)
    le = c.below(lt, "engine_small", stage=1)
    d1 = c.below(le, "decoupler_stack", stage=2)
    tt = c.below(d1, "tank_l")
    te = c.below(tt, "engine_vac", stage=3)
    d2 = c.below(te, "decoupler_stack", stage=4)
    ca = c.below(d2, "tank_l")
    cb = c.below(ca, "tank_l")
    c.below(cb, "engine_lifter", stage=6)
    c.radial(cb, "fin", 3, -1.4)
    c.boosters(cb, 2, 0.0, dec_stage=5, srb_stage=6)
    c.save("krill_lander.craft.json")


if __name__ == "__main__":
    hopper()
    orbiter()
    lander()
    # Rewrite in canonical form: the exact bytes the game itself saves.
    import glob
    import subprocess
    tool = os.path.join(ROOT, "build", "linux", "trs_tests")
    files = sorted(glob.glob(os.path.join(ROOT, "data", "craft", "*.craft.json")))
    if os.path.exists(tool):
        subprocess.run([tool, "--data", os.path.join(ROOT, "data"), "--normalize-craft", *files], check=True)
    else:
        print("note: build trs_tests to normalize the craft files")
