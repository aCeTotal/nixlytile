/* Live link RSSI for connected bluetooth devices, no root needed: the
 * kernel lets unprivileged raw HCI sockets send a small whitelist of
 * commands, Read_RSSI among them.  One socket per adapter; polled every
 * 2s while the bt popup is being rendered (bt_rssi_ping) and torn down
 * 6s after the last ping, so idle cost is zero.  Results are pushed
 * into btmon's model via btmon_set_link_rssi().
 *
 * Note BR/EDR links report RSSI relative to the "golden receive range":
 * 0 there means "signal is fine", not "unknown".
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include "netsys.h"

extern struct wl_event_loop *event_loop;

#define BTPROTO_HCI     1
#define SOL_HCI         0
#define HCI_FILTER      2
#define HCI_EVENT_PKT   0x04
#define HCI_COMMAND_PKT 0x01
#define EVT_CMD_COMPLETE 0x0e
#define OCF_READ_RSSI   0x1405
#define HCIGETCONNLIST  _IOR('H', 212, int)

struct sockaddr_hci {
	sa_family_t hci_family;
	unsigned short hci_dev;
	unsigned short hci_channel;
};

struct hci_filter {
	uint32_t type_mask;
	uint32_t event_mask[2];
	uint16_t opcode;
};

struct hci_conn_info {
	uint16_t handle;
	uint8_t bdaddr[6];
	uint8_t type;
	uint8_t out;
	uint16_t state;
	uint32_t link_mode;
};

struct hci_conn_list_req {
	uint16_t dev_id;
	uint16_t conn_num;
	struct hci_conn_info conn_info[8];
};

#define RSSI_ADAPT_MAX 4
#define RSSI_CONN_MAX  8

typedef struct {
	int dev_id;             /* hciN, -1 = slot free */
	int fd;
	struct wl_event_source *src;
	/* handle -> addr map from the latest conn list */
	uint16_t handle[RSSI_CONN_MAX];
	char addr[RSSI_CONN_MAX][18];
	int nconn;
} RssiSock;

static RssiSock rs[RSSI_ADAPT_MAX];
static struct wl_event_source *rssi_timer;
static uint64_t last_ping_ms;

static uint64_t
rssi_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
sock_close(RssiSock *s)
{
	if (s->src)
		wl_event_source_remove(s->src);
	if (s->fd >= 0)
		close(s->fd);
	memset(s, 0, sizeof(*s));
	s->dev_id = -1;
	s->fd = -1;
}

static int
sock_event(int fd, uint32_t mask, void *data)
{
	RssiSock *s = data;
	unsigned char buf[64];
	ssize_t n;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		sock_close(s);
		return 0;
	}
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		uint16_t opcode, handle;
		int8_t rssi;
		int i;

		/* cmd-complete for Read_RSSI:
		 * 04 0e plen ncmd op_lo op_hi status hnd_lo hnd_hi rssi */
		if (n < 10 || buf[0] != HCI_EVENT_PKT ||
				buf[1] != EVT_CMD_COMPLETE)
			continue;
		opcode = buf[4] | (buf[5] << 8);
		if (opcode != OCF_READ_RSSI || buf[6] != 0)
			continue;
		handle = (buf[7] | (buf[8] << 8)) & 0x0fff;
		rssi = (int8_t)buf[9];
		for (i = 0; i < s->nconn; i++)
			if (s->handle[i] == handle)
				btmon_set_link_rssi(s->addr[i], rssi);
	}
	return 0;
}

static RssiSock *
sock_get(int dev_id)
{
	struct sockaddr_hci a = { AF_BLUETOOTH, 0, 0 };
	struct hci_filter f = { 0 };
	RssiSock *s = NULL;
	int i;

	for (i = 0; i < RSSI_ADAPT_MAX; i++)
		if (rs[i].fd > 0 && rs[i].dev_id == dev_id)
			return &rs[i];
	for (i = 0; i < RSSI_ADAPT_MAX; i++)
		if (rs[i].fd <= 0) {
			s = &rs[i];
			break;
		}
	if (!s)
		return NULL;
	s->fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC,
			BTPROTO_HCI);
	if (s->fd < 0) {
		s->fd = -1;
		return NULL;
	}
	a.hci_dev = (unsigned short)dev_id;
	f.type_mask = 1u << HCI_EVENT_PKT;
	f.event_mask[0] = 1u << EVT_CMD_COMPLETE;
	if (bind(s->fd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
			setsockopt(s->fd, SOL_HCI, HCI_FILTER, &f,
				sizeof(f)) < 0) {
		close(s->fd);
		s->fd = -1;
		return NULL;
	}
	s->src = wl_event_loop_add_fd(event_loop, s->fd, WL_EVENT_READABLE,
			sock_event, s);
	if (!s->src) {
		close(s->fd);
		s->fd = -1;
		return NULL;
	}
	s->dev_id = dev_id;
	return s;
}

static void
poll_adapter(int dev_id)
{
	struct hci_conn_list_req req;
	RssiSock *s = sock_get(dev_id);
	int i;

	if (!s)
		return;
	memset(&req, 0, sizeof(req));
	req.dev_id = (uint16_t)dev_id;
	req.conn_num = RSSI_CONN_MAX;
	if (ioctl(s->fd, HCIGETCONNLIST, &req) < 0)
		return;
	s->nconn = req.conn_num < RSSI_CONN_MAX ? req.conn_num : RSSI_CONN_MAX;
	for (i = 0; i < s->nconn; i++) {
		const uint8_t *b = req.conn_info[i].bdaddr;
		unsigned char cmd[6] = { HCI_COMMAND_PKT,
			OCF_READ_RSSI & 0xff, OCF_READ_RSSI >> 8, 2, 0, 0 };

		s->handle[i] = req.conn_info[i].handle;
		snprintf(s->addr[i], sizeof(s->addr[i]),
				"%02X:%02X:%02X:%02X:%02X:%02X",
				b[5], b[4], b[3], b[2], b[1], b[0]);
		cmd[4] = req.conn_info[i].handle & 0xff;
		cmd[5] = (req.conn_info[i].handle >> 8) & 0x0f;
		if (write(s->fd, cmd, sizeof(cmd)) < 0 && errno == EPERM) {
			/* kernel refuses the command: give up for good */
			sock_close(s);
			return;
		}
	}
}

static int
rssi_tick(void *data)
{
	BtAdapter ads[RSSI_ADAPT_MAX];
	int n, i;

	if (rssi_now_ms() - last_ping_ms > 6000) {
		for (i = 0; i < RSSI_ADAPT_MAX; i++)
			if (rs[i].fd > 0)
				sock_close(&rs[i]);
		return 0;
	}
	n = btmon_adapters(ads, RSSI_ADAPT_MAX);
	for (i = 0; i < n; i++)
		if (ads[i].powered && ads[i].id >= 0)
			poll_adapter(ads[i].id);
	wl_event_source_timer_update(rssi_timer, 2000);
	return 0;
}

void
bt_rssi_ping(void)
{
	int was_idle = rssi_now_ms() - last_ping_ms > 6000;

	last_ping_ms = rssi_now_ms();
	if (!rssi_timer)
		rssi_timer = wl_event_loop_add_timer(event_loop, rssi_tick,
				NULL);
	if (rssi_timer && was_idle)
		wl_event_source_timer_update(rssi_timer, 50);
}
