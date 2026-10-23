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

/* Public entry header for trs_core: the authoritative, headless simulation. */
#ifndef TRS_CORE_H
#define TRS_CORE_H

#include <stddef.h>

#define TRS_VERSION_MAJOR 0
#define TRS_VERSION_MINOR 1
#define TRS_VERSION_PATCH 0

const char *trs_version_string(void);

#include "trs/gate.h"

#endif
