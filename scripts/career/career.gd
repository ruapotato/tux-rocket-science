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
## The space program's progress: research and funds, the R&D tree, PASA's
## missions, and the experiment results riding aboard craft (by the uid of
## the part holding them, as crew seats are). Physics stays in trs_core;
## this is bookkeeping over its telemetry. Every number comes from
## data/career/*.json and the part catalog. In a sandbox game everything is
## unlocked and free, but research and missions still count.
extends Node

const SCIENCE_PATH := "res://data/career/science.json"
const TECH_PATH := "res://data/career/tech.json"
const MISSIONS_PATH := "res://data/career/missions.json"

signal changed  # funds, research, tech or missions moved
signal mission_done(m: Dictionary)
signal survey_done(s: Dictionary)
signal relic_heard(r: Dictionary)  # a Signal Scanner picked up a relic for the first time
signal landmark_sighted(lm: Dictionary)  # a landmark seen for the first time (lm has its name, text, research)
signal landmark_over(lm: Dictionary)  # over (or at) a landmark seen before, again

var science: Dictionary = {}
var tech: Array = []
var missions_def: Dictionary = {}

var mode := "sandbox"  # or "career"
var funds := 0.0
var research := 0.0
var unlocked: Array = []      # tech node ids
var done: Array = []          # mission ids
var credited: Dictionary = {} # subject -> results brought home
var aboard: Dictionary = {}   # part uid (string) -> [{subject, exp, body, situation}]
var visited: Dictionary = {}  # vessel serial (string) -> [bodies landed on]
var surveys: Array = []       # standing contracts: [{subject, exp, body, situation, funds, research}]
var surveys_done := 0
var heard: Array = []         # relic ids a Signal Scanner has heard (their place is known)
var sighted: Array = []       # "body/landmark" ids seen (close enough to see from)
var _over := ""               # the landmark below the ship now
const LANDMARK_RESEARCH := 4.0  # what seeing one for the first time teaches
var path := ""

## how close (m along the ground) a penguin on foot must come to a relic
const INVESTIGATE_M := 25.0


var _body_names := {}
var _biome_names := {}  # body id -> {biome id: name}


## A flag went up on `body`: missions that want one there are done.
func flag_planted(body: String) -> void:
	for m in active_missions():
		if m.goal.kind == "flag" and m.goal.body == body:
			complete(m)


func _init() -> void:
	for b in Sim.system_bodies():
		_body_names[b.id] = b.name
		var bn := {}
		for e in b.get("biomes", []):
			bn[e.id] = e.name
		_biome_names[b.id] = bn
	science = JSON.parse_string(FileAccess.get_file_as_string(SCIENCE_PATH))
	tech = JSON.parse_string(FileAccess.get_file_as_string(TECH_PATH)).nodes
	missions_def = JSON.parse_string(FileAccess.get_file_as_string(MISSIONS_PATH))


# ---- save --------------------------------------------------------------------

## Loads the program's state from `file` (a new program if there is none).
func load_from(file: String, new_mode := "sandbox") -> void:
	path = file
	var text := FileAccess.get_file_as_string(file) if FileAccess.file_exists(file) else ""
	var d = JSON.parse_string(text) if text != "" else null
	if not d is Dictionary:
		d = {"mode": new_mode, "funds": float(missions_def.start_funds), "research": 0.0, "unlocked": ["basics"]}
	mode = d.get("mode", new_mode)
	funds = float(d.get("funds", 0.0))
	research = float(d.get("research", 0.0))
	unlocked = d.get("unlocked", ["basics"])
	done = d.get("done", [])
	credited = d.get("credited", {})
	aboard = d.get("aboard", {})
	visited = d.get("visited", {})
	surveys = d.get("surveys", [])
	surveys_done = int(d.get("surveys_done", 0))
	heard = d.get("heard", [])
	sighted = d.get("sighted", [])
	refresh_surveys()
	changed.emit()


func save() -> void:
	if path == "":
		return
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f:
		f.store_string(JSON.stringify(state(), "  "))


func state() -> Dictionary:
	return {"version": 1, "mode": mode, "funds": funds, "research": research, "unlocked": unlocked, "done": done,
		"credited": credited, "aboard": aboard, "visited": visited, "surveys": surveys, "surveys_done": surveys_done, "heard": heard,
		"sighted": sighted}


## Puts the program back as `s` (a revert undoes the flight's results).
func restore(s: Dictionary) -> void:
	mode = s.mode
	funds = s.funds
	research = s.research
	unlocked = s.unlocked.duplicate()
	done = s.done.duplicate()
	credited = s.credited.duplicate(true)
	aboard = s.aboard.duplicate(true)
	visited = s.visited.duplicate(true)
	surveys = s.get("surveys", []).duplicate(true)
	surveys_done = int(s.get("surveys_done", 0))
	heard = s.get("heard", []).duplicate()
	sighted = s.get("sighted", []).duplicate()
	save()
	changed.emit()


func is_career() -> bool:
	return mode == "career"


# ---- parts, costs and R&D ------------------------------------------------------

func part_cost(id: String) -> float:
	return float(Sim.part(id).get("cost", 0))


## The tech node that unlocks part `id` ("" if none: always available).
func node_of_part(id: String) -> String:
	for n in tech:
		if id in n.parts:
			return n.id
	return ""


func part_unlocked(id: String) -> bool:
	if not is_career():
		return true
	var n := node_of_part(id)
	return n == "" or n in unlocked


## Whether the program can launch from `site`: the pad and the runway
## always, the Orbital Yard once a node opening it is researched.
func site_unlocked(site: String) -> bool:
	if not is_career() or site in ["pad", "runway"]:
		return true
	for n in tech:
		if n.get("site", "") == site and n.id in unlocked:
			return true
	return false


## Whether the ship's computer has `feature` ("planner": when_to_leave and
## what_if; "guidance": the programs that fly themselves): always in the
## sandbox, once researched in a career. The language, the parts, aiming,
## burning and the orbit's maths are the computer's from the start.
func computer_unlocked(feature: String) -> bool:
	if not is_career():
		return true
	for n in tech:
		if n.get("computer", "") == feature and n.id in unlocked:
			return true
	return false


## Why not, in words, for the script that asked ("" if it may).
func computer_refusal(feature: String) -> String:
	if computer_unlocked(feature):
		return ""
	for n in tech:
		if n.get("computer", "") == feature:
			return "the ship's computer needs the %s for that (R&D, %d research)" % [n.name, int(n.get("cost", 0))]
	return ""


func tech_node(id: String) -> Dictionary:
	for n in tech:
		if n.id == id:
			return n
	return {}


## Why node `id` can't be researched now ("" if it can).
func tech_refusal(id: String) -> String:
	var n := tech_node(id)
	if n.is_empty():
		return "no such node"
	if id in unlocked:
		return "already researched"
	if n.has("found_by"):
		return "found, not researched: %s" % mission(n.found_by).get("title", n.found_by)
	for r in n.requires:
		if not r in unlocked:
			return "needs %s first" % tech_node(r).get("name", r)
	if research < float(n.get("cost", 0)):
		return "needs %d research (have %d)" % [int(n.get("cost", 0)), int(research)]
	return ""


func unlock(id: String) -> String:
	var why := tech_refusal(id)
	if why != "":
		return why
	research -= float(tech_node(id).get("cost", 0))
	unlocked.append(id)
	refresh_surveys()
	save()
	changed.emit()
	return ""


## The part ids of saved craft text.
static func craft_parts(text: String) -> Array:
	var d = JSON.parse_string(text)
	if not d is Dictionary:
		return []
	return (d.get("parts", []) as Array).map(func(p): return str(p.part))


## Why craft text can't be launched in this program now ("" if it can).
func launch_refusal(text: String) -> String:
	if not is_career():
		return ""
	var ids := craft_parts(text)
	var locked := locked_parts(ids)
	if not locked.is_empty():
		return "%s isn't researched yet (R&D)" % Sim.part(locked[0]).get("name", locked[0])
	if craft_cost(ids) > funds:
		return "it costs %s and PASA has %s" % [money(craft_cost(ids)), money(funds)]
	return ""


## What a craft costs to build: the sum of its parts' costs.
func craft_cost(part_ids: Array) -> float:
	var c := 0.0
	for p in part_ids:
		c += part_cost(p)
	return c


## Parts of a craft that R&D hasn't unlocked yet.
func locked_parts(part_ids: Array) -> Array:
	var out := []
	for p in part_ids:
		if not part_unlocked(p) and not p in out:
			out.append(p)
	return out


## Pays for a launch; "" or why not (career only; a sandbox launch is free).
func charge_launch(part_ids: Array) -> String:
	if not is_career():
		return ""
	var locked := locked_parts(part_ids)
	if not locked.is_empty():
		return "%s isn't researched yet" % Sim.part(locked[0]).get("name", locked[0])
	var c := craft_cost(part_ids)
	if c > funds:
		return "costs %s, you have %s" % [money(c), money(funds)]
	funds -= c
	save()
	changed.emit()
	return ""


static func money(x: float) -> String:
	var s := str(int(roundf(x)))
	var out := ""
	while s.length() > 3:
		out = "," + s.right(3) + out
		s = s.left(s.length() - 3)
	return "ƒ" + s + out


# ---- science ---------------------------------------------------------------------

## Where telemetry `t` puts a craft for science: landed, flying_low,
## flying_high, space_low or space_high ("" if nowhere that counts).
func situation(t: Dictionary) -> String:
	if not t.get("alive", false):
		return ""
	var b: Dictionary = science.bodies.get(t.body, {})
	if b.is_empty():
		return ""
	if t.landed:
		return "landed"
	if t.in_atmosphere:
		return "flying_low" if float(t.altitude) < float(b.get("flying_high_alt", 0.0)) else "flying_high"
	return "space_low" if float(t.altitude) < float(b.get("space_high_alt", 1.0e30)) else "space_high"


## The biome a result in situation `sit` belongs to ("" if that
## situation is counted over the whole body).
func biome(t: Dictionary, sit: String) -> String:
	if sit == "" or not science.situations[sit].get("biome", false):
		return ""
	return str(t.get("biome", ""))


static func subject(exp: String, body: String, sit: String, bio := "") -> String:
	return "%s@%s:%s%s" % [exp, body, sit, ("/" + bio) if bio != "" else ""]


## A result's worth if brought home now: base x body x situation, halved
## for each earlier result of the same subject already home.
func value(exp: String, body: String, sit: String, bio := "") -> float:
	var e: Dictionary = science.experiments.get(exp, {})
	var b: Dictionary = science.bodies.get(body, {})
	var s: Dictionary = science.situations.get(sit, {})
	if e.is_empty() or b.is_empty() or s.is_empty() or not sit in e.situations:
		return 0.0
	var n := int(credited.get(subject(exp, body, sit, bio), 0))
	return float(e.base) * float(b.value) * float(s.value) * pow(float(science.repeat_factor), n)


## Worth of a result record ({exp, body, situation, biome}) brought home now.
func value_of(r: Dictionary) -> float:
	return value(r.exp, r.body, r.situation, r.get("biome", ""))


func describe(sit: String, body: String, bio := "") -> String:
	if sit == "":
		return "nowhere to study"
	var out := "%s %s" % [science.situations[sit].phrase, body_name(body)]
	if bio != "":
		out += " (%s)" % biome_name(body, bio)
	return out


func biome_name(body: String, bio: String) -> String:
	return _biome_names.get(body, {}).get(bio, bio.capitalize())


func body_biomes(body: String) -> Array:
	return _biome_names.get(body, {}).keys()


func body_name(id: String) -> String:
	return _body_names.get(id, id.capitalize())


## The experiments vessel `id` can run: [{uid, part, exp, name, value, why}]
## (why: "" if it can run here and now).
func experiments_on(id: int, t: Dictionary) -> Array:
	var out := []
	var sit := situation(t)
	var bio := biome(t, sit)
	var have := results_subjects(id)
	for p in JSON.parse_string(Sim.game.core.vessel_parts(id)) if Sim.game else []:
		for exp in Sim.part(p.part).get("science", []):
			var e: Dictionary = science.experiments.get(exp, {})
			var why := ""
			var v := 0.0
			if sit == "":
				why = "nothing to study here"
			elif not sit in e.situations:
				why = "not while %s" % science.situations[sit].name.to_lower()
			else:
				v = value(exp, t.body, sit, bio)
				if subject(exp, t.body, sit, bio) in have:
					why = "already have this one aboard"
			out.append({"uid": str(int(p.uid)), "part": p.part, "exp": exp, "name": e.get("name", exp), "value": v, "why": why})
	return out


## Runs experiment `exp` on part `uid` of vessel `id`: "" or why not.
func run(id: int, uid: String, exp: String, t: Dictionary) -> String:
	for x in experiments_on(id, t):
		if x.uid == uid and x.exp == exp:
			if x.why != "":
				return x.why
			var list: Array = aboard.get(uid, [])
			var sit := situation(t)
			var bio := biome(t, sit)
			list.append({"subject": subject(exp, t.body, sit, bio), "exp": exp, "body": t.body, "situation": sit, "biome": bio})
			aboard[uid] = list
			save()
			return ""
	return "no such experiment aboard"


## Every result aboard vessel `id`.
func results_on(id: int) -> Array:
	var out := []
	if Sim.game == null:
		return out
	for p in JSON.parse_string(Sim.game.core.vessel_parts(id)):
		out.append_array(aboard.get(str(int(p.uid)), []))
	return out


func results_subjects(id: int) -> Array:
	return results_on(id).map(func(r): return r.subject)


## Sends result `subject` aboard vessel `id` home by antenna: it counts at
## the antenna's fraction of its worth, for kJ per point received from the
## batteries. Returns {ok, research, kj, why}.
func transmit(id: int, subject_id: String, t: Dictionary) -> Dictionary:
	var frac: float = t.get("antenna_fraction", 0.0)
	if frac <= 0.0:
		return {"ok": false, "why": "no antenna aboard"}
	for p in JSON.parse_string(Sim.game.core.vessel_parts(id)):
		var uid := str(int(p.uid))
		var list: Array = aboard.get(uid, [])
		for k in list.size():
			var r: Dictionary = list[k]
			if r.subject != subject_id:
				continue
			var pts := value_of(r) * frac
			var kj: float = pts * float(t.get("antenna_kj_per_point", 0.0))
			if not Sim.game.core.vessel_use_ec(id, kj):
				return {"ok": false, "why": "needs %.0f kJ, the batteries hold %.0f" % [kj, t.get("ec", 0.0)]}
			research += pts
			credited[r.subject] = int(credited.get(r.subject, 0)) + 1
			list.remove_at(k)
			_survey_check(r.subject)
			if list.is_empty():
				aboard.erase(uid)
			save()
			changed.emit()
			return {"ok": true, "research": pts, "kj": kj, "why": ""}
	return {"ok": false, "why": "no such result aboard"}


## Moves the results held by part `from_uid` to part `to_uid` (a penguin
## climbing back in hands its samples over).
func move_results(from_uid: String, to_uid: String) -> void:
	if aboard.has(from_uid):
		var to: Array = aboard.get(to_uid, [])
		to.append_array(aboard[from_uid])
		aboard[to_uid] = to
		aboard.erase(from_uid)
		save()


## Results on parts that no longer exist are lost with them.
func sync(alive_uids: Dictionary) -> void:
	var gone := false
	for uid in aboard.keys():
		if not alive_uids.has(uid):
			aboard.erase(uid)
			gone = true
	if gone:
		save()


## Vessel `id` is recovered at Floe: its results count (each at its worth
## now) and, in a career, most of its parts' cost comes back. Returns
## {research, funds, results}.
func recover(id: int, serial: int) -> Dictionary:
	var got := 0.0
	var n := 0
	var parts: Array = JSON.parse_string(Sim.game.core.vessel_parts(id))
	for p in parts:
		var uid := str(int(p.uid))
		for r in aboard.get(uid, []):
			got += value_of(r)
			credited[r.subject] = int(credited.get(r.subject, 0)) + 1
			n += 1
			_survey_check(r.subject)
		aboard.erase(uid)
	research += got
	var refund := 0.0
	if is_career():
		refund = craft_cost(parts.map(func(p): return p.part)) * float(missions_def.recovery_refund)
		funds += refund
	var came_from: Array = visited.get(str(serial), [])
	visited.erase(str(serial))
	for m in active_missions():
		if m.goal.kind == "recover_from" and m.goal.body in came_from:
			complete(m)
	save()
	changed.emit()
	return {"research": got, "funds": refund, "results": n}


# ---- landmarks ----------------------------------------------------------------------

## The landmark below t's ship (close enough to see it whole: within its
## own size, and more the higher up), or {}.
static func landmark_below(t: Dictionary) -> Dictionary:
	var R := Sim.radius(str(t.get("body", "")))
	if R <= 0.0:
		return {}
	var la := deg_to_rad(float(t.get("lat_deg", 0.0)))
	var lo := deg_to_rad(float(t.get("lon_deg", 0.0)))
	var up := minf(maxf(float(t.get("altitude_terrain", 0.0)), 0.0), 80000.0)
	var best := {}
	var best_d := INF
	for lm in Sim.landmarks(str(t.body)):
		var la2 := deg_to_rad(float(lm.lat_deg))
		var lo2 := deg_to_rad(float(lm.lon_deg))
		var c := sin(la) * sin(la2) + cos(la) * cos(la2) * cos(lo - lo2)
		var d := acos(clampf(c, -1.0, 1.0)) * R
		var size := float(lm.get("length_km", 0.0)) * 500.0 if lm.get("kind", "") == "canyon" else float(lm.get("radius_km", 10.0)) * 1000.0
		if d < size * 1.2 + up and d < best_d:
			best = lm
			best_d = d
	return best


func _landmark_check(t: Dictionary) -> void:
	var lm := landmark_below(t)
	var key := "" if lm.is_empty() else "%s/%s" % [t.body, lm.id]
	if key == _over:
		return
	_over = key
	if key == "":
		return
	if not key in sighted:
		sighted.append(key)
		research += LANDMARK_RESEARCH
		save()
		changed.emit()
		var e := lm.duplicate()
		e["research"] = LANDMARK_RESEARCH
		landmark_sighted.emit(e)
	else:
		landmark_over.emit(lm)


# ---- relics -------------------------------------------------------------------------

## Relic `id` as telemetry `t` sees it ({} if it isn't on t's body).
static func relic_in(t: Dictionary, id: String) -> Dictionary:
	for r in t.get("relics", []):
		if r.id == id:
			return r
	return {}


## The relics on t's body whose place PASA knows, as telemetry sees them.
func known_relics(t: Dictionary) -> Array:
	return (t.get("relics", []) as Array).filter(func(r): return r.id in heard)


# ---- surveys -----------------------------------------------------------------------

const SURVEYS := 3
const SURVEY_FUNDS_PER_POINT := 1500.0
const SURVEY_RESEARCH_SHARE := 0.5
## a world joins the survey pool once its mission is done
const SURVEY_BODIES := {"floe": "", "krill": "orbit", "kelp": "krill_soi", "pebble": "kelp_soi", "narwhal": "narwhal_soi", "tusk": "narwhal_soi"}


## Experiments the program can fly now (every part of it researched).
func _flyable_experiments() -> Array:
	var out := []
	for id in Sim.catalog_order:
		if part_unlocked(id):
			for exp in Sim.part(id).get("science", []):
				if not exp in out:
					out.append(exp)
	return out


## Keeps SURVEYS standing contracts: results not yet brought home, from
## worlds the program has reached, by experiments it can fly. Each pays
## SURVEY_FUNDS_PER_POINT funds per research point of the result (and
## SURVEY_RESEARCH_SHARE of it again as research) when it comes home.
func refresh_surveys() -> void:
	var have := surveys.map(func(s): return s.subject)
	var pool := []
	for body in SURVEY_BODIES:
		var need: String = SURVEY_BODIES[body]
		if need != "" and not need in done:
			continue
		for exp in _flyable_experiments():
			for sit in science.experiments[exp].situations:
				if body in ["krill", "pebble", "tusk"] and sit.begins_with("flying"):
					continue  # no air
				var bios := body_biomes(body) if science.situations[sit].get("biome", false) else []
				for bio in bios if not bios.is_empty() else [""]:
					var subj := subject(exp, body, sit, bio)
					if int(credited.get(subj, 0)) == 0 and not subj in have:
						pool.append([exp, body, sit, bio])
	var rng := RandomNumberGenerator.new()
	rng.seed = 7919 * (surveys_done + 1) + surveys.size()
	while surveys.size() < SURVEYS and not pool.is_empty():
		var pick: Array = pool.pop_at(rng.randi() % pool.size())
		var v := value(pick[0], pick[1], pick[2], pick[3])
		surveys.append({"subject": subject(pick[0], pick[1], pick[2], pick[3]), "exp": pick[0], "body": pick[1], "situation": pick[2], "biome": pick[3],
			"funds": roundf(v * SURVEY_FUNDS_PER_POINT / 100.0) * 100.0, "research": snappedf(v * SURVEY_RESEARCH_SHARE, 0.1)})


func describe_survey(s: Dictionary) -> String:
	var where := describe(s.situation, s.body, s.get("biome", ""))
	return "%s %s" % [science.experiments[s.exp].name, where.left(1).to_lower() + where.substr(1)]


## A result of `subj` came home: a survey that wanted it pays out.
func _survey_check(subj: String) -> void:
	for k in surveys.size():
		var s: Dictionary = surveys[k]
		if s.subject == subj:
			research += float(s.research)
			if is_career():
				funds += float(s.funds)
			surveys.remove_at(k)
			surveys_done += 1
			survey_done.emit(s)
			refresh_surveys()
			return


# ---- missions ----------------------------------------------------------------------

func mission(id: String) -> Dictionary:
	for m in missions_def.missions:
		if m.id == id:
			return m
	return {}


## Missions on offer now: not done, and every mission they follow done.
func active_missions() -> Array:
	var out := []
	for m in missions_def.missions:
		if m.id in done:
			continue
		var ok := true
		for a in m.after:
			ok = ok and a in done
		if ok:
			out.append(m)
	return out


func complete(m: Dictionary) -> void:
	if m.id in done:
		return
	done.append(m.id)
	for n in tech:
		if n.get("found_by", "") == m.id and not n.id in unlocked:
			unlocked.append(n.id)  # what the relic taught is PASA's now
	refresh_surveys.call_deferred()  # new worlds may join the pool
	research += float(m.research)
	if is_career():
		funds += float(m.funds)
	save()
	changed.emit()
	mission_done.emit(m)


## Checks the active missions against vessel telemetry `t` (the craft being
## flown); `extra` carries {eva, target_distance, target_orbiting, docked}.
## Also notes where each vessel has landed, for "recover_from".
var _hover_t0 := -1.0   # when a hover goal's band was entered (-1: not in it)

func check(t: Dictionary, serial: int, extra := {}) -> void:
	if not t.get("alive", false):
		return
	if t.landed:
		var v: Array = visited.get(str(serial), [])
		if not t.body in v:
			v.append(t.body)
			visited[str(serial)] = v
			save()
	_landmark_check(t)
	for r in t.get("relics", []):
		if r.heard and not r.id in heard:
			heard.append(r.id)
			save()
			relic_heard.emit(r)
	var atm := float(t.get("atmosphere_height", 0.0))
	for m in active_missions():
		var g: Dictionary = m.goal
		var hit := false
		# the computer flying: nobody at the flying keys since liftoff
		if g.get("hands_off", false) and not extra.get("hands_off", false):
			if g.kind == "hover":
				_hover_t0 = -1.0
			continue
		match g.kind:
			"hover":
				var off := absf(float(t.get("altitude_terrain", 0.0)) - float(g.value))
				if off <= float(g.get("band", 2.0)) and not t.landed:
					if _hover_t0 < 0.0:
						_hover_t0 = float(t.t)
					hit = float(t.t) - _hover_t0 >= float(g.get("seconds", 20.0))
				else:
					_hover_t0 = -1.0
			"altitude":
				hit = t.body == g.body and float(t.altitude) >= float(g.value)
			"space":
				hit = t.body == g.body and not t.in_atmosphere and float(t.altitude) > atm
			"orbit":
				var pe = t.get("orbit", {}).get("pe_alt", null)
				hit = t.body == g.body and pe != null and float(pe) > atm and not t.landed
			"soi":
				hit = t.body == g.body
			"landed":
				hit = t.body == g.body and t.landed and not extra.get("eva", false)
			"eva_space":
				hit = extra.get("eva", false) and not t.in_atmosphere and not t.landed
			"eva_landed":
				hit = extra.get("eva", false) and t.body == g.body and t.landed
			"rendezvous":
				hit = float(extra.get("target_distance", INF)) <= float(g.value) and situation(t).begins_with("space")
			"docked":
				hit = extra.get("docked", false)
			"refine":
				hit = t.body != "floe" and float(t.get("refine_rate", 0.0)) > 0.0
			"hear":
				hit = g.relic in heard
			"investigate":
				var r := relic_in(t, g.relic)
				hit = extra.get("eva", false) and t.landed and not r.is_empty() and float(r.surface_distance) <= INVESTIGATE_M
		if hit:
			complete(m)

