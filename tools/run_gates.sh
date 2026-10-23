#!/usr/bin/env bash
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
# Build trs_core and run every verification gate headless.
#   tools/run_gates.sh            build + native gates + Godot gates
#   STRICT=1 tools/run_gates.sh   PENDING gates count as failures
#   GODOT=/path/to/godot          override the Godot binary
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

GODOT="${GODOT:-$(ls "$ROOT"/Godot_v4*_linux.x86_64 2>/dev/null | head -1 || true)}"
GODOT="${GODOT:-godot}"
STRICT_FLAG=""
[[ "${STRICT:-0}" == "1" ]] && STRICT_FLAG="--strict"

echo "== license headers"
python3 tools/license_headers.py

echo "== build (linux)"
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/linux

echo "== native gates (trs_tests)"
./build/linux/trs_tests --data data --all $STRICT_FLAG

echo "== godot gates"
if [[ ! -d .godot ]]; then
  "$GODOT" --headless --import >/dev/null 2>&1 || true
fi
"$GODOT" --headless -- --all-gates $STRICT_FLAG

# Optional: WINE_GODOT=/path/to/Godot_win64_console.exe also runs the Windows
# build under wine64 (cross-compiled with mingw-w64).
if [[ -n "${WINE_GODOT:-}" ]]; then
  echo "== windows build + gates (wine64)"
  cmake -S . -B build/windows -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake >/dev/null
  cmake --build build/windows
  WINE="${WINE:-$(command -v wine64 || echo /usr/lib/wine/wine64)}"
  WINEDEBUG=-all "$WINE" build/windows/trs_tests.exe --data data --all $STRICT_FLAG
  WINEDEBUG=-all "$WINE" "$WINE_GODOT" --headless -- --all-gates $STRICT_FLAG
fi
