/* May a headset use its microphone on the controller it is linked
 * through?  Classic Bluetooth pays for the mic by dropping playback to
 * HFP (telephone mono), and some USB controllers lose the link outright
 * once SCO starts, so the mic is only allowed when controller and
 * headset both speak LE Audio and the card actually landed on
 * bap-duplex — LC3 then carries full quality both ways.  The verdict is
 * pushed to WirePlumber's autoswitch setting, so nothing is configured
 * by hand and a better controller lights the mic up by itself.
 */
#include <stdio.h>
#include <string.h>

#include "netsys.h"
#include "fetch_async.h"

#define BAP_MAX 4

/* Controllers that die on SCO. */
static const struct {
	unsigned vid, pid;
} broken_sco[] = {
	{ 0x0a12, 0x0001 },     /* CSR8510 clones */
};

static struct {
	char addr[18];
	int active;
} bap[BAP_MAX];

/* Cached: ids never change while plugged. */
static int
adapter_sco_broken(int hci_id)
{
	static signed char cache[8];    /* 0 = unknown, 1 = ok, 2 = broken */
	char path[96];
	unsigned vid = 0, pid = 0;
	int bad = 0;
	FILE *f;
	size_t i;

	if (hci_id < 0 || hci_id >= (int)sizeof(cache))
		return 0;
	if (cache[hci_id])
		return cache[hci_id] == 2;
	snprintf(path, sizeof(path),
			"/sys/class/bluetooth/hci%d/device/../idVendor", hci_id);
	if ((f = fopen(path, "re")) != NULL) {
		if (fscanf(f, "%x", &vid) != 1)
			vid = 0;
		fclose(f);
	}
	snprintf(path, sizeof(path),
			"/sys/class/bluetooth/hci%d/device/../idProduct", hci_id);
	if ((f = fopen(path, "re")) != NULL) {
		if (fscanf(f, "%x", &pid) != 1)
			pid = 0;
		fclose(f);
	}
	for (i = 0; i < sizeof(broken_sco) / sizeof(broken_sco[0]); i++)
		if (broken_sco[i].vid == vid && broken_sco[i].pid == pid)
			bad = 1;
	cache[hci_id] = bad ? 2 : 1;
	return bad;
}

int
bt_caps_block(const BtDev *d)
{
	BtAdapter a;

	/* Only a connected mic costs anything. */
	if (!d->connected || !d->has_mic)
		return BT_MIC_OK;
	if (!btmon_adapter_for(d->path, &a))
		return BT_MIC_OK;
	if (adapter_sco_broken(a.id))
		return BT_MIC_BROKEN_SCO;
	if (!a.le_audio)
		return BT_MIC_NO_LE_ADAPTER;
	if (!d->le_audio)
		return BT_MIC_NO_LE_DEVICE;
	return BT_MIC_OK;
}

int
bt_caps_block_addr(const char *addr)
{
	BtDev devs[BT_DEV_MAX];
	int n = btmon_devices(devs, BT_DEV_MAX), i;

	for (i = 0; i < n; i++)
		if (strcmp(devs[i].addr, addr) == 0)
			return bt_caps_block(&devs[i]);
	return BT_MIC_OK;
}

const char *
bt_caps_reason(int block)
{
	switch (block) {
	case BT_MIC_BROKEN_SCO:
		return "This controller crashes on headset mic: mic kept off";
	case BT_MIC_NO_LE_ADAPTER:
		return "Controller has no LE Audio: mic would drop sound to "
		       "call quality";
	case BT_MIC_NO_LE_DEVICE:
		return "Headset has no LE Audio: mic would drop sound to call "
		       "quality";
	}
	return "";
}

static int *
bap_slot(const char *addr)
{
	int i, free_i = -1;

	for (i = 0; i < BAP_MAX; i++) {
		if (strcmp(bap[i].addr, addr) == 0)
			return &bap[i].active;
		if (free_i < 0 && !bap[i].addr[0])
			free_i = i;
	}
	if (free_i < 0)
		return NULL;
	snprintf(bap[free_i].addr, sizeof(bap[free_i].addr), "%s", addr);
	return &bap[free_i].active;
}

void
bt_caps_set_bap(const char *addr, int active)
{
	int *slot = bap_slot(addr);

	if (slot)
		*slot = active;
	bt_caps_apply();
}

void
bt_caps_clear(const char *addr)
{
	int i;

	for (i = 0; i < BAP_MAX; i++)
		if (strcmp(bap[i].addr, addr) == 0)
			memset(&bap[i], 0, sizeof(bap[i]));
}

static int
bap_active(const char *addr)
{
	int i;

	for (i = 0; i < BAP_MAX; i++)
		if (strcmp(bap[i].addr, addr) == 0)
			return bap[i].active;
	return 0;
}

void
bt_caps_apply(void)
{
	static int pushed = -1;
	BtDev devs[BT_DEV_MAX];
	int n = btmon_devices(devs, BT_DEV_MAX), i, allow = 0;

	/* One blocked headset blocks all. */
	for (i = 0; i < n; i++) {
		BtDev *d = &devs[i];

		if (!d->connected || !d->has_mic)
			continue;
		if (bt_caps_block(d) != BT_MIC_OK || !bap_active(d->addr)) {
			allow = 0;
			break;
		}
		allow = 1;
	}
	if (allow == pushed)
		return;
	pushed = allow;
	fetch_async(allow ?
			"wpctl settings bluetooth.autoswitch-to-headset-profile true" :
			"wpctl settings bluetooth.autoswitch-to-headset-profile false",
			NULL, NULL);
}
