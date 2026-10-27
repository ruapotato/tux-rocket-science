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
cmake_minimum_required(VERSION 3.16)
project(tux_rocket_science C)

# Keep Godot from importing build products when the build dir is in the project.
file(WRITE "${CMAKE_BINARY_DIR}/.gdignore" "")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Keep math strict: the sim must be deterministic across platforms.
set(TRS_WARN -Wall -Wextra -Wno-unused-parameter -ffp-contract=off -fno-fast-math)

# The ship's computer: NOMINAL's machine by way of RUNBOOK (os/machine,
# borrowed and kept as it was) and the bridge to the ship (os/core). Its
# names stay inside the library.
file(GLOB TRS_OS_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/os/machine/*.c ${CMAKE_SOURCE_DIR}/os/core/*.c)
add_library(trs_os STATIC ${TRS_OS_SRC})
target_include_directories(trs_os PUBLIC ${CMAKE_SOURCE_DIR}/os/core ${CMAKE_SOURCE_DIR}/os/machine)
target_compile_options(trs_os PRIVATE -std=gnu11 -Wno-comment -fvisibility=hidden)
set_target_properties(trs_os PROPERTIES POSITION_INDEPENDENT_CODE ON)

file(GLOB TRS_CORE_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/trs_core/src/*.c)
add_library(trs_core_lib STATIC ${TRS_CORE_SRC})
target_link_libraries(trs_core_lib PUBLIC trs_os)
target_include_directories(trs_core_lib PUBLIC ${CMAKE_SOURCE_DIR}/trs_core/include)
target_compile_options(trs_core_lib PRIVATE ${TRS_WARN})
set_target_properties(trs_core_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
  # mingw's own printf/strtod: correctly rounded, so JSON round-trips exactly
  target_compile_definitions(trs_core_lib PUBLIC __USE_MINGW_ANSI_STDIO=1)
else()
  target_link_libraries(trs_core_lib PUBLIC m)
endif()

# GDExtension shared library, written straight into res://bin
file(GLOB TRS_GDEXT_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/trs_core/gdext/*.c)
add_library(trs_core SHARED ${TRS_GDEXT_SRC})
target_include_directories(trs_core PRIVATE ${CMAKE_SOURCE_DIR}/trs_core/third_party/godot)
target_compile_options(trs_core PRIVATE ${TRS_WARN} -fvisibility=hidden)
target_link_libraries(trs_core PRIVATE trs_core_lib trs_os)
if(WIN32)
  set(TRS_LIB_NAME "trs_core.windows.x86_64")
  target_link_options(trs_core PRIVATE -static-libgcc -static)
else()
  set(TRS_LIB_NAME "trs_core.linux.x86_64")
endif()
set_target_properties(trs_core PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME ${TRS_LIB_NAME}
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Standalone headless gate runner (no Godot needed)
add_executable(trs_tests ${CMAKE_SOURCE_DIR}/trs_core/tests/trs_tests.c)
target_link_libraries(trs_tests PRIVATE trs_core_lib trs_os)
target_compile_options(trs_tests PRIVATE ${TRS_WARN})
if(WIN32)
  target_link_options(trs_tests PRIVATE -static-libgcc -static)
endif()
