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
# templates.gd -- the Script Editor's "New": scripts to start from. Every
# one is flown by test-by-hand (tools/sync_examples.py puts them in
# /root/examples too), so each works as it is.

const FIRST := """# first_script.py -- your first flight script. Press Run (above).
#
# Anything after a # is a note for you: the computer skips it. Run this on
# the pad, then change a number and run it again: it's yours.

print("Hello from the ship's computer!")
print(f"We're on {ship.body.name}, {ship.height:.0f} m above the ground.")

engine = ship.engines[0]          # the first engine to fire (try ship.part("...") too)
print("Our engine is the", engine.name)

countdown(3)
engine.throttle = 100             # full power: it lights, and the clamps let go
aim("up")                         # nose up, and hold it there

# climb to 2 km, drawing the climb on the flight screen as we go
while ship.height < 2000:
    chart("height", ship.height)
    sleep(0.2)

engine.throttle = 0
print("Engine off at", round(ship.height), "m: coasting up...")
while ship.vertical_speed > 0:
    chart("height", ship.height)
    sleep(0.2)
print(f"The top: {ship.height:.0f} m! Down by parachute.")
chutes()

# Things to try:
#   climb higher: change 2000 to 8000 (is there fuel for it?)
#   lean east as you climb: aim_at(80, 90) instead of aim("up")
#   the console below runs a line now: try  ship.mass  or  engine.fuel
#   click by a line number to stop there (a breakpoint), then Step
"""

const HOVER := """# hover.py -- hold 100 m over the pad, then come down to 20 m
#
# A controller: each tenth of a second it works out the throttle that would
# just hold the ship up, then pushes harder the further below the height it
# is, and eases off as it rises. Tune the 1.5 and the 3 and watch the chart.
lifter = ship.engines[0]
target = 100
print("lift off: holding", target, "m")
lifter.throttle = 60                 # lights it: the clamps let go
aim("up")
t0 = ship.time
while ship.time - t0 < 45:
    if ship.time - t0 > 28 and target != 20:
        target = 20
        print("down to", target, "m")
    push = lifter.max_thrust * 1000 / ship.mass     # m/s2, flat out
    hover = 100 * ship.body.gravity / push          # % that just holds it
    err = target - ship.height
    lifter.throttle = clamp(hover + err * 1.5 - ship.vertical_speed * 3, 0, 100)
    chart("height", ship.height)
    chart("throttle", lifter.throttle)
    sleep(0.1)
lifter.throttle = 0
print(f"done: {ship.height:.1f} m up, {ship.fuel:.0f}% fuel left")
"""

const WATCH := """# watch.py -- a co-pilot: it keeps an eye out while YOU fly
#
# Run it, then fly with the keys as usual: it says on the flight screen what
# it sees. It never touches the controls. Add your own warnings!
said = []

def warn(key, text):
    # each warning once (until it's clear again)
    if key not in said:
        say(text)
        print(text)
        said.append(key)

def clear(key):
    if key in said:
        said.remove(key)

while True:
    if ship.fuel < 10 and ship.thrust > 0:
        warn("fuel", f"Under 10% fuel in this stage ({ship.delta_v:.0f} m/s left in all)")
    else:
        clear("fuel")
    hot = [p for p in ship.parts if p.heat > 80]
    if hot:
        warn("heat", f"{hot[0].name} is at {hot[0].heat:.0f}% of melting!")
    else:
        clear("heat")
    if ship.height < 400 and ship.falling_speed > 15 and not ship.landed:
        warn("ground", f"Falling {ship.falling_speed:.0f} m/s, {ship.height:.0f} m up!")
    else:
        clear("ground")
    chart("height", ship.height)
    sleep(0.5)
"""

const SUICIDE := """# suicide_burn.py -- up 3 km, fall, and light the engine at the last moment
#
# The way boosters land: no fuel spent hovering, just one hard burn that
# ends at the ground. stopping_distance() says how far it takes to stop at
# full power; the trick is lighting it no sooner than that.
e = ship.engines[0]
e.throttle = 100
aim("up")
# burn until the coast alone would carry it to 3 km: what you're going up
# at carries you v * v / (2 * gravity) higher
while ship.height + ship.vertical_speed ** 2 / (2 * ship.body.gravity) < 3000:
    chart("height", ship.height)
    sleep(0.05)
e.throttle = 0
print(f"engine off at {ship.height:.0f} m: coasting up to 3 km...")
while ship.vertical_speed > 0:
    chart("height", ship.height)
    sleep(0.2)
aim("surface_retrograde")
while ship.height > stopping_distance() * 1.1 + 20:
    chart("height", ship.height)
    sleep(0.05)
print(f"burn! {ship.height:.0f} m up, falling {ship.falling_speed:.0f} m/s")
# just enough to stop at the ground: the push that turns this speed to
# nothing in the height left, gravity on top
while not ship.landed:
    push = e.max_thrust * 1000 / ship.mass
    left = max(ship.height - 4, 0.5)
    need = ship.falling_speed ** 2 / (2 * left) + ship.body.gravity
    if ship.falling_speed < 2:
        need = ship.body.gravity * 0.95       # the last metres, gently
    e.throttle = clamp(100 * need / push, 0, 100)
    chart("height", ship.height)
e.throttle = 0
print(f"down! {ship.fuel:.0f}% fuel left")
"""

const GUARD := """# guard.py -- the ship's lasers keep the ground round it clear
#
# Land, run this, and go for a walk (F): whatever lives near the ship and
# isn't a penguin, the lasers see off while you're out. ship.life lists
# what is alive near (nearest first); each has a name, a distance (m), a
# health, and whether it's hostile and whether it's a penguin. Only want
# the dangerous ones gone? Add  and c.hostile  to the test below.
reach = 300                    # m round the ship
lasers = ship.lasers
if not lasers:
    print("No lasers aboard: fit a Tern Laser Turret (Utility) and try again")
else:
    print(f"On guard with {len(lasers)} laser(s), out to {reach} m")
    aimed = None
    while True:
        near = [c for c in ship.life if not c.penguin and c.distance < reach]
        if near:
            if near[0] != aimed:
                aimed = near[0]
                print(f"Zapping a {aimed.name}, {aimed.distance:.0f} m off")
            ship.fire_at(aimed)       # the lasers lock on it, and fire while it's in reach
        elif aimed:
            aimed = None
            ship.cease_fire()
        sleep(0.5)
"""

const DRILL := """# drill_guard.py -- dig for propellant, and keep what comes for the drill off it
#
# A drill digging is loud: hunters within 1.5 km hear it and come to chew
# on it, and a Jaeger camp within 30 km sends its raiders. Land on legs
# with a drill within 2 m of the ground (and an ore bin, a refinery and
# the power for them), run this, and warp: it digs till the tanks are
# full, and the lasers take whatever comes for the drill, nearest first.
# Warp stops when something has heard it; warp on once it's dealt with.
# (ship.heard says what has: "" while all is quiet. A sleep holds time
# warp back so it wakes on time; so, quiet, it sleeps an hour: something
# hearing the drill wakes it at once.)
reach = 600                    # m: as far as ship.threats looks (a Tern's beam reaches 1500)
lasers = ship.lasers
if not lasers:
    print("No lasers aboard: the drill will be eaten. Fit a Tern Laser Turret (Utility)")
mining()
print(f"Digging: {ship.liquid:.0f} of {ship.liquid_capacity:.0f} kg")
aimed = None
while ship.liquid < ship.liquid_capacity * 0.995:
    near = [c for c in ship.threats if c.distance < reach]
    if near and lasers:
        if near[0] != aimed:
            aimed = near[0]
            print(f"{aimed.name}, {aimed.distance:.0f} m off: firing")
        ship.fire_at(aimed)       # the lasers lock on it, and fire while it's in reach
    elif aimed:
        aimed = None
        ship.cease_fire()
        print("All clear")
    if near or ship.heard:
        sleep(0.5)
    else:
        sleep(3600)
mining(False)
ship.cease_fire()
print(f"Tanks full ({ship.liquid:.0f} kg): the drill is off, and quiet")
"""

const BLANK := """# my_script.py
#
# ship.  (type the dot: it lists what the ship has)   ship.part("...")
# Run starts it; the watch shows its variables; the console tries a line.

print(ship.situation, ship.height)
"""

## label, file name, text
const LIST := [
	["Your first script (a hop)", "first_script.py", FIRST],
	["Hover: hold a height", "hover.py", HOVER],
	["Co-pilot: warnings while you fly", "watch.py", WATCH],
	["Suicide burn: land like a booster", "suicide_burn.py", SUICIDE],
	["Guard: the lasers keep the ground clear", "guard.py", GUARD],
	["Drill guard: dig, and defend the drill", "drill_guard.py", DRILL],
	["Blank", "my_script.py", BLANK],
]
