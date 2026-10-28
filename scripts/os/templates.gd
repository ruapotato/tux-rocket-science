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
	["Blank", "my_script.py", BLANK],
]
