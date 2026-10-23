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

#include "trs/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	const char *p;
	char *err;
	size_t errlen;
	int failed;
} parser;

static void fail(parser *ps, const char *msg) {
	if (!ps->failed && ps->err)
		snprintf(ps->err, ps->errlen, "%s near '%.20s'", msg, ps->p);
	ps->failed = 1;
}

static void skip_ws(parser *ps) {
	for (;;) {
		while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
			ps->p++;
		/* tolerate // comments so data files can be annotated */
		if (ps->p[0] == '/' && ps->p[1] == '/') {
			while (*ps->p && *ps->p != '\n')
				ps->p++;
			continue;
		}
		break;
	}
}

trs_json *trs_json_new(trs_json_type t) {
	trs_json *j = calloc(1, sizeof(*j));
	j->type = t;
	return j;
}

trs_json *trs_json_new_num(double v) {
	trs_json *j = trs_json_new(TRS_JSON_NUMBER);
	j->num = v;
	return j;
}

static char *dup_str(const char *s) {
	size_t n = strlen(s);
	char *d = malloc(n + 1);
	memcpy(d, s, n + 1);
	return d;
}

trs_json *trs_json_new_str(const char *s) {
	trs_json *j = trs_json_new(TRS_JSON_STRING);
	j->str = dup_str(s);
	return j;
}

trs_json *trs_json_new_bool(int b) {
	trs_json *j = trs_json_new(TRS_JSON_BOOL);
	j->b = b != 0;
	return j;
}

static void grow(trs_json *j) {
	if (j->count < j->cap)
		return;
	j->cap = j->cap ? j->cap * 2 : 4;
	j->items = realloc(j->items, j->cap * sizeof(*j->items));
	if (j->type == TRS_JSON_OBJECT)
		j->keys = realloc(j->keys, j->cap * sizeof(*j->keys));
}

void trs_json_push(trs_json *arr, trs_json *item) {
	grow(arr);
	arr->items[arr->count++] = item;
}

void trs_json_set(trs_json *obj, const char *key, trs_json *item) {
	for (size_t i = 0; i < obj->count; i++)
		if (!strcmp(obj->keys[i], key)) {
			trs_json_free(obj->items[i]);
			obj->items[i] = item;
			return;
		}
	grow(obj);
	obj->keys[obj->count] = dup_str(key);
	obj->items[obj->count++] = item;
}

void trs_json_free(trs_json *j) {
	if (!j)
		return;
	for (size_t i = 0; i < j->count; i++) {
		trs_json_free(j->items[i]);
		if (j->keys)
			free(j->keys[i]);
	}
	free(j->items);
	free(j->keys);
	free(j->str);
	free(j);
}

static void put_utf8(char **o, unsigned cp) {
	char *q = *o;
	if (cp < 0x80) {
		*q++ = (char)cp;
	} else if (cp < 0x800) {
		*q++ = (char)(0xC0 | (cp >> 6));
		*q++ = (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		*q++ = (char)(0xE0 | (cp >> 12));
		*q++ = (char)(0x80 | ((cp >> 6) & 0x3F));
		*q++ = (char)(0x80 | (cp & 0x3F));
	} else {
		*q++ = (char)(0xF0 | (cp >> 18));
		*q++ = (char)(0x80 | ((cp >> 12) & 0x3F));
		*q++ = (char)(0x80 | ((cp >> 6) & 0x3F));
		*q++ = (char)(0x80 | (cp & 0x3F));
	}
	*o = q;
}

static int hex4(const char *p, unsigned *out) {
	unsigned v = 0;
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		v <<= 4;
		if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
		else return 0;
	}
	*out = v;
	return 1;
}

static char *parse_string_raw(parser *ps) {
	if (*ps->p != '"') {
		fail(ps, "expected string");
		return NULL;
	}
	ps->p++;
	const char *start = ps->p;
	size_t maxlen = 0;
	while (start[maxlen] && start[maxlen] != '"') {
		if (start[maxlen] == '\\' && start[maxlen + 1])
			maxlen++;
		maxlen++;
	}
	char *out = malloc(maxlen + 1), *o = out;
	while (*ps->p && *ps->p != '"') {
		char c = *ps->p++;
		if (c != '\\') {
			*o++ = c;
			continue;
		}
		c = *ps->p++;
		switch (c) {
		case '"': *o++ = '"'; break;
		case '\\': *o++ = '\\'; break;
		case '/': *o++ = '/'; break;
		case 'b': *o++ = '\b'; break;
		case 'f': *o++ = '\f'; break;
		case 'n': *o++ = '\n'; break;
		case 'r': *o++ = '\r'; break;
		case 't': *o++ = '\t'; break;
		case 'u': {
			unsigned cp;
			if (!hex4(ps->p, &cp)) {
				fail(ps, "bad \\u escape");
				free(out);
				return NULL;
			}
			ps->p += 4;
			if (cp >= 0xD800 && cp < 0xDC00 && ps->p[0] == '\\' && ps->p[1] == 'u') {
				unsigned lo;
				if (hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					ps->p += 6;
				}
			}
			put_utf8(&o, cp);
		} break;
		default:
			fail(ps, "bad escape");
			free(out);
			return NULL;
		}
	}
	if (*ps->p != '"') {
		fail(ps, "unterminated string");
		free(out);
		return NULL;
	}
	ps->p++;
	*o = 0;
	return out;
}

static trs_json *parse_value(parser *ps, int depth);

static trs_json *parse_value(parser *ps, int depth) {
	if (depth > 200) {
		fail(ps, "nesting too deep");
		return NULL;
	}
	skip_ws(ps);
	char c = *ps->p;
	if (c == '{') {
		ps->p++;
		trs_json *o = trs_json_new(TRS_JSON_OBJECT);
		skip_ws(ps);
		if (*ps->p == '}') {
			ps->p++;
			return o;
		}
		for (;;) {
			skip_ws(ps);
			char *k = parse_string_raw(ps);
			if (!k) break;
			skip_ws(ps);
			if (*ps->p != ':') {
				free(k);
				fail(ps, "expected ':'");
				break;
			}
			ps->p++;
			trs_json *v = parse_value(ps, depth + 1);
			if (!v) {
				free(k);
				break;
			}
			trs_json_set(o, k, v);
			free(k);
			skip_ws(ps);
			if (*ps->p == ',') {
				ps->p++;
				continue;
			}
			if (*ps->p == '}') {
				ps->p++;
				return o;
			}
			fail(ps, "expected ',' or '}'");
			break;
		}
		trs_json_free(o);
		return NULL;
	}
	if (c == '[') {
		ps->p++;
		trs_json *a = trs_json_new(TRS_JSON_ARRAY);
		skip_ws(ps);
		if (*ps->p == ']') {
			ps->p++;
			return a;
		}
		for (;;) {
			trs_json *v = parse_value(ps, depth + 1);
			if (!v) break;
			trs_json_push(a, v);
			skip_ws(ps);
			if (*ps->p == ',') {
				ps->p++;
				continue;
			}
			if (*ps->p == ']') {
				ps->p++;
				return a;
			}
			fail(ps, "expected ',' or ']'");
			break;
		}
		trs_json_free(a);
		return NULL;
	}
	if (c == '"') {
		char *s = parse_string_raw(ps);
		if (!s) return NULL;
		trs_json *j = trs_json_new(TRS_JSON_STRING);
		j->str = s;
		return j;
	}
	if (!strncmp(ps->p, "true", 4)) {
		ps->p += 4;
		return trs_json_new_bool(1);
	}
	if (!strncmp(ps->p, "false", 5)) {
		ps->p += 5;
		return trs_json_new_bool(0);
	}
	if (!strncmp(ps->p, "null", 4)) {
		ps->p += 4;
		return trs_json_new(TRS_JSON_NULL);
	}
	if (c == '-' || (c >= '0' && c <= '9')) {
		char *end;
		double v = strtod(ps->p, &end);
		if (end == ps->p) {
			fail(ps, "bad number");
			return NULL;
		}
		ps->p = end;
		return trs_json_new_num(v);
	}
	fail(ps, "unexpected character");
	return NULL;
}

trs_json *trs_json_parse(const char *text, char *err, size_t errlen) {
	parser ps = { text, err, errlen, 0 };
	trs_json *j = parse_value(&ps, 0);
	if (j) {
		skip_ws(&ps);
		if (*ps.p) {
			fail(&ps, "trailing characters");
			trs_json_free(j);
			return NULL;
		}
	}
	return j;
}

trs_json *trs_json_load_file(const char *path, char *err, size_t errlen) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		if (err) snprintf(err, errlen, "cannot open %s", path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)n + 1);
	size_t got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	buf[got] = 0;
	trs_json *j = trs_json_parse(buf, err, errlen);
	free(buf);
	return j;
}

const trs_json *trs_json_get(const trs_json *obj, const char *key) {
	if (!obj || obj->type != TRS_JSON_OBJECT)
		return NULL;
	for (size_t i = 0; i < obj->count; i++)
		if (!strcmp(obj->keys[i], key))
			return obj->items[i];
	return NULL;
}

const trs_json *trs_json_at(const trs_json *arr, size_t i) {
	if (!arr || (arr->type != TRS_JSON_ARRAY && arr->type != TRS_JSON_OBJECT) || i >= arr->count)
		return NULL;
	return arr->items[i];
}

double trs_json_num(const trs_json *obj, const char *key, double fallback) {
	const trs_json *j = trs_json_get(obj, key);
	return j && j->type == TRS_JSON_NUMBER ? j->num : fallback;
}

const char *trs_json_str(const trs_json *obj, const char *key, const char *fallback) {
	const trs_json *j = trs_json_get(obj, key);
	return j && j->type == TRS_JSON_STRING ? j->str : fallback;
}

int trs_json_bool(const trs_json *obj, const char *key, int fallback) {
	const trs_json *j = trs_json_get(obj, key);
	return j && j->type == TRS_JSON_BOOL ? j->b : fallback;
}

/* ---- writer ------------------------------------------------------------ */

typedef struct {
	char *buf;
	size_t len, cap;
} sbuf;

static void sb_put(sbuf *b, const char *s, size_t n) {
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 256;
		while (cap < b->len + n + 1)
			cap *= 2;
		b->buf = realloc(b->buf, cap);
		b->cap = cap;
	}
	memcpy(b->buf + b->len, s, n);
	b->len += n;
	b->buf[b->len] = 0;
}

static void sb_puts(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }

void trs_json_format_number(double v, char *buf) {
	if (!isfinite(v)) {
		strcpy(buf, "0");
		return;
	}
	if (v == 0) {
		strcpy(buf, "0");
		return;
	}
	if (v == floor(v) && fabs(v) < 1e15) {
		snprintf(buf, 32, "%.0f", v);
		return;
	}
	for (int prec = 1; prec <= 17; prec++) {
		snprintf(buf, 32, "%.*g", prec, v);
		if (strtod(buf, NULL) == v)
			break;
	}
	for (char *p = buf; *p; p++) /* guard against comma locales */
		if (*p == ',')
			*p = '.';
}

static void write_string(sbuf *b, const char *s) {
	sb_puts(b, "\"");
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		char tmp[8];
		switch (*p) {
		case '"': sb_puts(b, "\\\""); break;
		case '\\': sb_puts(b, "\\\\"); break;
		case '\n': sb_puts(b, "\\n"); break;
		case '\r': sb_puts(b, "\\r"); break;
		case '\t': sb_puts(b, "\\t"); break;
		default:
			if (*p < 0x20) {
				snprintf(tmp, sizeof tmp, "\\u%04x", *p);
				sb_puts(b, tmp);
			} else {
				sb_put(b, (const char *)p, 1);
			}
		}
	}
	sb_puts(b, "\"");
}

static int is_scalar_array(const trs_json *j) {
	for (size_t i = 0; i < j->count; i++)
		if (j->items[i]->type == TRS_JSON_ARRAY || j->items[i]->type == TRS_JSON_OBJECT)
			return 0;
	return 1;
}

static void indent(sbuf *b, int depth) {
	for (int i = 0; i < depth; i++)
		sb_puts(b, "  ");
}

static void write_value(sbuf *b, const trs_json *j, int depth, int pretty) {
	char num[32];
	switch (j->type) {
	case TRS_JSON_NULL: sb_puts(b, "null"); break;
	case TRS_JSON_BOOL: sb_puts(b, j->b ? "true" : "false"); break;
	case TRS_JSON_NUMBER:
		trs_json_format_number(j->num, num);
		sb_puts(b, num);
		break;
	case TRS_JSON_STRING: write_string(b, j->str); break;
	case TRS_JSON_ARRAY:
		if (!j->count) {
			sb_puts(b, "[]");
			break;
		}
		if (!pretty || is_scalar_array(j)) { /* short vectors stay on one line */
			sb_puts(b, "[");
			for (size_t i = 0; i < j->count; i++) {
				if (i) sb_puts(b, pretty ? ", " : ",");
				write_value(b, j->items[i], depth + 1, pretty);
			}
			sb_puts(b, "]");
			break;
		}
		sb_puts(b, "[\n");
		for (size_t i = 0; i < j->count; i++) {
			indent(b, depth + 1);
			write_value(b, j->items[i], depth + 1, pretty);
			sb_puts(b, i + 1 < j->count ? ",\n" : "\n");
		}
		indent(b, depth);
		sb_puts(b, "]");
		break;
	case TRS_JSON_OBJECT:
		if (!j->count) {
			sb_puts(b, "{}");
			break;
		}
		sb_puts(b, pretty ? "{\n" : "{");
		for (size_t i = 0; i < j->count; i++) {
			if (pretty) indent(b, depth + 1);
			write_string(b, j->keys[i]);
			sb_puts(b, pretty ? ": " : ":");
			write_value(b, j->items[i], depth + 1, pretty);
			if (i + 1 < j->count) sb_puts(b, ",");
			if (pretty) sb_puts(b, "\n");
		}
		if (pretty) indent(b, depth);
		sb_puts(b, "}");
		break;
	}
}

char *trs_json_write(const trs_json *j) {
	sbuf b = { 0 };
	write_value(&b, j, 0, 1);
	sb_puts(&b, "\n");
	return b.buf;
}

char *trs_json_write_compact(const trs_json *j) {
	sbuf b = { 0 };
	write_value(&b, j, 0, 0);
	return b.buf;
}
