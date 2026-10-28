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
# codeapp.gd -- the Script Editor: write a flight script, run it, and watch
# it think.
#
#   the editor     Python, coloured; suggestions as you type: every call
#                  with what it takes, and after a dot what the ship really
#                  has (engine. offers throttle, fuel, ignite()...; inside
#                  ship.part(" the names of the parts aboard)
#   breakpoints    click the gutter; Run stops before that line
#   the watch      every variable the running script has, live, and the
#                  line it is on (marked in the editor)
#   the console    type Python and it runs on the ship, now, keeping its
#                  variables (py -i in the background): x = ship.orbit.top,
#                  then x / km
#
# All of it goes through the machine: the file is on its disk (cat shows
# it), Run is `ship run <file>`, the watch is /run/py/<pid>.vars that the
# script writes, pause and step are /run/py/<pid>.ctl that this writes, and
# the console is /run/py/console.in. Nothing here can do what a terminal
# could not.

extends Control

const PyLib := preload("res://scripts/os/pylib.gd")
const Templates := preload("res://scripts/os/templates.gd")
const ScriptMaker := preload("res://scripts/os/script_maker.gd")

var mono: Font
var machine: Object = null
var ship: Node = null            # the flight scene (the parts aboard, the log)

const BG := Color("#1e2228")
const PANEL := Color("#262b33")
const INK := Color("#d7dee6")
const DIM := Color("#8b95a1")
const KEYWORD := Color("#c792ea")
const CALL := Color("#82aaff")
const NUMBER := Color("#f78c6c")
const STRING := Color("#c3e88d")
const COMMENT := Color("#697180")
const SHIPC := Color("#ffcb6b")
const BAD := Color("#ff6b6b")
const GOOD := Color("#8fd18f")
const FS := 13

var edit: CodeEdit
var files: OptionButton
var status: Label
var watch: Tree
var out: RichTextLabel
var prompt: LineEdit
var _btn := {}

var path := ""
var _paths: Array = []
var _saved_text := ""
var _pid := -1               # the script this editor ran, while it runs
var _step := 0
var _go := 0
var _paused := false
var _last_vars := {}
var _exec_line := -1
var _err_line := -1
var _since := 0.0
var _seen_log := 0
var _history: Array[String] = []
var _hist_at := 0
var _block: Array[String] = []   # a console block being typed (after a line ending in :)


func _ready() -> void:
	focus_mode = Control.FOCUS_NONE
	if mono == null:
		mono = preload("res://scripts/os/uifont.gd").mono()
	_build()
	_list_files()
	if path == "" and str(machine.read_file("/root/scripts/first_script.py")) == "" and str(machine.read_file("/root/.editor_seen")) == "":
		# the first time: a gentle script to run and change
		machine.write_file("/root/scripts/first_script.py", Templates.FIRST)
		machine.write_file("/root/.editor_seen", "yes\n")
		path = "/root/scripts/first_script.py"
		_list_files()
	if path == "":
		path = "/root/scripts/my_script.py" if _paths.is_empty() else str(_paths[0])
	_open(path)


func take_focus() -> void:
	if edit:
		edit.grab_focus()


# ---- the window ---------------------------------------------------------

func _build() -> void:
	var bg := ColorRect.new()
	bg.color = PANEL
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(bg)
	var v := VBoxContainer.new()
	v.set_anchors_preset(Control.PRESET_FULL_RECT)
	v.add_theme_constant_override("separation", 3)
	add_child(v)
	# the bar: the file, and what to do with it
	var bar := HBoxContainer.new()
	v.add_child(bar)
	files = OptionButton.new()
	files.custom_minimum_size = Vector2(260, 0)
	files.item_selected.connect(func(i): _open(str(_paths[i])))
	bar.add_child(files)
	var nb := MenuButton.new()
	nb.text = "New"
	nb.focus_mode = Control.FOCUS_NONE
	nb.flat = false
	var pm := nb.get_popup()
	for i in Templates.LIST.size():
		pm.add_item(Templates.LIST[i][0], i)
	pm.add_item("From this ship's stages", 100)
	pm.add_item("To orbit, by your own logic (the example)", 101)
	pm.add_item("To Krill and down, by hand (the example)", 102)
	pm.id_pressed.connect(_new_from)
	bar.add_child(nb)
	for b in [["Save", _save], ["Run", _run], ["Pause", _pause], ["Step", _step_line], ["Stop", _stop]]:
		var bt := Button.new()
		bt.text = b[0]
		bt.focus_mode = Control.FOCUS_NONE
		bt.pressed.connect(b[1])
		bar.add_child(bt)
		_btn[b[0]] = bt
	status = Label.new()
	status.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	status.clip_text = true
	status.add_theme_color_override("font_color", DIM)
	bar.add_child(status)
	# the editor and the watch, side by side
	var split := HSplitContainer.new()
	split.size_flags_vertical = Control.SIZE_EXPAND_FILL
	v.add_child(split)
	edit = CodeEdit.new()
	edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	edit.custom_minimum_size = Vector2(380, 200)
	edit.add_theme_font_override("font", mono)
	edit.add_theme_font_size_override("font_size", FS)
	edit.add_theme_color_override("background_color", BG)
	edit.add_theme_color_override("font_color", INK)
	edit.add_theme_color_override("current_line_color", Color(1, 1, 1, 0.04))
	edit.add_theme_color_override("caret_color", Color.WHITE)
	edit.add_theme_color_override("executing_line_color", Color("#ffcb6b"))
	edit.gutters_draw_line_numbers = true
	edit.gutters_draw_breakpoints_gutter = true
	edit.gutters_draw_executing_lines = true
	edit.indent_use_spaces = true
	edit.indent_size = 4
	edit.indent_automatic = true
	edit.auto_brace_completion_enabled = true
	edit.code_completion_enabled = true
	edit.code_completion_prefixes = [".", "(", "\""]
	edit.syntax_highlighter = _highlighter()
	# Python's: # to the end of the line is a comment (so the ' in "we're"
	# opens no string), and strings end with their line
	edit.clear_comment_delimiters()
	edit.add_comment_delimiter("#", "", true)
	edit.clear_string_delimiters()
	edit.add_string_delimiter("\"", "\"", true)
	edit.add_string_delimiter("'", "'", true)
	edit.code_completion_requested.connect(_complete)
	edit.text_changed.connect(_on_typed)
	edit.breakpoint_toggled.connect(func(_l): _write_ctl())
	edit.gui_input.connect(_edit_keys)
	# a call's help on hovering its name
	if edit.has_method("set_tooltip_request_func"):
		edit.set_tooltip_request_func(_tooltip)
	split.add_child(edit)
	var right := VBoxContainer.new()
	right.custom_minimum_size = Vector2(230, 0)
	split.add_child(right)
	var wl := Label.new()
	wl.text = "WATCH  (the running script's variables)"
	wl.add_theme_color_override("font_color", SHIPC)
	wl.add_theme_font_size_override("font_size", 11)
	right.add_child(wl)
	watch = Tree.new()
	watch.columns = 2
	watch.hide_root = true
	watch.size_flags_vertical = Control.SIZE_EXPAND_FILL
	watch.add_theme_font_override("font", mono)
	watch.add_theme_font_size_override("font_size", 12)
	watch.set_column_expand(0, false)
	watch.set_column_custom_minimum_width(0, 100)
	watch.focus_mode = Control.FOCUS_NONE
	right.add_child(watch)
	# the console: what scripts say, and a prompt that runs Python now
	out = RichTextLabel.new()
	out.custom_minimum_size = Vector2(0, 120)
	out.scroll_following = true
	out.add_theme_font_override("normal_font", mono)
	out.add_theme_font_size_override("normal_font_size", 12)
	out.add_theme_color_override("default_color", INK)
	var sb := StyleBoxFlat.new()
	sb.bg_color = BG
	out.add_theme_stylebox_override("normal", sb)
	out.selection_enabled = true
	v.add_child(out)
	var pr := HBoxContainer.new()
	v.add_child(pr)
	var pl := Label.new()
	pl.text = ">>>"
	pl.add_theme_color_override("font_color", GOOD)
	pl.add_theme_font_override("font", mono)
	pr.add_child(pl)
	prompt = LineEdit.new()
	prompt.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	prompt.placeholder_text = "Python, run on the ship now: ship.orbit.top / km"
	prompt.add_theme_font_override("font", mono)
	prompt.text_submitted.connect(_console_line)
	prompt.gui_input.connect(_prompt_keys)
	pr.add_child(prompt)


func _highlighter() -> CodeHighlighter:
	var h := CodeHighlighter.new()
	h.number_color = NUMBER
	h.symbol_color = INK
	h.function_color = CALL
	h.member_variable_color = INK
	for k in PyLib.KEYWORDS:
		h.add_keyword_color(k, KEYWORD)
	for c in PyLib.CALLS:
		h.add_keyword_color(c[0], CALL)
	for n in PyLib.NAMES:
		h.add_keyword_color(n, SHIPC)
	h.add_color_region("#", "", COMMENT, true)
	h.add_color_region("\"", "\"", STRING, true)
	h.add_color_region("'", "'", STRING, true)
	return h


# ---- the files -----------------------------------------------------------

func _list_files() -> void:
	_paths.clear()
	files.clear()
	for dir in ["/root/scripts", "/root/examples"]:
		for n in str(machine.list_dir(dir)).split("\n", false):
			if n.ends_with(".py"):
				_paths.append(dir + "/" + n)
	if path != "" and not path in _paths:
		_paths.push_front(path)
	for p in _paths:
		files.add_item(str(p))
	var i := _paths.find(path)
	if i >= 0:
		files.select(i)


func _open(p: String) -> void:
	if edit.text != _saved_text and path != "":
		_save()
	path = p
	var t: String = machine.read_file(p)
	edit.text = t
	_saved_text = t
	edit.clear_undo_history()
	_err_line = -1
	_list_files()
	_say_status("%s  (%d lines)" % [p, edit.get_line_count()])


## "New": a script to start from, as a new file (never over one of yours).
func _new_from(id: int) -> void:
	var name := "my_script.py"
	var text := ""
	if id < Templates.LIST.size():
		name = Templates.LIST[id][1]
		text = Templates.LIST[id][2]
	elif id == 100:
		# the ship being flown, stage by stage (the stages still to fire)
		if ship == null or ship.get("computer") == null or (ship.computer._parts as Array).is_empty():
			_say_status("no ship to make a script from: fly one first", BAD)
			return
		var t: Dictionary = ship.session.telemetry()
		name = str(t.get("name", "my_ship")).to_snake_case().validate_filename() + ".py"
		text = ScriptMaker.make(ship.computer._parts, str(t.get("name", "my ship")), int(t.get("next_stage", 99)))
	elif id == 101 or id == 102:
		name = "to_orbit.py" if id == 101 else "krill_by_hand.py"
		text = str(machine.read_file("/root/examples/" + name))
	var p := _free_name(name)
	machine.write_file(p, text)
	_open(p)
	_say_status("new: %s (Run to try it)" % p)


func _free_name(name: String) -> String:
	var p := "/root/scripts/" + name
	var k := 2
	while str(machine.read_file(p)) != "":
		p = "/root/scripts/%s_%d.py" % [name.get_basename(), k]
		k += 1
	return p


func _save() -> void:
	if path.begins_with("/root/examples/"):
		# the examples stay as they came: a copy of one is yours to change
		path = "/root/scripts/" + path.get_file()
	machine.write_file(path, edit.text)
	_saved_text = edit.text
	if ship != null and ship.get("computer") != null:
		ship.computer.save()
	_list_files()
	_say_status("saved " + path)


# ---- running it, and watching ----------------------------------------------

func _run() -> void:
	_save()
	_stop()
	_paused = false
	_err_line = -1
	_clear_marks()
	var said: String = machine.sh_on(0, "ship run " + path)
	_print(said.strip_edges(), DIM)
	# found before it ran (a typo, a missing colon): mark that line
	var m := RegEx.create_from_string("py: line (\\d+): (.*)").search(said)
	if m:
		_err_line = int(m.get_string(1)) - 1
		if _err_line >= 0 and _err_line < edit.get_line_count():
			edit.set_line_background_color(_err_line, Color(1, 0.3, 0.3, 0.25))
			edit.set_caret_line(_err_line)
		_say_status("line %s: %s" % [m.get_string(1), m.get_string(2)], BAD)
	_pid = -1
	_btn["Pause"].text = "Pause"


func _stop() -> void:
	if _pid >= 0:
		machine.sh_on(0, "kill %d" % _pid)
		_print("stopped %s" % path, DIM)
	_pid = -1
	_exec(-1)


func _pause() -> void:
	if _pid < 0:
		return
	_paused = not _paused
	if not _paused:
		_go += 1      # (go on from wherever it stopped: a breakpoint, a step, a pause)
	_btn["Pause"].text = "Continue" if _paused else "Pause"
	_write_ctl()


func _step_line() -> void:
	if _pid < 0:
		return
	_paused = true
	_btn["Pause"].text = "Continue"
	_step += 1
	_write_ctl()


## What the script is told: its breakpoints (the editor's lines are 0-based,
## the script's 1-based), run or pause, and the step count.
func _write_ctl() -> void:
	if _pid < 0:
		return
	var lines := PackedStringArray()
	for l in edit.get_breakpointed_lines():
		lines.append(str(int(l) + 1))
	var t := "break %s\nmode %s\ngo %d\nstep %d\n" % [" ".join(lines), "pause" if _paused else "run", _go, _step]
	machine.write_file("/run/py/%d.ctl" % _pid, t)


## /run/py/<pid>.vars of the script running this file (or the console's).
func _find_vars(script: String) -> Dictionary:
	for n in str(machine.list_dir("/run/py")).split("\n", false):
		if not n.ends_with(".vars"):
			continue
		var t: String = machine.read_file("/run/py/" + n)
		if t.begins_with("script %s\n" % script):
			var d := _parse_vars(t)
			d["pid"] = int(n.trim_suffix(".vars"))
			return d
	return {}


static func _parse_vars(t: String) -> Dictionary:
	var d := {"vars": []}
	for line in t.split("\n", false):
		if line.begins_with("script "):
			d.script = line.substr(7)
		elif line.begins_with("line "):
			d.line = int(line.substr(5))
		elif line.begins_with("state "):
			d.state = line.substr(6)
		else:
			var tab := line.find("\t")
			if tab > 0:
				d.vars.append([line.substr(0, tab), line.substr(tab + 1)])
	return d


func _alive(pid: int) -> bool:
	for row in str(machine.sh_on(0, "ps")).split("\n", false):
		var f := row.strip_edges().split(" ", false)
		if f.size() >= 3 and f[0] == str(pid):
			return f[2] == "running"
	return false


func _process(dt: float) -> void:
	_since += dt
	_drain_log()
	if _since < 0.2:
		return
	_since = 0.0
	var d := _find_vars(path)
	if d.is_empty():
		return
	var running := _alive(int(d.pid))
	if running and _pid != int(d.pid):
		_pid = int(d.pid)
		_write_ctl()
	var state := str(d.get("state", ""))
	_show_watch(d)
	if running:
		_exec(int(d.get("line", 0)) - 1)
		_say_status("%s: line %d, %s" % [path.get_file(), int(d.get("line", 0)), state])
		if state.begins_with("paused") and not _paused:
			_paused = true   # (it stopped itself: a breakpoint, or a step's end)
			_btn["Pause"].text = "Continue"
			_write_ctl()
	else:
		if _pid == int(d.pid):
			_pid = -1
			_exec(-1)
		if state.begins_with("stopped"):
			_err_line = int(d.get("line", 0)) - 1
			if _err_line >= 0 and _err_line < edit.get_line_count():
				edit.set_line_background_color(_err_line, Color(1, 0.3, 0.3, 0.25))
			_say_status(state, BAD)
		else:
			_say_status("%s: %s" % [path.get_file(), state])


func _exec(line: int) -> void:
	if _exec_line == line:
		return
	if _exec_line >= 0 and _exec_line < edit.get_line_count():
		edit.set_line_as_executing(_exec_line, false)
		if _exec_line != _err_line:
			edit.set_line_background_color(_exec_line, Color(0, 0, 0, 0))
	_exec_line = line
	if line >= 0 and line < edit.get_line_count():
		edit.set_line_as_executing(line, true)
		edit.set_line_background_color(line, Color(1.0, 0.8, 0.3, 0.16))  # the line it's on


func _clear_marks() -> void:
	for i in edit.get_line_count():
		edit.set_line_background_color(i, Color(0, 0, 0, 0))


## The variables, the ones that changed since last look in yellow.
func _show_watch(d: Dictionary) -> void:
	watch.clear()
	var root := watch.create_item()
	var now := {}
	for kv in d.vars:
		var it := watch.create_item(root)
		it.set_text(0, kv[0])
		it.set_text(1, kv[1])
		it.set_tooltip_text(1, kv[1])
		it.set_custom_color(0, DIM)
		var changed: bool = _last_vars.has(kv[0]) and _last_vars[kv[0]] != kv[1]
		it.set_custom_color(1, SHIPC if changed else INK)
		now[kv[0]] = kv[1]
	_last_vars = now


func _say_status(t: String, c := DIM) -> void:
	status.text = t
	status.add_theme_color_override("font_color", c)


# ---- the console -----------------------------------------------------------

var _printed := 0

func _print(t: String, c := INK) -> void:
	if t == "":
		return
	out.push_color(c)
	out.add_text(t + "\n")
	out.pop()
	_printed += 1
	if _printed > 500:
		# (a chatty script: the last 400 lines are kept, or the window crawls)
		out.remove_paragraph(0)


## What the scripts and the console printed (the computer's log).
func _drain_log() -> void:
	if ship == null or ship.get("computer") == null:
		return
	var lg: Array = ship.computer.log
	var fresh := mini(int(ship.computer.log_count) - _seen_log, lg.size())
	for k in range(lg.size() - maxi(fresh, 0), lg.size()):
		var line := str(lg[k])
		if line.begins_with("> "):
			continue  # (the commands the ship was given: the Flight app shows those)
		_print(line, BAD if line.begins_with("py: ") or line.begins_with("error: ") or line.begins_with("syntax: ") else INK)
	_seen_log = int(ship.computer.log_count)


func _console_line(line: String) -> void:
	prompt.text = ""
	_console_up()
	if line.strip_edges() != "":
		_history.append(line)
		_hist_at = _history.size()
	# a block (if/for/def...) goes in whole: until a line with nothing on it
	if not _block.is_empty() or line.strip_edges().ends_with(":"):
		_block.append(line)
		_print("... " + line, DIM)
		if line.strip_edges() != "":
			prompt.text = "    " if line.strip_edges().ends_with(":") or line.begins_with(" ") else ""
			prompt.caret_column = prompt.text.length()
			return
		line = "\n".join(_block)
		_block.clear()
	else:
		_print(">>> " + line, GOOD)
	if line.strip_edges() == "":
		return
	var was: String = machine.read_file("/run/py/console.in")
	machine.write_file("/run/py/console.in", was + line + "\n\u001e\n")


## Ctrl+C: what the console is running stops (KeyboardInterrupt).
var _ints := 0
func _interrupt() -> void:
	var d := _find_vars("console")
	if d.is_empty():
		return
	_ints += 1
	machine.write_file("/run/py/%d.ctl" % int(d.pid), "interrupt %d\n" % _ints)
	_block.clear()
	_print("^C", DIM)


## The console's python, started the first time it's wanted.
func _console_up() -> void:
	var d := _find_vars("console")
	if not d.is_empty() and _alive(int(d.pid)):
		return
	machine.write_file("/run/py/console.in", "")
	var said: String = machine.sh_on(0, "ship run -i")
	for l in said.split("\n", false):
		if not l.begins_with("running "):
			_print(l, DIM)


func _prompt_keys(e: InputEvent) -> void:
	if e is InputEventKey and e.pressed and e.keycode == KEY_C and e.ctrl_pressed:
		_interrupt()
		prompt.accept_event()
		return
	if not (e is InputEventKey and e.pressed) or _history.is_empty():
		return
	if e.keycode == KEY_UP:
		_hist_at = maxi(_hist_at - 1, 0)
	elif e.keycode == KEY_DOWN:
		_hist_at = mini(_hist_at + 1, _history.size())
	else:
		return
	prompt.text = _history[_hist_at] if _hist_at < _history.size() else ""
	prompt.caret_column = prompt.text.length()
	prompt.accept_event()


## Ctrl+S saves, F5 runs, Shift+F5 stops, F10 steps (the keys an editor has).
func _edit_keys(e: InputEvent) -> void:
	if not (e is InputEventKey and e.pressed and not e.echo):
		return
	var k := e as InputEventKey
	if k.keycode == KEY_S and k.ctrl_pressed:
		_save()
	elif k.keycode == KEY_F5 and k.shift_pressed:
		_stop()
	elif k.keycode == KEY_F5:
		_run()
	elif k.keycode == KEY_F10:
		_step_line()
	else:
		return
	edit.accept_event()


## What a word under the mouse is: a call's arguments and what it does.
func _tooltip(word: String) -> String:
	for c in PyLib.CALLS:
		if c[0] == word:
			return "%s%s\n%s" % [c[0], c[1], c[2]]
	for m in PyLib.SHIP + PyLib.PART + PyLib.ORBIT:
		if m[0] == word:
			return "%s  %s" % [m[0], m[1]]
	return ""


# ---- suggestions -------------------------------------------------------------

func _on_typed() -> void:
	if _err_line >= 0:
		_clear_marks()
		_err_line = -1
	var line := edit.get_line(edit.get_caret_line()).substr(0, edit.get_caret_column())
	var word := _word_before(line)
	if word.length() >= 2 or line.ends_with(".") or line.ends_with("part(\"") or line.ends_with("parts(\""):
		edit.request_code_completion()


static func _word_before(line: String) -> String:
	var i := line.length()
	while i > 0 and (line[i - 1] == "_" or line[i - 1].is_valid_identifier() or line[i - 1].is_valid_int()):
		i -= 1
	return line.substr(i)


## What fits where the caret is: a part's name inside ship.part("; after a
## dot what that thing has (the ship's, its orbit's, its body's, a part's,
## from the ship as it is now); else every call, keyword and name the
## script has.
func _complete() -> void:
	var line := edit.get_line(edit.get_caret_line()).substr(0, edit.get_caret_column())
	var word := _word_before(line)
	var before := line.substr(0, line.length() - word.length())
	if before.ends_with("part(\"") or before.ends_with("parts(\""):
		for nm in _part_names():
			if word == "" or str(nm).to_lower().begins_with(word.to_lower()):
				edit.add_code_completion_option(CodeEdit.KIND_PLAIN_TEXT, nm, nm, SHIPC)  # (inside a string: CodeEdit quotes it)
	elif before.ends_with("."):
		var owner := _word_before(before.substr(0, before.length() - 1))
		var chain := _chain_before(before.substr(0, before.length() - 1))
		for m in _members(owner, chain):
			if word == "" or str(m[0]).begins_with(word):
				var call: bool = str(m[1]).begins_with("(")
				edit.add_code_completion_option(CodeEdit.KIND_MEMBER if not call else CodeEdit.KIND_FUNCTION,
					"%s  %s" % [m[0], m[1]], m[0] + ("()" if call and str(m[1]).begins_with("()") else ("(" if call else "")), SHIPC)
	else:
		if word == "":
			return
		for c in PyLib.CALLS:
			if str(c[0]).begins_with(word):
				edit.add_code_completion_option(CodeEdit.KIND_FUNCTION, "%s%s  %s" % [c[0], c[1], c[2]], c[0] + "(", CALL)
		for k in PyLib.KEYWORDS + PyLib.NAMES:
			if str(k).begins_with(word):
				edit.add_code_completion_option(CodeEdit.KIND_PLAIN_TEXT, k, k, KEYWORD)
		for n in _script_names():
			if str(n).begins_with(word) and n != word:
				edit.add_code_completion_option(CodeEdit.KIND_VARIABLE, n, n, INK)
	edit.update_code_completion_options(true)


## "ship.orbit" before the dot in "ship.orbit.", as its parts.
static func _chain_before(text: String) -> Array:
	var i := text.length()
	while i > 0 and (text[i - 1] == "_" or text[i - 1] == "." or text[i - 1].is_valid_identifier() or text[i - 1].is_valid_int()):
		i -= 1
	return text.substr(i).split(".", false)


func _members(owner: String, chain: Array) -> Array:
	if chain.size() == 1 and owner == "ship":
		return PyLib.SHIP
	if chain.size() == 2 and chain[0] == "ship" and chain[1] == "orbit":
		return PyLib.ORBIT
	if chain.size() == 2 and chain[0] == "ship" and chain[1] == "body":
		return PyLib.BODY
	# a name set to a part (engine = ship.part("Walrus")): what that kind has
	var kind := _kind_of(owner)
	if kind != "":
		var out := []
		for m in PyLib.PART:
			if m[0] in _part_keys(kind):
				out.append(m)
		return out
	return PyLib.PART


## The kind of part a script's name holds, from the line that set it.
func _kind_of(name: String) -> String:
	var re := RegEx.create_from_string("(?m)^\\s*" + name + "\\s*=\\s*ship\\.part\\(\"([^\"]+)\"\\)")
	var m := re.search(edit.text)
	if m == null:
		var re2 := RegEx.create_from_string("(?m)^\\s*" + name + "\\s*=\\s*ship\\.(engines|boosters|tanks|decouplers|parachutes|legs|pods)\\[")
		var m2 := re2.search(edit.text)
		if m2 == null:
			return ""
		return {"engines": "engine", "boosters": "booster", "tanks": "tank", "decouplers": "decoupler",
			"parachutes": "parachute", "legs": "leg", "pods": "pod"}[m2.get_string(1)]
	var want := m.get_string(1).to_lower()
	if ship != null and ship.get("computer") != null:
		for p in ship.computer._parts:
			var nm := str(ship.computer._names.get(int(p.uid), p.name)).to_lower()
			if nm == want or nm.begins_with(want + " "):
				return str(p.kind)
	return ""


static func _part_keys(kind: String) -> Array:
	var all := ["name", "kind", "stage", "temp", "heat"]
	match kind:
		"engine": all += ["throttle", "fuel", "fuel_kg", "lit", "thrust", "max_thrust", "isp", "ignite", "shutdown"]
		"booster": all += ["fuel", "fuel_kg", "lit", "thrust", "max_thrust", "isp", "ignite"]
		"tank": all += ["fuel", "fuel_kg", "capacity_kg"]
		"decoupler": all += ["decouple", "fired"]
		"parachute": all += ["deploy", "cut", "state"]
		"leg": all += ["deploy", "retract", "deployed"]
	return all


func _part_names() -> Array:
	var out := []
	if ship != null and ship.get("computer") != null:
		for uid in ship.computer._names:
			out.append(str(ship.computer._names[uid]))
	out.sort()
	return out


## The names this script makes (x = ..., def f, for x in).
func _script_names() -> Array:
	var seen := {}
	var re := RegEx.create_from_string("(?m)^\\s*(?:def\\s+|for\\s+)?([A-Za-z_][A-Za-z0-9_]*)\\s*(?:=[^=]|\\(|\\s+in\\b)")
	for m in re.search_all(edit.text):
		seen[m.get_string(1)] = true
	for k in PyLib.KEYWORDS:
		seen.erase(k)
	return seen.keys()
