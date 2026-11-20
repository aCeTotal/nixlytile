/* Loopback listener for OAuth redirects. */
#include <ctype.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "calsync.h"

#define LISTEN_BACKLOG 8
#define WAIT_MS        300000
#define READ_TIMEOUT_S 5
#define REQUEST_MAX    8192
#define PAGE_MAX       2048
#define MS_PER_S       1000
#define NS_PER_MS      1000000
#define PAGE \
	"<!doctype html><html lang=en><meta charset=utf-8>" \
	"<meta name=viewport content=\"width=device-width,initial-scale=1\">" \
	"<title>nixlytile</title><style>:root{color-scheme:dark}" \
	"body{margin:0;min-height:100vh;display:grid;place-items:center;" \
	"background:radial-gradient(circle at 50%% 30%%,#1b1f2a,#0c0e13);" \
	"color:#e6e8ee;font:500 17px/1.5 system-ui,sans-serif}" \
	"main{display:flex;align-items:center;gap:16px;padding:26px 34px;" \
	"border-radius:18px;background:#161a22;border:1px solid #ffffff14;" \
	"box-shadow:0 24px 64px #0009;animation:rise .45s ease-out}" \
	"b{display:grid;place-items:center;width:40px;height:40px;" \
	"border-radius:50%%;background:%s26;color:%s;font-size:21px}" \
	"@keyframes rise{from{opacity:0;transform:translateY(10px)}}</style>" \
	"<main><b>%s</b><span>%s</span></main>"

typedef struct {
	const char *status, *accent, *glyph, *text;
} Page;

static const Page page_ok = { "200 OK", "#5cc28a", "&#10003;",
	"Calendar connected — you can close this tab." };
static const Page page_failed = { "200 OK", "#ef6f6c", "&#10005;",
	"Sign-in failed — you can close this tab." };
static const Page page_stale = { "400 Bad Request", "#f5a35a", "!",
	"This sign-in link is no longer valid." };
static const Page page_missing = { "404 Not Found", "#9aa0a6", "?",
	"Nothing here." };

static int64_t
mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * MS_PER_S + ts.tv_nsec / NS_PER_MS;
}

static void
reply(int fd, const Page *p)
{
	char body[PAGE_MAX];
	int n = snprintf(body, sizeof body, PAGE, p->accent, p->accent,
			p->glyph, p->text);

	dprintf(fd, "HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\n"
			"Content-Length: %d\r\nCache-Control: no-store\r\n"
			"Connection: close\r\n\r\n%s", p->status, n, body);
}

/* Request head through first newline. */
static void
read_request(int fd, char *buf, size_t cap)
{
	struct timeval tv = { .tv_sec = READ_TIMEOUT_S };
	size_t n = 0;
	ssize_t got = 1;

	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	while (got > 0 && n + 1 < cap && !memchr(buf, '\n', n)) {
		got = recv(fd, buf + n, cap - 1 - n, 0);
		n += got > 0 ? (size_t)got : 0;
	}
	buf[n] = 0;
}

/* Query of "GET /", else NULL. */
static const char *
root_query(const char *req)
{
	static const char get[] = "GET /";
	const char *p;

	if (strncmp(req, get, sizeof get - 1))
		return NULL;
	p = req + sizeof get - 1;
	if (*p == '?')
		return p + 1;
	return *p == ' ' ? p : NULL;
}

/* Raw value, "" if absent. */
static const char *
param(const char *q, const char *key)
{
	size_t n = strlen(key);

	while (*q && *q != ' ') {
		if (!strncmp(q, key, n) && q[n] == '=')
			return q + n + 1;
		q += strcspn(q, "& ");
		q += *q == '&';
	}
	return "";
}

static int
hex_pair(const char *s)
{
	char pair[] = { s[0], s[1], 0 };

	if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1]))
		return -1;
	return (int)strtol(pair, NULL, 16);
}

static void
decode(char *dst, size_t len, const char *s)
{
	size_t end = strcspn(s, "& \r\n#");
	size_t n = 0;

	for (size_t i = 0; i < end && n + 1 < len; i++) {
		int byte = s[i] == '%' && i + 2 < end ? hex_pair(s + i + 1) : -1;

		if (byte >= 0) {
			dst[n++] = (char)byte;
			i += 2;
			continue;
		}
		dst[n++] = s[i] == '+' ? ' ' : s[i];
	}
	dst[n] = 0;
}

/* 1 when code or error. */
static int
answer(Loopback *lb, int fd, const char *req)
{
	const char *q = root_query(req);
	char state[sizeof lb->state + 1];

	if (!q) {
		reply(fd, &page_missing);
		return 0;
	}
	decode(state, sizeof state, param(q, "state"));
	if (!strcmp(state, lb->state)) {
		decode(lb->error, sizeof lb->error, param(q, "error"));
		decode(lb->code, sizeof lb->code, param(q, "code"));
	}
	if (!lb->error[0] && !lb->code[0]) {
		reply(fd, &page_stale);
		return 0;
	}
	reply(fd, lb->error[0] ? &page_failed : &page_ok);
	return 1;
}

static int
serve(Loopback *lb, int listener)
{
	char req[REQUEST_MAX];
	int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
	int done;

	if (fd < 0)
		return 0;
	read_request(fd, req, sizeof req);
	done = answer(lb, fd, req);
	close(fd);
	explicit_bzero(req, sizeof req);
	return done;
}

static int
serve_ready(Loopback *lb, const struct pollfd *p)
{
	for (int i = 0; i < lb->nfd; i++)
		if ((p[i].revents & POLLIN) && serve(lb, p[i].fd))
			return 1;
	return 0;
}

int
loopback_wait(Loopback *lb)
{
	struct pollfd p[sizeof lb->fd / sizeof *lb->fd];
	int64_t deadline = mono_ms() + WAIT_MS;
	int64_t left;

	for (int i = 0; i < lb->nfd; i++)
		p[i] = (struct pollfd){ .fd = lb->fd[i], .events = POLLIN };
	while ((left = deadline - mono_ms()) > 0)
		if (poll(p, (nfds_t)lb->nfd, (int)left) > 0 && serve_ready(lb, p))
			return 0;
	return -1;
}

/* Browsers may pick ::1. */
static void
listen_v6(Loopback *lb, in_port_t port)
{
	struct sockaddr_in6 sa = { .sin6_family = AF_INET6, .sin6_port = port,
			.sin6_addr = IN6ADDR_LOOPBACK_INIT };
	int one = 1;
	int fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

	if (fd < 0)
		return;
	setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) || listen(fd, LISTEN_BACKLOG)) {
		close(fd);
		return;
	}
	lb->fd[lb->nfd++] = fd;
}

int
loopback_open(Loopback *lb, const char *host)
{
	struct sockaddr_in sa = { .sin_family = AF_INET,
			.sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t len = sizeof sa;
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

	if (fd < 0)
		return -1;
	lb->fd[lb->nfd++] = fd;
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) || listen(fd, LISTEN_BACKLOG)
			|| getsockname(fd, (struct sockaddr *)&sa, &len))
		return -1;
	snprintf(lb->redirect, sizeof lb->redirect, "http://%s:%u", host,
			ntohs(sa.sin_port));
	if (!strcmp(host, "localhost"))
		listen_v6(lb, sa.sin_port);
	return 0;
}

void
loopback_close(Loopback *lb)
{
	while (lb->nfd > 0)
		close(lb->fd[--lb->nfd]);
}
