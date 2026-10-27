/*
 * Tux Rocket Science
 * Copyright (C) 2026 Tux Rocket Science contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* stdint.h — a shim, so the lifted language files can include what they expect.
 *
 * There is no libc on this machine. The four files that make up the language
 * (value.c, lex.c, compile.c, vm.c) are compiled here UNCHANGED, and they
 * include the headers any host C file would; providing empty ones that route
 * to nom.h is what lets them stay unchanged.
 *
 * Everything they actually call -- strlen, memcpy, strcmp, snprintf's job --
 * is in nom.h, and this file exists so the #include line above it resolves.
 */
#ifndef GUEST_SHIM_STDINT_H
#define GUEST_SHIM_STDINT_H
#include "nom.h"
#endif
