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

/* Small JSON DOM: parse, query, build, and write deterministically.
 * Object keys keep insertion order so save -> load -> save is byte-identical. */
#ifndef TRS_JSON_H
#define TRS_JSON_H

#include <stddef.h>

typedef enum {
	TRS_JSON_NULL,
	TRS_JSON_BOOL,
	TRS_JSON_NUMBER,
	TRS_JSON_STRING,
	TRS_JSON_ARRAY,
	TRS_JSON_OBJECT,
} trs_json_type;

typedef struct trs_json trs_json;
struct trs_json {
	trs_json_type type;
	int b;
	double num;
	char *str;      /* STRING value */
	char **keys;    /* OBJECT keys, parallel to items */
	trs_json **items;
	size_t count, cap;
};

/* Returns NULL on error and writes a message into err (if non-NULL). */
trs_json *trs_json_parse(const char *text, char *err, size_t errlen);
trs_json *trs_json_load_file(const char *path, char *err, size_t errlen);
void trs_json_free(trs_json *j);

const trs_json *trs_json_get(const trs_json *obj, const char *key);
const trs_json *trs_json_at(const trs_json *arr, size_t i);
double trs_json_num(const trs_json *obj, const char *key, double fallback);
const char *trs_json_str(const trs_json *obj, const char *key, const char *fallback);
int trs_json_bool(const trs_json *obj, const char *key, int fallback);

trs_json *trs_json_new(trs_json_type t);
trs_json *trs_json_new_num(double v);
trs_json *trs_json_new_str(const char *s);
trs_json *trs_json_new_bool(int b);
void trs_json_push(trs_json *arr, trs_json *item);
void trs_json_set(trs_json *obj, const char *key, trs_json *item); /* append or replace */

/* Pretty-printed with 2-space indent and a trailing newline. Caller frees. */
char *trs_json_write(const trs_json *j);
/* Compact, single line. Caller frees. */
char *trs_json_write_compact(const trs_json *j);
/* Shortest decimal that round-trips to exactly v. buf >= 32 bytes. */
void trs_json_format_number(double v, char *buf);

#endif
