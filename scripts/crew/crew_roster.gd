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
## PASA astronaut corps: named penguins that persist across flights in
## user://crew.json. Each has a look (from a seed) and two traits that shape
## how they react in flight: nerve (calm under G) and glee (how much they
## enjoy it). Flights, records and the occasional infirmary stay are kept.
extends Node

var path := "user://crew.json"  # gates switch to a scratch roster
const FIRST := ["Pip", "Gus", "Nell", "Otto", "Mabel", "Tobias", "Wren", "Juniper", "Hector", "Olive", "Barnaby",
	"Clementine", "Fitz", "Rosie", "Ignatius", "Dot", "Ferdinand", "Hazel", "Quill", "Winnie"]
const LAST := ["Floeberg", "Icewing", "Snowfeather", "Krillsworth", "Waddlesby", "Frostbeak", "Glacierton",
	"Slushman", "Puddlefoot", "Spratley", "Brinesby", "Pebblechin", "Driftwhistle", "Hailstone"]
const ACCESSORIES := ["scarf", "bowtie", "goggles", "beanie", "none", "headset", "monocle", "bandana"]

var crew: Array = []  # [{id, name, seed, nerve, glee, accessory, color, flights, status, records}]
var _rng := RandomNumberGenerator.new()


func _ready() -> void:
	load_roster()


func load_roster() -> void:
	crew.clear()
	if FileAccess.file_exists(path):
		var d: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
		if typeof(d) == TYPE_DICTIONARY and d.has("crew"):
			crew = d.crew
	if crew.is_empty():
		_found_corps()
		save_roster()


func save_roster() -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f:
		f.store_string(JSON.stringify({"version": 1, "crew": crew}, "  "))


## The first PASA class: Commander Tux plus seven recruits.
func _found_corps() -> void:
	_rng.seed = 1991  # deterministic first class, same for every new player
	crew.append(_make("Commander Tux", 1991, 0.9, 0.7, "scarf", "#e8a23a"))
	var firsts := FIRST.duplicate()
	var lasts := LAST.duplicate()
	while crew.size() < 8:
		var f: String = firsts.pop_at(_rng.randi() % firsts.size())
		var l: String = lasts.pop_at(_rng.randi() % lasts.size())
		var seed_state := _rng.state
		recruit_named(f + " " + l)
		_rng.state = seed_state


func _make(n: String, seed: int, nerve: float, glee: float, acc: String, color: String) -> Dictionary:
	return {
		"id": n.to_snake_case(), "name": n, "seed": seed, "nerve": snappedf(nerve, 0.01), "glee": snappedf(glee, 0.01),
		"accessory": acc, "color": color, "flights": 0, "status": "available",
		"records": {"max_altitude": 0.0, "max_g": 0.0, "orbits": 0, "krill_landings": 0, "visited": []},
	}


func recruit_named(n: String) -> Dictionary:
	var seed := hash(n) & 0x7fffffff
	_rng.seed = seed
	var colors := ["#d84a3a", "#3a7bd8", "#3ab07a", "#e8c23a", "#9a5ad8", "#e87a3a", "#3ac0d0", "#d83a8a"]
	var c := _make(n, seed, _rng.randf_range(0.15, 0.95), _rng.randf_range(0.2, 1.0),
		ACCESSORIES[_rng.randi() % ACCESSORIES.size()], colors[_rng.randi() % colors.size()])
	crew.append(c)
	return c


func recruit_random() -> Dictionary:
	for i in 100:
		var n: String = FIRST[randi() % FIRST.size()] + " " + LAST[randi() % LAST.size()]
		if find(n).is_empty():
			var c := recruit_named(n)
			save_roster()
			return c
	return {}


func find(name_or_id: String) -> Dictionary:
	for c in crew:
		if c.name == name_or_id or c.id == name_or_id:
			return c
	return {}


func available() -> Array:
	return crew.filter(func(c): return c.status == "available")


## Picks crew for `seats` seats: the named ones first, then whoever is free.
func assign(seats: int, wanted: Array = []) -> Array:
	# never launch an empty seat: recruit if the corps has run short
	while available().size() < seats:
		if recruit_random().is_empty():
			break
	var out := []
	for n in wanted:
		var c := find(n)
		if not c.is_empty() and c.status == "available" and out.size() < seats:
			out.append(c.name)
	for c in available():
		if out.size() >= seats:
			break
		if not out.has(c.name):
			out.append(c.name)
	return out


## Records a completed flight. outcome: "recovered" | "lost"
func record_flight(names: Array, outcome: String, stats: Dictionary) -> void:
	if outcome == "lost":
		record_loss(names, stats)
		return
	for n in names:
		var c := find(n)
		if c.is_empty() or c.status == "lost":
			continue
		_add_stats(c, stats)
		c.status = "available"
	# anyone in the infirmary from before gets better after a flight
	for c in crew:
		if c.status == "infirmary" and not names.has(c.name):
			c.status = "available"
	save_roster()


## The crew did not come home. Lost penguins stay on the roster (with their
## records) but never fly again; PASA recruits replacements as needed.
func record_loss(names: Array, stats: Dictionary) -> void:
	for n in names:
		var c := find(n)
		if c.is_empty() or c.status == "lost":
			continue
		_add_stats(c, stats)
		c.status = "lost"
	save_roster()


func _add_stats(c: Dictionary, stats: Dictionary) -> void:
	c.flights += 1
	var r: Dictionary = c.records
	r.max_altitude = maxf(r.max_altitude, stats.get("max_altitude", 0.0))
	r.max_g = maxf(r.max_g, stats.get("max_g", 0.0))
	r.orbits += int(stats.get("orbits", 0))
	r.krill_landings += int(stats.get("krill_landings", 0))
	for b in stats.get("visited", []):
		if not r.visited.has(b):
			r.visited.append(b)
