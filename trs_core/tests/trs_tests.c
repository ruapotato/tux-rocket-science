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

/* Standalone gate runner: trs_tests [--data DIR] [--strict] [--all | --<gate> ...]
 * Exit code 0 when no gate failed. With --strict, PENDING also fails. */
#include "trs/trs_core.h"

#include "trs/craft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* trs_tests --data DIR --normalize-craft FILE...: rewrite craft files in
 * canonical form (the exact bytes the game saves). */
static int normalize(const char *data_dir, int argc, char **argv, int first) {
	char path[1024], err[256];
	trs_part_catalog cat;
	snprintf(path, sizeof path, "%s/parts/stock_parts.json", data_dir);
	if (!trs_catalog_load(&cat, path, err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		return 1;
	}
	for (int i = first; i < argc; i++) {
		trs_json *j = trs_json_load_file(argv[i], err, sizeof err);
		trs_craft c;
		if (!j || !trs_craft_from_json(&c, &cat, j, err, sizeof err)) {
			fprintf(stderr, "%s: %s\n", argv[i], err);
			return 1;
		}
		trs_json *o = trs_craft_to_json(&c);
		char *text = trs_json_write(o);
		FILE *f = fopen(argv[i], "wb");
		fwrite(text, 1, strlen(text), f);
		fclose(f);
		printf("normalized %s\n", argv[i]);
		free(text);
		trs_json_free(o);
		trs_json_free(j);
		trs_craft_free(&c);
	}
	trs_catalog_free(&cat);
	return 0;
}

static int run_one(const trs_gate_def *g, const char *data_dir, int strict) {
	if (!g->fn) {
		printf("[%-20s] SKIP     (driven from Godot, %s)\n", g->name, g->milestone);
		return 0;
	}
	trs_log log = { 0 };
	trs_gate_status s = g->fn(&log, data_dir);
	printf("[%-20s] %-8s (%s)\n", g->name, trs_gate_status_name(s), g->milestone);
	if (log.buf)
		for (char *line = strtok(log.buf, "\n"); line; line = strtok(NULL, "\n"))
			printf("    %s\n", line);
	trs_log_free(&log);
	return s == TRS_GATE_FAIL || (strict && s == TRS_GATE_PENDING);
}

int main(int argc, char **argv) {
	const char *data_dir = "data";
	int strict = 0, all = 0, ran = 0, failed = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--data") && i + 1 < argc)
			data_dir = argv[++i];
		else if (!strcmp(argv[i], "--strict"))
			strict = 1;
		else if (!strcmp(argv[i], "--all"))
			all = 1;
		else if (!strcmp(argv[i], "--normalize-craft"))
			return normalize(data_dir, argc, argv, i + 1);
	}
	printf("trs_core %s gate runner (data: %s)\n", trs_version_string(), data_dir);
	size_t n;
	const trs_gate_def *gates = trs_gates(&n);
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--", 2) || !strcmp(argv[i], "--all") || !strcmp(argv[i], "--strict"))
			continue;
		if (!strcmp(argv[i], "--data")) {
			i++;
			continue;
		}
		const trs_gate_def *g = trs_gate_find(argv[i] + 2);
		if (!g) {
			fprintf(stderr, "unknown gate %s\n", argv[i]);
			return 2;
		}
		failed += run_one(g, data_dir, strict);
		ran++;
	}
	if (all || !ran)
		for (size_t i = 0; i < n; i++)
			failed += run_one(&gates[i], data_dir, strict);
	printf(failed ? "%d gate(s) FAILED\n" : "all gates ok%.0d\n", failed);
	return failed ? 1 : 0;
}
