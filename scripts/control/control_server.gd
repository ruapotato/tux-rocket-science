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
## Control socket: newline-delimited JSON over TCP on 127.0.0.1.
## Enabled with `-- --control-port=7373` (add `--blind` to run without the
## main menu, e.g. headless). Each line is a ControlAPI command; each reply
## is one JSON line. A running flight scene attaches its live session.
extends Node

var api: ControlAPI
var _server: TCPServer
var _clients: Array[StreamPeerTCP] = []
var _buffers: Array[String] = []


func _ready() -> void:
	var port := 0
	var blind := false
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--control-port="):
			port = int(a.get_slice("=", 1))
		elif a == "--blind":
			blind = true
	if port <= 0:
		set_process(false)
		return
	process_mode = Node.PROCESS_MODE_ALWAYS
	api = ControlAPI.new()
	_server = TCPServer.new()
	var err := _server.listen(port, "127.0.0.1")
	print("control socket %s on 127.0.0.1:%d" % ["listening" if err == OK else "FAILED (%s)" % error_string(err), port])
	if blind:
		get_tree().paused = true
		(func(): if get_tree().current_scene: get_tree().current_scene.queue_free()).call_deferred()


## The flight scene hands over its live session so commands act on it.
func attach(session: FlightSession) -> void:
	if api:
		api.session = session


func _process(_dt: float) -> void:
	while _server and _server.is_connection_available():
		_clients.append(_server.take_connection())
		_buffers.append("")
	for k in range(_clients.size() - 1, -1, -1):
		var c := _clients[k]
		c.poll()
		if c.get_status() != StreamPeerTCP.STATUS_CONNECTED:
			_clients.remove_at(k)
			_buffers.remove_at(k)
			continue
		var n := c.get_available_bytes()
		if n > 0:
			_buffers[k] += c.get_utf8_string(n)
		while _buffers[k].contains("\n"):
			var line := _buffers[k].get_slice("\n", 0)
			_buffers[k] = _buffers[k].substr(line.length() + 1)
			if line.strip_edges() == "":
				continue
			var cmd: Variant = JSON.parse_string(line)
			var reply: Dictionary
			if typeof(cmd) != TYPE_DICTIONARY:
				reply = {"ok": false, "error": "bad json"}
			else:
				reply = api.handle(cmd)
			c.put_data((JSON.stringify(reply) + "\n").to_utf8_buffer())
