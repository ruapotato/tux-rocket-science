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
## --test-by-hand: a script a player writes, with the logic their own: the
## parts by name (engine.throttle = 100, drop.decouple()), the maths asked
## for (speed_needed, speed_at, burn_time) and the burn flown to it. The
## Orbiter off the pad into orbit that way; then the same script with each
## stage left to the pilot's space (wait_user_trigger), the presses given
## here as a pilot would.
extends RefCounted

var _log := PackedStringArray()
var _failed := 0
var _t_update := 0
var _t_step := 0
var _u_last := 0

const TO_ORBIT := """# to_orbit.py -- the pad to orbit, by your own logic
countdown(3)

lifter = ship.part("Walrus")                    # the first stage's engine
upper = ship.part("Albatross")                  # the second's
drop = ship.part("Crack-1 Stack Decoupler 1")   # between them

def lean_for(height):
    # straight up for the first km, then over: fast at first, slower as the
    # air thins (the square root), flat by 50 km
    if height < 1000:
        return 90
    return max(90 - 90 * sqrt((height - 1000) / 49000), 0)

TRIGGER1
lifter.throttle = 100           # lights it, and the clamps let go
staged = False
while ship.orbit.top < 100 * km:
    aim_at(lean_for(ship.altitude), 90)
    chart("height", ship.altitude)
    if not staged and lifter.fuel < 0.5:     # (once it's gone, it has no fuel to read)
        print(f"first stage dry at {ship.altitude / km:.1f} km: dropping it")
        lifter.throttle = 0
        TRIGGER2
        drop.decouple()
        upper.throttle = 100
        staged = True

for e in ship.engines:
    e.throttle = 0
print("Coasting to the top of the climb.")

def round_off():
    # the speed a circle needs at the top, less what we'll have there
    top = ship.orbit.top
    needed = speed_needed(top)
    have = speed_at(top)
    extra = needed - have
    print(f"A circle needs {needed:.0f} m/s up there. We'll be doing {have:.0f}, so {extra:.0f} extra.")
    # half the burn before the top, half after; level with the horizon,
    # since the speed we're adding is sideways speed
    sleep(ship.orbit.time_to_top - burn_time(extra) / 2)
    aim_at(0, 90)
    burn(extra)

# a long burn never comes out exact: look, and go again at the new top
tries = 0
while ship.orbit.bottom < 70 * km and tries < 3:
    round_off()
    tries += 1
    print(f"after burn {tries}: {ship.orbit.bottom / km:.1f} x {ship.orbit.top / km:.1f} km")
print(f"In orbit: {ship.orbit.bottom / km:.1f} x {ship.orbit.top / km:.1f} km")
"""

const KRILL := """# krill_by_hand.py -- up by the guidance computer, then to Krill and down by you
launch(80000)

trip = when_to_leave("krill")
print(f"leave in {trip.wait:.0f} s: {trip.dv:.0f} m/s, {trip.travel / 3600:.1f} h out, passing {trip.closest / km:.0f} km up")
if trip.dv > ship.delta_v:
    print("not enough fuel for Krill")
    exit(1)
sleep(trip.wait - burn_time(trip.dv) / 2)
aim("prograde")
burn(trip.dv)

# a long burn is never exact: try pushes for free with what_if, keep the best
goal = 30 * km
def miss(p):
    if p.meets != "Krill":
        return 1000000000
    return abs(p.closest - goal)

def find_fix(at):
    # step along each way while it gets closer, halving the step when
    # nothing does: [forward, sideways, outward, how far off]
    best = [0, 0, 0]
    here = miss(what_if(0, 0, 0, at))
    step = 8
    while step > 0.1 and here > 1 * km:
        better = False
        for d in [[step, 0, 0], [-step, 0, 0], [0, step, 0], [0, -step, 0], [0, 0, step], [0, 0, -step]]:
            t = [best[0] + d[0], best[1] + d[1], best[2] + d[2]]
            m = miss(what_if(t[0], t[1], t[2], at))
            if m < here:
                here = m
                best = t
                better = True
        if not better:
            step = step / 2
    return [best[0], best[1], best[2], here]

# look, fix, and look again: the closer the fix, the smaller the next one
tries = 0
off = miss(what_if(0, 0, 0, ship.time + 600))
while off > 5 * km and tries < 3:
    tries += 1
    at = ship.time + 600
    fix = find_fix(at)
    dv = sqrt(fix[0] ** 2 + fix[1] ** 2 + fix[2] ** 2)
    print(f"fix {tries}: {off / km:.0f} km off; {fix[0]:.1f} forward, {fix[1]:.1f} sideways, {fix[2]:.1f} out = {dv:.1f} m/s should make it {fix[3] / km:.1f}")
    if dv < 0.2:
        break
    sleep(at - ship.time - burn_time(dv) / 2)
    aim_along(fix[0], fix[1], fix[2])
    burn(dv)
    off = miss(what_if(0, 0, 0, ship.time + 600))
print(f"the pass: {off / km:.1f} km off the {goal / km:.0f} km I want")

while ship.orbit.body != "krill":
    sleep(600)
low = ship.orbit.bottom
print(f"in Krill's pull, passing {low / km:.1f} km up")

# caught: at the bottom, slow to a circle's speed there
slow = speed_at(low) - speed_needed(low)
sleep(ship.orbit.time_to_bottom - burn_time(slow) / 2)
aim("retrograde")
burn(slow)
print(f"caught: {ship.orbit.bottom / km:.1f} x {ship.orbit.top / km:.1f} km, {ship.delta_v:.0f} m/s left")

# land on the lander: drop the transfer stage, light the lander's own engine
lander = ship.part("Puffling")
while not lander.lit:
    stage()
print(f"on the lander's own engine: {ship.delta_v:.0f} m/s left")

# down: drop the bottom to 8 km, fall to it, and brake to the ground
aim("retrograde")
throttle(100)
while ship.orbit.bottom > 8 * km:
    sleep(0.1)
throttle(0)
print(f"the bottom is {ship.orbit.bottom / km:.1f} km now ({ship.delta_v:.0f} m/s left)")
sleep(ship.orbit.time_to_bottom - 60)
legs()
# at the bottom, burn off the sideways speed: then it only falls
aim("surface_retrograde")
throttle(100)
while ship.horizontal_speed > 20:
    if ship.max_thrust == 0:
        print(f"no thrust at {ship.height:.0f} m: staging")
        stage()            # this stage is dry: on to the next
    sleep(0.1)
throttle(0)
print(f"sideways speed gone at {ship.height:.0f} m: falling ({ship.delta_v:.0f} m/s left)")
# fall until stopping takes most of the height left (with some to spare)
while ship.height > stopping_distance() * 1.5 + 100:
    chart("height", ship.height)
    sleep(0.1)
print(f"braking at {ship.height:.0f} m, falling {ship.falling_speed:.0f} m/s ({ship.delta_v:.0f} m/s left)")
# then down no faster than it could still stop in the height left (with a
# margin), to a walking pace at the end: the throttle that holds it still,
# and more (or less) for how far off that speed it is
upright = False
while not ship.landed:
    if not upright and ship.height < 150:
        aim("up")          # the last of it straight down, legs first
        upright = True
    if ship.max_thrust == 0:
        print(f"out of fuel at {ship.height:.0f} m")
        break
    push = ship.max_thrust * 1000 / ship.mass       # m/s2 the engine gives, flat out
    hover = 100 * ship.body.gravity / push
    want = 1.5 + 0.6 * sqrt(2 * max(push - ship.body.gravity, 0.1) * max(ship.height - 8, 0))
    throttle(clamp(hover + (ship.falling_speed - want) * 40, 0, 100))
    chart("height", ship.height)
throttle(0)
print(f"down on Krill with {ship.delta_v:.0f} m/s left")
"""

const BY_SPACE := """# each stage on the pilot's space
lifter = ship.part("Walrus")
drop = ship.part("Crack-1 Stack Decoupler 1")
wait_user_trigger()
lifter.throttle = 100
while ship.altitude < 3000:
    sleep(0.5)
lifter.throttle = 0
print(f"engine off at {ship.altitude:.0f} m, {len(ship.parts)} parts: space drops the stage")
wait_user_trigger()
drop.decouple()
print(f"dropped: {len(ship.parts)} parts left")
"""


func _check(ok: bool, msg: String) -> void:
	_log.append(("ok   " if ok else "FAIL ") + msg)
	if not ok:
		_failed += 1


func _frames(tree: SceneTree, n: int) -> void:
	for i in n:
		await tree.process_frame


func _scene(tree: SceneTree, craft: String, site: String, clock := -1.0) -> Node3D:
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	if clock >= 0.0:
		Sim.game.core.reset_time(clock)
	Sim.seats = {}
	for c in tree.root.get_children():
		if c is Node3D or (c is Control and c.name != "GateRunner"):
			c.queue_free()
	await _frames(tree, 2)
	Sim.pending_craft_text = FileAccess.get_file_as_string(Sim.STOCK_CRAFT_DIR + craft + ".craft.json")
	Sim.pending_site = site
	var scene: Node3D = load("res://scenes/flight.tscn").instantiate()
	tree.root.add_child(scene)
	await _frames(tree, 5)
	scene.hud.hide_help()
	return scene


## The pids of scripts still running on the computer.
static func _scripts(comp: ShipComputer) -> Array:
	# (the computer's own list of scripts, alive: a process a kill has left
	# behind in ps, never to run again, is no script still going)
	var live := {}
	for row in str(comp.machine.scripts()).split("\n", false):
		var g := row.split("\t")
		if g.size() >= 4 and g[1] == "1" and g[3] != "-i":
			live[g[0]] = true
	var pids := []
	for row in str(comp.sh("ps")).split("\n", false):
		var f := row.strip_edges().split(" ", false)
		if f.size() >= 6 and f[2] == "running" and f[f.size() - 1] in ["/bin/py", "/bin/sh"] and f[1] == "1" and live.has(f[0]):
			pids.append(f[0])
	return pids


## Runs a script on the ship's computer, stepping the flight, until it
## ends; a pilot presses space `presses` times when it waits for one.
func _fly(scene: Node3D, path: String, text: String, max_s: float, presses := 0) -> Dictionary:
	var s: FlightSession = scene.session
	var comp: ShipComputer = scene.computer
	for pid in _scripts(comp):
		comp.sh("kill " + str(pid))
	comp.machine.write_file(path, text)
	comp.log.clear()
	var why := str(comp.machine.start(path))
	var heard := []
	var seen := comp.log_count
	var t0 := s.time()
	var running := true
	var guard := 0
	var pressed := 0
	var waited := 0
	var wall := Time.get_ticks_msec()
	while running and s.time() - t0 < max_s and guard < 400000 and Time.get_ticks_msec() - wall < 600000:
		# the pilot warps through a script's long sleep, as the game lets
		# them (above the air, engines quiet), to a little before it wakes
		var tw := s.telemetry()
		var wake := comp.wake_at()
		var before := s.time()
		if wake - s.time() > 30.0 and float(tw.get("thrust", 0.0)) == 0.0 and not tw.get("in_atmosphere", true) \
				and float(tw.get("altitude_terrain", 0.0)) > 5000.0 and not comp.programs.running():
			s.warp_to(wake - 10.0)
		if s.time() - before < 0.01:
			# coasting, the pilot runs time at 4x (the physics warp); near
			# the ground, a tick at a time
			var agl := float(tw.get("altitude_terrain", 1e9))
			s.step_ticks(1 if agl < 300.0 and not tw.get("landed", false) else (20 if float(tw.get("thrust", 0.0)) == 0.0 and wake - s.time() > 5.0 else 5))
		guard += 1
		var u0 := Time.get_ticks_usec()
		comp.update(scene, s.telemetry())
		_t_update += Time.get_ticks_usec() - u0
		_t_step += u0 - _u_last
		_u_last = Time.get_ticks_usec()
		if guard % 500 == 0 and OS.has_environment("TRACE"):
			print("iter %d: update %.1f ms, the rest %.1f ms each; game t %.0f" % [guard, _t_update / 500000.0, _t_step / 500000.0, s.time() - t0])
			_t_update = 0
			_t_step = 0
		if comp.wants_space():
			waited += 1
			if waited > 30 and pressed < presses:   # (half a second's thought)
				comp.take_space()
				pressed += 1
				waited = 0
		var fresh := mini(comp.log_count - seen, comp.log.size())
		for k in range(comp.log.size() - fresh, comp.log.size()):
			if not str(comp.log[k]).begins_with("> "):
				heard.append(str(comp.log[k]))
		seen = comp.log_count
		if guard % 25 == 0:
			running = not _scripts(comp).is_empty()
		if guard % 120 == 0 and OS.has_environment("TRACE"):
			var tt := s.telemetry()
			var rd := comp.readings(tt)
			print("t=%.0f alt=%.0f pitch=%.1f hdg=%.1f sas=%s thr=%.2f thrust=%.0f lit=%d aimerr=%s ec=%.1f powered=%s spin=%.3f prog=%s/%s" % [s.time() - t0, float(tt.altitude), float(tt.pitch_deg), float(tt.heading_deg), tt.sas, float(tt.throttle), float(tt.thrust), int(tt.engines_lit), comp.machine.api("get aim_error"), float(tt.ec), tt.powered, float(tt.spin), comp.programs.name, comp.programs.status])
		if not s.telemetry().get("alive", false):
			break
	for pid in _scripts(comp):
		comp.sh("kill " + str(pid))
	return {"started": why == "", "why": why, "ended": not running, "said": heard, "t": s.time() - t0, "pressed": pressed}


func run(runner: Node) -> Dictionary:
	var tree := runner.get_tree()
	tree.paused = false
	var scene := await _scene(tree, "orbiter", "pad")
	var s: FlightSession = scene.session
	var comp: ShipComputer = scene.computer
	comp.update(scene, s.telemetry())
	var r := _fly(scene, "/root/scripts/to_orbit.py", TO_ORBIT.replace("TRIGGER1\n", "").replace("        TRIGGER2\n", ""), 3000.0)
	var t := s.telemetry()
	var o: Dictionary = t.get("orbit", {})
	var said := " / ".join(r.said)
	_check(r.started and r.ended and str(t.get("situation", "")) == "orbiting" and float(o.get("pe_alt", 0.0)) > 70000.0
			and said.contains("In orbit:") and said.contains("dropping it"),
		"to_orbit.py (parts by name, the maths, the burn): orbiting %.1f x %.1f km after %.0f s (%s)" % [
			float(o.get("pe_alt", 0.0)) / 1000.0, float(o.get("ap_alt", 0.0)) / 1000.0, r.t, said])
	_check(comp.charts.has("height") and (comp.charts.height as Array).size() > 20,
		"chart(\"height\", ...) kept %d points for the Flight app's graph" % (comp.charts.get("height", []) as Array).size())
	scene.queue_free()
	await _frames(tree, 2)

	# 2. each stage on the pilot's space
	scene = await _scene(tree, "orbiter", "pad")
	s = scene.session
	comp = scene.computer
	comp.update(scene, s.telemetry())
	r = _fly(scene, "/root/scripts/by_space.py", BY_SPACE, 600.0, 2)
	said = " / ".join(r.said)
	_check(r.started and r.ended and r.pressed == 2 and said.contains("waiting for you to press space") and said.contains("parts left")
			and int(s.telemetry().get("next_stage", -1)) == 1,
		"wait_user_trigger(): the script waited for the pilot's space twice (liftoff, then the drop) and went on each time; the stage list caught up (next stage %d) (started %s %s, pressed %d, %.0f s: %s)" % [int(s.telemetry().get("next_stage", -1)), r.started, r.why, r.pressed, r.t, said])

	scene.queue_free()
	await _frames(tree, 2)

	# 3. the hangar's "Make script": the Krill lander's stages as a script,
	# run with a space a stage, does what pressing space does
	var model := CraftModel.new()
	model.load_file(Sim.STOCK_CRAFT_DIR + "krill_lander.craft.json")
	var made: String = preload("res://scripts/os/script_maker.gd").make(model.parts, model.craft_name)
	var presses := made.count("\nwait_user_trigger()\n")
	scene = await _scene(tree, "krill_lander", "pad")
	s = scene.session
	comp = scene.computer
	comp.update(scene, s.telemetry())
	var n_pieces := Sim.vessels().size()
	r = _fly(scene, "/root/scripts/krill_lander.py", made, 600.0, presses)
	said = " / ".join(r.said)
	_check(r.started and r.ended and r.pressed == presses and said.contains("every stage done") and not said.contains("py: error")
			and int(s.telemetry().get("next_stage", 9)) <= 0 and Sim.vessels().size() > n_pieces + 3,
		"Make script (the Krill lander, %d stages): run with a space each, every stage went as space does: next stage %d, %d pieces flying (%s)" % [
			presses, int(s.telemetry().get("next_stage", 9)), Sim.vessels().size(), said])
	_log.append("     the script it made:\n" + made.indent("        "))
	scene.queue_free()
	await _frames(tree, 2)

	# 4. to Krill and down, by your own logic
	# (from the clock it has when run alone: the whole run's earlier gates
	# leave the moons elsewhere, and one in a few of those trips missed)
	var t_was: float = Sim.game.core.time()
	scene = await _scene(tree, "krill_lander", "pad", 333.0)
	s = scene.session
	var t_from := s.time()
	comp = scene.computer
	comp.update(scene, s.telemetry())
	r = _fly(scene, "/root/scripts/krill_by_hand.py", KRILL, 3.0e6)
	t = s.telemetry()
	said = " / ".join(r.said)
	_check(r.started and r.ended and t.get("landed", false) and str(t.get("body", "")) == "krill" and float(t.get("pitch_deg", 0.0)) > 60.0
			and float(t.get("max_impact", 99.0)) < 6.0 and said.contains("down on Krill"),
		"krill_by_hand.py: when_to_leave, a what_if search for the fix, a capture worked out, a landing by hand: landed %s on %s, pitch %.0f, worst contact %.1f m/s, %.0f s (%s)" % [
			t.get("landed", false), t.get("body", "?"), float(t.get("pitch_deg", 0.0)), float(t.get("max_impact", 0.0)), r.t, said])
	var spent := s.time() - t_from
	scene.queue_free()
	await _frames(tree, 2)
	for v in Sim.vessels():
		Sim.game.core.vessel_remove(int(v.id))
	Sim.game.core.reset_time(t_was + spent)   # (the clock goes on from where it was)
	Sim.seats = {}
	Sim.save_game()
	return {"status": "FAIL" if _failed else "PASS", "log": "\n".join(_log)}
