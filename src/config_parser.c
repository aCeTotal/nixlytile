/* config_parser.c — minimal KDL-subset parser. */
#include "config_parser.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	const char *src;
	size_t      pos;
	size_t      len;
	int         line;
	char       *err;
} Parser;

static void
set_err(Parser *p, const char *fmt, ...)
{
	if (p->err) return;
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	char out[320];
	snprintf(out, sizeof(out), "line %d: %s", p->line, buf);
	p->err = strdup(out);
}

static int
peek(Parser *p)
{
	return p->pos < p->len ? (unsigned char)p->src[p->pos] : -1;
}

static int
peek_at(Parser *p, size_t off)
{
	return p->pos + off < p->len ? (unsigned char)p->src[p->pos + off] : -1;
}

static int
advance(Parser *p)
{
	if (p->pos >= p->len) return -1;
	int c = (unsigned char)p->src[p->pos++];
	if (c == '\n') p->line++;
	return c;
}

static int
is_ident_start(int c)
{
	return c == '_' || isalpha(c);
}

static int
is_ident_cont(int c)
{
	return c == '_' || c == '-' || isalnum(c);
}

/* Skip whitespace, line continuations, comments. If `nl_significant`, stop
 * before a newline (so the caller can use it as a statement terminator). */
static void
skip_ws(Parser *p, int nl_significant)
{
	for (;;) {
		int c = peek(p);
		if (c < 0) return;
		if (c == ' ' || c == '\t' || c == '\r') {
			advance(p);
		} else if (c == '\\' && peek_at(p, 1) == '\n') {
			advance(p); advance(p);
		} else if (c == '\n') {
			if (nl_significant) return;
			advance(p);
		} else if (c == ';') {
			/* semicolon = node terminator, treat exactly like
			 * newline: leave it for the caller when significant,
			 * else parse_node never sees the terminator and
			 * "a; b" folds b into a's arguments. */
			if (nl_significant) return;
			advance(p);
		} else if (c == '/' && peek_at(p, 1) == '/') {
			while ((c = peek(p)) >= 0 && c != '\n') advance(p);
		} else if (c == '/' && peek_at(p, 1) == '*') {
			advance(p); advance(p);
			int depth = 1;
			while (depth > 0 && (c = peek(p)) >= 0) {
				if (c == '/' && peek_at(p, 1) == '*') { advance(p); advance(p); depth++; }
				else if (c == '*' && peek_at(p, 1) == '/') { advance(p); advance(p); depth--; }
				else advance(p);
			}
		} else {
			return;
		}
	}
}

static char *
parse_string(Parser *p)
{
	if (peek(p) != '"') { set_err(p, "expected '\"'"); return NULL; }
	advance(p);
	size_t cap = 32, n = 0;
	char *buf = malloc(cap);
	if (!buf) { set_err(p, "oom"); return NULL; }
	for (;;) {
		int c = peek(p);
		if (c < 0) { set_err(p, "unterminated string"); free(buf); return NULL; }
		if (c == '"') { advance(p); break; }
		if (c == '\\') {
			advance(p);
			int e = advance(p);
			switch (e) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case '"': c = '"';  break;
			case '\\': c = '\\'; break;
			case '/': c = '/'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case '0': c = '\0'; break;
			default:
				set_err(p, "bad escape \\%c", e);
				free(buf);
				return NULL;
			}
		} else {
			advance(p);
		}
		if (n + 1 >= cap) {
			cap *= 2;
			char *nb = realloc(buf, cap);
			if (!nb) { set_err(p, "oom"); free(buf); return NULL; }
			buf = nb;
		}
		buf[n++] = (char)c;
	}
	buf[n] = '\0';
	return buf;
}

static char *
parse_ident(Parser *p)
{
	int c = peek(p);
	if (!is_ident_start(c)) { set_err(p, "expected identifier"); return NULL; }
	size_t start = p->pos;
	while (is_ident_cont(peek(p))) advance(p);
	size_t n = p->pos - start;
	char *s = malloc(n + 1);
	if (!s) { set_err(p, "oom"); return NULL; }
	memcpy(s, p->src + start, n);
	s[n] = '\0';
	return s;
}

/* Parse a numeric literal into KdlValue.  Supports int, float, hex int. */
static int
parse_number(Parser *p, KdlValue *out)
{
	size_t start = p->pos;
	int c = peek(p);
	int is_float = 0;
	if (c == '+' || c == '-') advance(p);
	if (peek(p) == '0' && (peek_at(p, 1) == 'x' || peek_at(p, 1) == 'X')) {
		advance(p); advance(p);
		while (isxdigit(peek(p)) || peek(p) == '_') advance(p);
		char buf[64];
		size_t n = p->pos - start;
		if (n >= sizeof(buf)) { set_err(p, "number too long"); return 0; }
		size_t j = 0;
		for (size_t i = 0; i < n; i++) if (p->src[start + i] != '_') buf[j++] = p->src[start + i];
		buf[j] = '\0';
		out->kind = KDL_VAL_INT;
		out->u.i = strtol(buf, NULL, 0);
		return 1;
	}
	while (isdigit(peek(p)) || peek(p) == '_') advance(p);
	if (peek(p) == '.') { is_float = 1; advance(p); while (isdigit(peek(p)) || peek(p) == '_') advance(p); }
	if (peek(p) == 'e' || peek(p) == 'E') {
		is_float = 1; advance(p);
		if (peek(p) == '+' || peek(p) == '-') advance(p);
		while (isdigit(peek(p))) advance(p);
	}
	char buf[64];
	size_t n = p->pos - start;
	if (n == 0 || n >= sizeof(buf)) { set_err(p, "bad number"); return 0; }
	size_t j = 0;
	for (size_t i = 0; i < n; i++) if (p->src[start + i] != '_') buf[j++] = p->src[start + i];
	buf[j] = '\0';
	if (is_float) {
		out->kind = KDL_VAL_FLOAT;
		out->u.f = strtod(buf, NULL);
	} else {
		out->kind = KDL_VAL_INT;
		out->u.i = strtol(buf, NULL, 10);
	}
	return 1;
}

/* Parse a single value token. Returns 1 on success. */
static int
parse_value(Parser *p, KdlValue *out)
{
	int c = peek(p);
	if (c == '"') {
		char *s = parse_string(p);
		if (!s) return 0;
		out->kind = KDL_VAL_STRING;
		out->u.s = s;
		return 1;
	}
	if (c == '-' || c == '+' || isdigit(c) || (c == '.' && isdigit(peek_at(p, 1))))
		return parse_number(p, out);
	if (is_ident_start(c)) {
		size_t save = p->pos;
		char *id = parse_ident(p);
		if (!id) return 0;
		if (strcmp(id, "true") == 0)      { out->kind = KDL_VAL_BOOL; out->u.i = 1; free(id); return 1; }
		if (strcmp(id, "false") == 0)     { out->kind = KDL_VAL_BOOL; out->u.i = 0; free(id); return 1; }
		if (strcmp(id, "null") == 0)      { out->kind = KDL_VAL_NULL; out->u.s = NULL; free(id); return 1; }
		/* Bare identifier as value → treat as string. */
		out->kind = KDL_VAL_STRING;
		out->u.s = id;
		(void)save;
		return 1;
	}
	set_err(p, "expected value, got '%c'", c < 0 ? '?' : c);
	return 0;
}

static void
free_value(KdlValue *v)
{
	if (v->kind == KDL_VAL_STRING) free(v->u.s);
	v->kind = KDL_VAL_NULL;
	v->u.s = NULL;
}

static void
free_node_contents(KdlNode *n)
{
	free(n->name);
	for (size_t i = 0; i < n->n_args; i++) free_value(&n->args[i]);
	free(n->args);
	for (size_t i = 0; i < n->n_props; i++) { free(n->props[i].key); free_value(&n->props[i].val); }
	free(n->props);
	for (size_t i = 0; i < n->n_children; i++) free_node_contents(&n->children[i]);
	free(n->children);
}

static int parse_node(Parser *p, KdlNode *out);

static int
parse_nodes_until(Parser *p, int stop_brace, KdlNode **out_arr, size_t *out_n)
{
	KdlNode *arr = NULL;
	size_t n = 0, cap = 0;
	for (;;) {
		skip_ws(p, 0);
		int c = peek(p);
		if (c < 0) {
			if (stop_brace) { set_err(p, "unterminated '{'"); goto fail; }
			break;
		}
		if (c == '}') { if (!stop_brace) { set_err(p, "unexpected '}'"); goto fail; } advance(p); break; }
		if (n == cap) {
			cap = cap ? cap * 2 : 8;
			KdlNode *na = realloc(arr, cap * sizeof(*arr));
			if (!na) { set_err(p, "oom"); goto fail; }
			arr = na;
		}
		memset(&arr[n], 0, sizeof(arr[n]));
		if (!parse_node(p, &arr[n])) goto fail;
		n++;
	}
	*out_arr = arr;
	*out_n = n;
	return 1;
fail:
	for (size_t i = 0; i < n; i++) free_node_contents(&arr[i]);
	free(arr);
	return 0;
}

static int
parse_node(Parser *p, KdlNode *out)
{
	out->line = p->line;
	int c = peek(p);
	char *name;
	if (c == '"') name = parse_string(p);
	else          name = parse_ident(p);
	if (!name) return 0;
	out->name = name;

	for (;;) {
		skip_ws(p, 1);
		int ch = peek(p);
		if (ch < 0 || ch == '\n' || ch == ';') {
			if (ch == '\n' || ch == ';') advance(p);
			return 1;
		}
		if (ch == '{') {
			advance(p);
			if (!parse_nodes_until(p, 1, &out->children, &out->n_children)) return 0;
			skip_ws(p, 1);
			if (peek(p) == '\n' || peek(p) == ';') advance(p);
			return 1;
		}

		/* Try prop=val first if the next identifier is followed by '='. */
		if (is_ident_start(ch) || ch == '"') {
			size_t save = p->pos;
			int save_line = p->line;
			char *maybe;
			if (ch == '"') maybe = parse_string(p);
			else           maybe = parse_ident(p);
			if (!maybe) return 0;
			if (peek(p) == '=') {
				advance(p);
				KdlValue v = {0};
				if (!parse_value(p, &v)) { free(maybe); return 0; }
				{
					void *np = realloc(out->props, (out->n_props + 1) * sizeof(*out->props));
					if (!np) { free(maybe); return 0; }
					out->props = np;
				}
				out->props[out->n_props].key = maybe;
				out->props[out->n_props].val = v;
				out->n_props++;
				continue;
			}
			/* Not a prop; rewind and re-parse as value. We can shortcut: a
			 * bare identifier becomes a string arg. */
			{
				void *na2 = realloc(out->args, (out->n_args + 1) * sizeof(*out->args));
				if (!na2) { free(maybe); return 0; }
				out->args = na2;
			}
			if (ch == '"') {
				out->args[out->n_args].kind = KDL_VAL_STRING;
				out->args[out->n_args].u.s = maybe;
			} else if (strcmp(maybe, "true") == 0) {
				out->args[out->n_args].kind = KDL_VAL_BOOL;
				out->args[out->n_args].u.i = 1;
				free(maybe);
			} else if (strcmp(maybe, "false") == 0) {
				out->args[out->n_args].kind = KDL_VAL_BOOL;
				out->args[out->n_args].u.i = 0;
				free(maybe);
			} else if (strcmp(maybe, "null") == 0) {
				out->args[out->n_args].kind = KDL_VAL_NULL;
				out->args[out->n_args].u.s = NULL;
				free(maybe);
			} else {
				out->args[out->n_args].kind = KDL_VAL_STRING;
				out->args[out->n_args].u.s = maybe;
			}
			out->n_args++;
			(void)save; (void)save_line;
			continue;
		}

		/* Numeric / signed value. */
		KdlValue v = {0};
		if (!parse_value(p, &v)) return 0;
		{
			void *na3 = realloc(out->args, (out->n_args + 1) * sizeof(*out->args));
			if (!na3) return 0;
			out->args = na3;
		}
		out->args[out->n_args++] = v;
	}
}

KdlDoc
kdl_parse(const char *text)
{
	KdlDoc d = {0};
	if (!text) { d.err = strdup("null input"); return d; }
	Parser p = { .src = text, .pos = 0, .len = strlen(text), .line = 1, .err = NULL };
	if (!parse_nodes_until(&p, 0, &d.roots, &d.n_roots)) {
		d.err = p.err ? p.err : strdup("parse failed");
		return d;
	}
	if (p.err) d.err = p.err;
	return d;
}

void
kdl_doc_free(KdlDoc *doc)
{
	if (!doc) return;
	for (size_t i = 0; i < doc->n_roots; i++) free_node_contents(&doc->roots[i]);
	free(doc->roots);
	free(doc->err);
	doc->roots = NULL;
	doc->n_roots = 0;
	doc->err = NULL;
}

const KdlNode *
kdl_find_child(const KdlNode *parent, const char *name)
{
	if (!parent) return NULL;
	for (size_t i = 0; i < parent->n_children; i++)
		if (strcmp(parent->children[i].name, name) == 0)
			return &parent->children[i];
	return NULL;
}

const KdlValue *
kdl_get_prop(const KdlNode *node, const char *key)
{
	if (!node) return NULL;
	for (size_t i = 0; i < node->n_props; i++)
		if (strcmp(node->props[i].key, key) == 0)
			return &node->props[i].val;
	return NULL;
}

int
kdl_arg_string(const KdlNode *n, size_t idx, const char **out)
{
	if (!n || idx >= n->n_args) return 0;
	const KdlValue *v = &n->args[idx];
	if (v->kind != KDL_VAL_STRING) return 0;
	*out = v->u.s;
	return 1;
}

int
kdl_arg_int(const KdlNode *n, size_t idx, long *out)
{
	if (!n || idx >= n->n_args) return 0;
	const KdlValue *v = &n->args[idx];
	if (v->kind == KDL_VAL_INT)   { *out = v->u.i; return 1; }
	if (v->kind == KDL_VAL_FLOAT) { *out = (long)v->u.f; return 1; }
	if (v->kind == KDL_VAL_BOOL)  { *out = v->u.i ? 1 : 0; return 1; }
	return 0;
}

int
kdl_arg_float(const KdlNode *n, size_t idx, double *out)
{
	if (!n || idx >= n->n_args) return 0;
	const KdlValue *v = &n->args[idx];
	if (v->kind == KDL_VAL_FLOAT) { *out = v->u.f; return 1; }
	if (v->kind == KDL_VAL_INT)   { *out = (double)v->u.i; return 1; }
	return 0;
}

int
kdl_arg_bool(const KdlNode *n, size_t idx, int *out)
{
	if (!n || idx >= n->n_args) return 0;
	const KdlValue *v = &n->args[idx];
	if (v->kind == KDL_VAL_BOOL) { *out = v->u.i ? 1 : 0; return 1; }
	if (v->kind == KDL_VAL_INT)  { *out = v->u.i ? 1 : 0; return 1; }
	return 0;
}

int kdl_prop_string(const KdlNode *n, const char *key, const char **out)
{
	const KdlValue *v = kdl_get_prop(n, key);
	if (!v || v->kind != KDL_VAL_STRING) return 0;
	*out = v->u.s;
	return 1;
}

int kdl_prop_int(const KdlNode *n, const char *key, long *out)
{
	const KdlValue *v = kdl_get_prop(n, key);
	if (!v) return 0;
	if (v->kind == KDL_VAL_INT)   { *out = v->u.i; return 1; }
	if (v->kind == KDL_VAL_FLOAT) { *out = (long)v->u.f; return 1; }
	if (v->kind == KDL_VAL_BOOL)  { *out = v->u.i ? 1 : 0; return 1; }
	return 0;
}

int kdl_prop_float(const KdlNode *n, const char *key, double *out)
{
	const KdlValue *v = kdl_get_prop(n, key);
	if (!v) return 0;
	if (v->kind == KDL_VAL_FLOAT) { *out = v->u.f; return 1; }
	if (v->kind == KDL_VAL_INT)   { *out = (double)v->u.i; return 1; }
	return 0;
}

int kdl_prop_bool(const KdlNode *n, const char *key, int *out)
{
	const KdlValue *v = kdl_get_prop(n, key);
	if (!v) return 0;
	if (v->kind == KDL_VAL_BOOL) { *out = v->u.i ? 1 : 0; return 1; }
	if (v->kind == KDL_VAL_INT)  { *out = v->u.i ? 1 : 0; return 1; }
	return 0;
}
