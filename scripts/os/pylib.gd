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
# pylib.gd -- the ship computer's Python, as the Script Editor suggests it:
# every call /bin/py has (os/guest/py.c's NATIVES), what it takes and what
# it does, in a line. test-script-editor checks every name here is one
# py really answers.

const CALLS := [
	# -- the ship
	["countdown", "(seconds=5)", "counts down on the HUD, a second at a time"],
	["sleep", "(seconds)", "waits this long in game time (time warp eases off before it ends)"],
	["wait", "(seconds)", "the same as sleep"],
	["wait_until", "(reading, \">\", value)", "waits until a reading passes a value: wait_until(\"altitude\", \">\", 5000)"],
	["wait_user_trigger", "()", "waits for you to press space (the press is the script's, not a stage)"],
	["wait_for_space", "()", "the same as wait_user_trigger"],
	["throttle", "(percent)", "every engine's throttle, 0 to 100 (one engine's own: engine.throttle = 50)"],
	["stage", "()", "fires the next stage, as space does"],
	["aim", "(\"prograde\")", "points the nose and holds it: prograde retrograde normal antinormal radial_out radial_in up surface_retrograde"],
	["aim_at", "(pitch, heading)", "points the nose: degrees above the horizon, degrees from north (90 = east); holds it there as the ship goes round"],
	["aim_along", "(forward, sideways, outward)", "points the nose along a burn (what_if's three numbers): forward is prograde, sideways normal, outward radial"],
	["burn", "(dv)", "burns this many m/s along the nose, then cuts the throttle (burn(\"prograde\", dv) points it first)"],
	["burn_until", "(\"prograde\", reading, \">\", value)", "burns until a reading passes a value"],
	["sas", "(\"stability\")", "SAS mode: off stability prograde retrograde normal antinormal radial_out radial_in target"],
	["point", "(pitch, heading)", "points the nose without waiting"],
	["chutes", "()", "arms the parachutes"],
	["legs", "()", "landing legs out (legs(False): in)"],
	["rcs", "(\"on\")", "RCS thrusters on or off"],
	["say", "(text)", "puts a line on the flight screen"],
	["beep", "(\"alarm\")", "a sound: beep (the default), alarm or chime"],
	["chart", "(name, value)", "a point on the Flight app's graph: chart(\"height\", ship.altitude)"],
	["get", "(reading)", "one reading by name, as a number or a word: get(\"apoapsis\")"],
	["target", "(name)", "picks a vessel to target"],
	["autostage", "(False)", "whether the programs stage when a stage is spent"],
	# -- the maths
	["speed_needed", "(height)", "the speed of a circular orbit that high (m/s)"],
	["speed_at", "(height)", "how fast this orbit goes when it is that high (m/s)"],
	["burn_time", "(dv)", "seconds at full thrust for that much delta-v, on the engines burning now"],
	["time_to", "(\"top\")", "seconds until the top (or \"bottom\") of the orbit"],
	["stopping_distance", "()", "how far it takes to stop from this speed, flat out, against this gravity (m)"],
	["when_to_leave", "(\"krill\")", "works out the burn to a moon or planet: {wait, at, dv, travel, closest, body}"],
	["what_if", "(forward, sideways, outward, when)", "the path as if that burn were made then: {top, bottom, meets, closest, arrive}"],
	["clamp", "(x, low, high)", "x, kept between low and high"],
	["sqrt", "(x)", "square root"], ["sin", "(radians)", ""], ["cos", "(radians)", ""], ["tan", "(radians)", ""],
	["asin", "(x)", "radians"], ["acos", "(x)", "radians"], ["atan", "(x)", "radians"], ["atan2", "(y, x)", "radians"],
	["rad", "(degrees)", "degrees to radians"], ["deg", "(radians)", "radians to degrees"],
	["exp", "(x)", ""], ["log", "(x)", "natural log"],
	["abs", "(x)", ""], ["min", "(a, b, ...)", ""], ["max", "(a, b, ...)", ""],
	["round", "(x, places=0)", ""], ["floor", "(x)", ""], ["ceil", "(x)", ""],
	["float", "(x)", "a number from text"], ["int", "(x)", "a whole number"],
	["format", "(x, \".1f\")", "a number as text, as f\"{x:.1f}\" does"],
	# -- the ship's programs (the guidance computer flies these itself)
	["launch", "(apoapsis)", "PROGRAM: off the ground to orbit, staging itself"],
	["circularize", "()", "PROGRAM: rounds the orbit off at the top"],
	["transfer", "(\"krill\")", "PROGRAM: plans and flies the burn to a body, with corrections"],
	["wait_for_window", "(\"krill\")", "PROGRAM: warps to when the burn to that body starts"],
	["until_orbit_crosses", "(\"krill\")", "PROGRAM: burns prograde until the path meets it"],
	["warp_to", "(\"apoapsis\")", "PROGRAM: time warp: seconds, apoapsis, periapsis, soi, node, or a body"],
	["capture", "(apoapsis)", "PROGRAM: brakes into orbit at the bottom"],
	["land", "()", "PROGRAM: down onto the ground below"],
	["hover", "(height, seconds)", "PROGRAM: holds a height above the ground"],
	["node", "()", "PROGRAM: flies the map's maneuver node"],
	["abort", "()", "stops the program running and closes the throttle"],
	# -- the language
	["print", "(...)", "to the console"],
	["dir", "(x)", "what x has: dir(ship), dir(engine) -- its readings, and (with ()) what it does"],
	["help", "()", "where to look"],
	["len", "(x)", ""], ["str", "(x)", ""], ["split", "(text, sep)", ""], ["join", "(list, sep)", ""],
	["append", "(list, x)", ""], ["keys", "(dict)", ""], ["has", "(dict, key)", ""],
	["find", "(text, part)", ""], ["replace", "(text, from, to)", ""], ["strip", "(text)", ""],
	["lower", "(text)", ""], ["upper", "(text)", ""], ["sub", "(text, from, to)", "a slice of text"],
	["startswith", "(text, part)", ""], ["endswith", "(text, part)", ""],
	["read", "(path)", "a file's text"], ["write", "(path, text)", ""], ["exit", "(code=0)", "ends the script"],
	["json", "(text)", ""], ["lines", "(text)", ""], ["do", "(command)", "one `ship` command, as a button sends it"],
	# -- the rest of Python's builtins
	["range", "(stop) / (start, stop, step)", "the numbers from start up to (not including) stop"],
	["list", "(xs)", "a list of them"], ["tuple", "(xs)", "a tuple of them"], ["dict", "(pairs)", "a dict (dict(a=1), dict(zip(keys, values)))"],
	["set", "(xs)", "them with no repeats: | & - work"], ["frozenset", "(xs)", "the same as set here"],
	["sum", "(xs, start=0)", ""], ["sorted", "(xs, key=f, reverse=False)", "a sorted copy"], ["reversed", "(xs)", ""],
	["any", "(xs)", "True if any is"], ["all", "(xs)", "True if all are"],
	["enumerate", "(xs, start=0)", "(i, x) pairs: for i, x in enumerate(xs)"], ["zip", "(xs, ys, ...)", "pairs, side by side"],
	["map", "(f, xs)", "f of each"], ["filter", "(f, xs)", "the ones f says True to"],
	["bool", "(x)", ""], ["repr", "(x)", "x as Python writes it"], ["type", "(x)", "what x is: type(x).__name__"],
	["isinstance", "(x, kind)", "isinstance(x, (int, float))"], ["callable", "(x)", "can it be called"],
	["next", "(generator, default)", "a generator's next value"], ["iter", "(xs)", ""],
	["ord", "(letter)", "its code"], ["chr", "(code)", "the letter"], ["hex", "(n)", "'0xff'"], ["bin", "(n)", "'0b101'"], ["oct", "(n)", ""],
	["hash", "(x)", ""], ["id", "(x)", ""], ["divmod", "(a, b)", "(a // b, a % b)"], ["pow", "(x, y)", ""],
	["getattr", "(x, \"name\", default)", "x.name, by its name"], ["hasattr", "(x, \"name\")", ""], ["setattr", "(x, \"name\", value)", ""],
	["open", "(path, \"w\")", "a file on the ship's computer: with open(\"/root/log.csv\", \"w\") as f: f.write(...)"],
	["staticmethod", "(f)", "@staticmethod"], ["classmethod", "(f)", "@classmethod"], ["property", "(f)", "@property: worked out when read"],
	["Exception", "(\"why\")", "raise it; class MyError(Exception): ..."], ["ValueError", "(\"why\")", ""], ["TypeError", "(\"why\")", ""],
	["KeyError", "(key)", ""], ["IndexError", "(\"why\")", ""], ["ZeroDivisionError", "(\"why\")", ""], ["RuntimeError", "(\"why\")", ""],
	["AttributeError", "(\"why\")", ""], ["AssertionError", "(\"why\")", ""], ["StopIteration", "()", ""], ["NameError", "(\"why\")", ""],
]

const KEYWORDS := ["if", "elif", "else", "while", "for", "in", "break", "continue", "def", "return",
	"and", "or", "not", "True", "False", "None", "pass", "global", "nonlocal", "import", "from", "try", "except",
	"finally", "raise", "lambda", "is", "as", "assert", "class", "del", "yield", "with", "super"]

const NAMES := ["ship", "km", "pi", "math", "time", "random", "self"]

## A string's, a list's, a dict's, a set's, a file's methods, and the
## modules' (after a dot on a name the script set to one).
const STR := [
	["split", "(sep=None, maxsplit=-1)", "the pieces"], ["join", "(xs)", "\", \".join(names)"], ["strip", "(chars)", "spaces off the ends"],
	["lstrip", "(chars)", ""], ["rstrip", "(chars)", ""], ["lower", "()", ""], ["upper", "()", ""], ["title", "()", ""],
	["capitalize", "()", ""], ["replace", "(old, new, count)", ""], ["find", "(part)", "where, or -1"], ["rfind", "(part)", ""],
	["index", "(part)", "where (a ValueError if not)"], ["count", "(part)", ""], ["startswith", "(part)", ""], ["endswith", "(part)", ""],
	["format", "(...)", "\"{} at {:.1f}\".format(a, b)"], ["isdigit", "()", ""], ["isalpha", "()", ""], ["isspace", "()", ""],
	["splitlines", "()", ""], ["partition", "(sep)", "(before, sep, after)"], ["zfill", "(width)", ""], ["center", "(width)", ""],
	["ljust", "(width)", ""], ["rjust", "(width)", ""], ["removeprefix", "(part)", ""], ["removesuffix", "(part)", ""],
]
const LIST := [
	["append", "(x)", "on the end"], ["extend", "(xs)", ""], ["insert", "(i, x)", ""], ["pop", "(i=-1)", "takes one off (the last)"],
	["remove", "(x)", "the first that equals x"], ["index", "(x)", ""], ["count", "(x)", ""], ["sort", "(key=f, reverse=False)", "in place"],
	["reverse", "()", "in place"], ["copy", "()", ""], ["clear", "()", ""],
]
const DICT := [
	["keys", "()", ""], ["values", "()", ""], ["items", "()", "(key, value) pairs: for k, v in d.items()"], ["get", "(key, default=None)", ""],
	["pop", "(key, default)", ""], ["setdefault", "(key, value)", ""], ["update", "(other)", ""], ["copy", "()", ""], ["clear", "()", ""],
	["popitem", "()", ""],
]
const SET := [
	["add", "(x)", ""], ["discard", "(x)", "out, if it's in"], ["remove", "(x)", "out (a KeyError if not)"], ["pop", "()", ""],
	["union", "(other)", "| does it too"], ["intersection", "(other)", "&"], ["difference", "(other)", "-"],
	["symmetric_difference", "(other)", "^"], ["issubset", "(other)", "<="], ["issuperset", "(other)", ">="],
	["isdisjoint", "(other)", ""], ["update", "(other)", ""], ["copy", "()", ""], ["clear", "()", ""],
]
const FILE := [
	["write", "(text)", "onto the end of the file"], ["read", "()", "the rest of it"], ["readline", "()", "the next line"],
	["readlines", "()", "every line"], ["close", "()", "(with open(...) as f: closes it for you)"],
]
const MODULES := {
	"math": [["sqrt", "(x)", ""], ["sin", "(radians)", ""], ["cos", "(radians)", ""], ["tan", "(radians)", ""],
		["asin", "(x)", ""], ["acos", "(x)", ""], ["atan", "(x)", ""], ["atan2", "(y, x)", ""], ["degrees", "(radians)", ""],
		["radians", "(degrees)", ""], ["hypot", "(x, y)", ""], ["exp", "(x)", ""], ["log", "(x, base=e)", ""], ["log10", "(x)", ""],
		["log2", "(x)", ""], ["floor", "(x)", ""], ["ceil", "(x)", ""], ["trunc", "(x)", ""], ["fabs", "(x)", ""],
		["isclose", "(a, b, rel_tol=1e-9)", ""], ["copysign", "(x, y)", ""], ["fmod", "(x, y)", ""], ["factorial", "(n)", ""],
		["gcd", "(a, b)", ""], ["dist", "(p, q)", "between two points"], ["prod", "(xs)", ""], ["fsum", "(xs)", "an exact sum"],
		["isfinite", "(x)", ""], ["isnan", "(x)", ""], ["pi", "3.14159...", ""], ["e", "2.71828...", ""], ["tau", "2 pi", ""], ["inf", "", ""]],
	"time": [["time", "()", "game seconds"], ["sleep", "(seconds)", ""], ["monotonic", "()", ""]],
	"random": [["random", "()", "0 to 1"], ["uniform", "(a, b)", ""], ["randint", "(a, b)", "a to b, both"], ["choice", "(xs)", ""], ["seed", "(n)", ""]],
}

## What the ship has (ship.X), beyond its readings (`ship status`).
const SHIP := [
	["altitude", "m above sea level"], ["height", "m above the ground below"], ["speed", "m/s over the ground"],
	["vertical_speed", "m/s, + up"], ["falling_speed", "m/s, + down"], ["horizontal_speed", "m/s"],
	["mass", "kg"], ["delta_v", "m/s left, every stage"], ["thrust", "kN now"], ["max_thrust", "kN, flat out"],
	["throttle", "%, every engine (set it)"], ["stage", "the next stage's number"], ["fuel", "% left in this stage"],
	["landed", "True on the ground"], ["situation", "flying, orbiting, landed..."],
	["orbit", "its orbit: .top .bottom .period .time_to_top ..."], ["body", "where it is: .name .radius .mu .gravity ..."],
	["parts", "every part (ship.parts(\"Swivel\"): the ones called that)"], ["engines", "the engines"],
	["boosters", "the solid boosters"], ["tanks", "the tanks"], ["decouplers", "the decouplers"],
	["parachutes", "the parachutes"], ["legs", "the landing legs"], ["pods", "the crew pods"],
	["part", "(\"Walrus\"): the one part called that"],
]

const ORBIT := [
	["top", "m, the highest it goes"], ["bottom", "m, the lowest"], ["apoapsis", "the top"], ["periapsis", "the bottom"],
	["time_to_top", "s"], ["time_to_bottom", "s"], ["period", "s round"], ["eccentricity", "0 is a circle"],
	["inclination", "degrees"], ["speed", "m/s"], ["semi_major_axis", "m"], ["body", "the body's name"],
]

const BODY := [
	["name", ""], ["radius", "m"], ["mu", "m3/s2 (gravity's strength)"], ["gravity", "m/s2 at the ground"],
	["atmosphere", "m, how high the air goes (0: none)"], ["has_air", "True or False"], ["sphere", "m, how far its pull wins"],
	["day", "s, one turn"],
]

## Every part's, by kind (and every part has name kind stage temp heat).
const PART := [
	["name", "its name aboard"], ["kind", "engine booster tank decoupler parachute leg pod part"], ["stage", ""],
	["temp", "K"], ["heat", "% of what melts it"],
	["throttle", "%, its own (set it: lights it)"], ["fuel", "% left (an engine: in what feeds it)"], ["fuel_kg", "kg"],
	["lit", "True when it's on"], ["thrust", "kN now"], ["max_thrust", "kN"], ["isp", "s"],
	["ignite", "(): lights it"], ["shutdown", "(): puts it out"], ["decouple", "(): fires it"],
	["fired", "True once it has"], ["deploy", "(): opens (chutes, legs)"], ["cut", "(): cuts a chute away"],
	["retract", "(): legs in"], ["deployed", "legs: True when out"], ["state", "chutes: stowed armed opening open cut"],
	["capacity_kg", "a tank's"],
]
