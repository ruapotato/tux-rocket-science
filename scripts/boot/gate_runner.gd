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
var _errors := ErrorCounter.new()


## Counts script and engine errors so a gate that "passes" while the game
## spews errors underneath still fails.
class ErrorCounter extends Logger:
	var count := 0
	var first := ""
	var _lock := Mutex.new()

	func _log_error(function: String, file: String, line: int, code: String, rationale: String, _editor_notify: bool,
			error_type: int, _script_backtraces: Array[ScriptBacktrace]) -> void:
		if error_type == ERROR_TYPE_WARNING:
			return
		_lock.lock()
		count += 1
		if first == "":
			first = "%s (%s:%d in %s)" % [rationale if rationale != "" else code, file, line, function]
		_lock.unlock()

	func _log_message(_message: String, _error: bool) -> void:
		pass


func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var wanted: Array[String] = []
	var all := false
	for a in args:
		if a == "--strict":
			_strict = true
		elif a == "--all-gates":
			all = true
		elif a.begins_with("--test-") or a.begins_with("--dev-") or a == "--screens":
			wanted.append(a.substr(2))
	if wanted.is_empty() and not all:
		return  # normal game launch
	OS.add_logger(_errors)
	Sim.use_sandbox()  # never touch the player's saved game
	process_mode = Node.PROCESS_MODE_ALWAYS
	get_tree().paused = true  # keep the main scene inert while gates run
	_arm_watchdog()
	_run.call_deferred(wanted, all)  # let the tree finish setting up first


## A gate that errors mid-await would otherwise hang the runner forever.
func _arm_watchdog() -> void:
	var limit := 1800.0
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--gate-timeout="):
			limit = float(a.get_slice("=", 1))
	var t := Timer.new()
	t.process_mode = Node.PROCESS_MODE_ALWAYS
	t.one_shot = true
	t.wait_time = limit
	t.timeout.connect(func():
		printerr("gate runner watchdog: no result after %.0f s, failing" % limit)
		get_tree().quit(3))
	add_child(t)
	t.start()


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
		if name.begins_with("dev-"):
			known[name] = {"gate": name, "milestone": "dev", "native": false}
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
			var before := _errors.count
			_errors.first = ""
			res = await _run_scripted(name, known[name].milestone)
			if _errors.count > before and res.status == "PASS":
				res.status = "FAIL"
				res.log = str(res.get("log", "")) + "\n%d error(s) while running, first: %s" % [_errors.count - before, _errors.first]
		print("[%-20s] %-8s (%s)" % [name, res.status, res.get("milestone", "")])
		for line in str(res.get("log", "")).split("\n", false):
			print("    ", line)
		if res.status == "FAIL" or res.status == "UNKNOWN" or (_strict and res.status == "PENDING"):
			failed += 1
	print("%d gate(s) FAILED" % failed if failed else "all gates ok")
	get_tree().quit(1 if failed else 0)


func _run_scripted(name: String, milestone: String) -> Dictionary:
	var path := SCRIPTED_DIR + name.replace("-", "_") + ".gd"
	if name.begins_with("dev-"):
		path = SCRIPTED_DIR + "dev/" + name.substr(4).replace("-", "_") + ".gd"
	if not ResourceLoader.exists(path):
		return {"status": "PENDING", "milestone": milestone, "log": "not implemented yet"}
	var script: GDScript = load(path)
	if script == null or not script.can_instantiate():
		return {"status": "FAIL", "milestone": milestone, "log": "gate script failed to load: " + path}
	var gate: Object = script.new()
	var res: Variant = await gate.run(self)
	if typeof(res) != TYPE_DICTIONARY or not res.has("status"):
		return {"status": "FAIL", "milestone": milestone, "log": "gate returned no result (script error?)"}
	res["milestone"] = milestone
	return res
