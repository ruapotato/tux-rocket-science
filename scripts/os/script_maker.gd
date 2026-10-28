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
# script_maker.gd -- the hangar's "Make script": a rocket's stage list as a
# flight script. Every part a stage fires gets a name (stage1_engine...);
# each stage waits for the pilot's space (wait_user_trigger) and then does
# what that press does. Run untouched, it flies exactly as pressing space
# does; swap a wait for your own test (a height, the fuel left) and that
# stage goes by itself.
#
# The part names are the flight computer's own (ShipComputer.part_names):
# what ship.part("...") finds aboard.

const KIND_WORD := {"engine": "engine", "booster": "booster", "decoupler": "decoupler",
	"parachute": "chute", "leg": "leg"}


## A part's kind, as the flight computer says it (vessel_part_states).
static func kind_of(d: Dictionary) -> String:
	if d.has("engine"):
		return "booster" if str(d.engine.get("propellant", "")) == "solid" else "engine"
	if d.has("decoupler"):
		return "decoupler"
	if d.has("parachute"):
		return "parachute"
	if d.has("leg"):
		return "leg"
	if float(d.get("fuel_liquid", 0.0)) > 0.0:
		return "tank"
	if int(d.get("crew", 0)) > 0:
		return "pod"
	return "part"


## parts: the craft's parts ({part, stage, ...}), as built; in flight, only
## the stages up to next_stage are still to fire.
static func make(parts: Array, craft_name: String, next_stage := 999) -> String:
	# the flight computer's view of them: uid in build order, name, kind
	var flat := []
	for i in parts.size():
		var d: Dictionary = Sim.part(str(parts[i].part))
		flat.append({"uid": i, "name": str(d.get("name", parts[i].part)), "stage": int(parts[i].get("stage", -1)), "kind": kind_of(d)})
	var names := ShipComputer.part_names(flat)
	var stages := []
	for p in flat:
		if int(p.stage) >= 0 and int(p.stage) <= next_stage and not int(p.stage) in stages:
			stages.append(int(p.stage))
	stages.sort()
	stages.reverse()   # firing order: the highest goes first
	var out := PackedStringArray()
	var file := craft_name.to_lower().replace(" ", "_")
	out.append("# %s.py -- written by the hangar from %s's stages." % [file, craft_name])
	out.append("#")
	out.append("# Run as it is, it flies just as pressing space does: each wait_user_trigger()")
	out.append("# waits for your space, then does that stage. Swap one for your own test,")
	out.append("#     while ship.altitude < 30 * km: sleep(0.5)")
	out.append("# and that stage goes by itself. (Open it in the Script Editor: it suggests")
	out.append("# what each part has as you type its name and a dot.)")
	out.append("")
	out.append("# ---- your parts, by the stage that fires them ----")
	var acts := {}   # stage -> lines
	var press := 0
	for sg in stages:
		press += 1
		var lines := PackedStringArray()
		var by_kind := {}
		for p in ShipComputer._order(flat):
			if int(p.stage) != sg or not KIND_WORD.has(str(p.kind)):
				continue
			var k := str(p.kind)
			if not by_kind.has(k):
				by_kind[k] = []
			by_kind[k].append(p)
		for k in ["engine", "booster", "decoupler", "parachute", "leg"]:
			if not by_kind.has(k):
				continue
			var ps: Array = by_kind[k]
			var var_name := "stage%d_%s" % [press, KIND_WORD[k]]
			# one part: its name; more of the same kind: a list of them
			if ps.size() == 1:
				out.append("%s = ship.part(\"%s\")" % [var_name, names[int(ps[0].uid)]])
				lines.append(_do(var_name, k))
			else:
				var each := PackedStringArray()
				for p in ps:
					each.append("ship.part(\"%s\")" % names[int(p.uid)])
				out.append("%ss = [%s]" % [var_name, ", ".join(each)])
				lines.append("for p in %ss:" % var_name)
				lines.append("    " + _do("p", k))
		acts[sg] = lines
	var tanks := PackedStringArray()
	for p in ShipComputer._order(flat):
		if str(p.kind) == "tank":
			tanks.append(names[int(p.uid)])
	if not tanks.is_empty():
		out.append("# (tanks, for their .fuel: %s)" % ", ".join(tanks))
	out.append("")
	press = 0
	for sg in stages:
		press += 1
		out.append("# ---- SPACE #%d: stage %d ----" % [press, sg])
		out.append("wait_user_trigger()")
		for l in acts[sg]:
			out.append(l)
		if (acts[sg] as PackedStringArray).is_empty():
			out.append("stage()   # (nothing here a script names: the stage itself)")
		out.append("")
	out.append("print(\"every stage done\")")
	return "\n".join(out) + "\n"


static func _do(who: String, kind: String) -> String:
	match kind:
		"engine": return "%s.throttle = 100    # lights it, at full" % who
		"booster": return "%s.ignite()         # a solid: once lit, it burns out" % who
		"decoupler": return "%s.decouple()" % who
		"parachute": return "%s.deploy()         # opens when the air is thick and slow enough" % who
		"leg": return "%s.deploy()" % who
	return "pass"
