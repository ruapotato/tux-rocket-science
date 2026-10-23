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
## Headless verification entry point.
## Usage: godot --headless -- --test-kepler [--test-deltav ...] [--all-gates] [--strict]
## Native gates run inside trs_core; scripted gates live in res://scripts/gates/<gate>.gd
## and drive the game blind through the same control interface agents use.
extends Node

const SCRIPTED_DIR := "res://scripts/gates/"

var _strict := false


func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var wanted: Array[String] = []
	var all := false
	for a in args:
		if a == "--strict":
			_strict = true
		elif a == "--all-gates":
			all = true
		elif a.begins_with("--test-") or a == "--screens":
			wanted.append(a.substr(2))
	if wanted.is_empty() and not all:
		return  # normal game launch
	process_mode = Node.PROCESS_MODE_ALWAYS
	get_tree().paused = true  # keep the main scene inert while gates run
	_run(wanted, all)


func _run(wanted: Array[String], all: bool) -> void:
	var sim := TrsSim.new()
	print("Tux Rocket Science gate runner — trs_core ", sim.version())
	var roster: Array = JSON.parse_string(sim.gate_roster())
	if all:
		wanted.clear()
		for g in roster:
			if g.gate != "screens":
				wanted.append(g.gate)
	var known := {}
	for g in roster:
		known[g.gate] = g
	var failed := 0
	for name in wanted:
		if not known.has(name):
			printerr("unknown gate --", name)
			failed += 1
			continue
		var res: Dictionary
		if known[name].native:
			res = JSON.parse_string(sim.run_gate(name, ProjectSettings.globalize_path("res://data")))
			if name == "test-smoke" and res.status == "PASS":
				var b: Dictionary = await load(SCRIPTED_DIR + "test_smoke_binding.gd").new().run(self)
				res.status = b.status
				res.log += "binding: " + b.log
		else:
			res = await _run_scripted(name, known[name].milestone)
		print("[%-20s] %-8s (%s)" % [name, res.status, res.get("milestone", "")])
		for line in str(res.get("log", "")).split("\n", false):
			print("    ", line)
		if res.status == "FAIL" or res.status == "UNKNOWN" or (_strict and res.status == "PENDING"):
			failed += 1
	print("%d gate(s) FAILED" % failed if failed else "all gates ok")
	get_tree().quit(1 if failed else 0)


func _run_scripted(name: String, milestone: String) -> Dictionary:
	var path := SCRIPTED_DIR + name.replace("-", "_") + ".gd"
	if not ResourceLoader.exists(path):
		return {"status": "PENDING", "milestone": milestone, "log": "not implemented yet"}
	var gate: Object = load(path).new()
	var res: Dictionary = await gate.run(self)
	res["milestone"] = milestone
	return res
