#include "nixlytile.h"
#include "client.h"

void
cec_switch_to_active_source(void)
{
	pid_t pid;

	wlr_log(WLR_INFO, "Switching TV to active source via CEC");

	pid = fork();
	if (pid == 0) {
		/* Child process */
		int fd;

		setsid();
		fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd > 2)
				close(fd);
		}

		/* Send "active source" command via cec-client
		 * "as" = Active Source - tells TV to switch to this input
		 * -s = single command mode
		 * -d 1 = minimal output */
		execlp("sh", "sh", "-c",
			"echo 'as' | cec-client -s -d 1 2>/dev/null || "
			"echo 'tx 4F:82:10:00' | cec-client -s -d 1 2>/dev/null",
			(char *)NULL);
		_exit(0);
	}
	/* Parent doesn't wait - fire and forget */
}

int
bt_is_gamepad_name(const char *name)
{
	int i;

	if (!name || !*name)
		return 0;

	for (i = 0; bt_gamepad_patterns[i]; i++) {
		if (strcasestr(name, bt_gamepad_patterns[i]))
			return 1;
	}
	return 0;
}

int
bt_trust_cb(sd_bus_message *reply, void *userdata, sd_bus_error *error)
{
	char *path = userdata;

	if (sd_bus_error_is_set(error))
		wlr_log(WLR_DEBUG, "Failed to trust %s: %s", path, error->message);
	else
		wlr_log(WLR_DEBUG, "Trusted device: %s", path);

	free(path);
	return 0;
}

void
bt_trust_device(const char *path)
{
	int r;
	char *path_copy;

	if (!bt_bus || !path)
		return;

	path_copy = strdup(path);
	if (!path_copy)
		return;

	r = sd_bus_call_method_async(bt_bus, NULL,
		"org.bluez", path,
		"org.freedesktop.DBus.Properties", "Set",
		bt_trust_cb, path_copy,
		"ssv", "org.bluez.Device1", "Trusted", "b", 1);

	if (r < 0) {
		wlr_log(WLR_DEBUG, "Failed to send trust request for %s: %s",
			path, strerror(-r));
		free(path_copy);
	}
}

int
bt_pair_cb(sd_bus_message *reply, void *userdata, sd_bus_error *error)
{
	char *path = userdata;

	if (sd_bus_error_is_set(error)) {
		/* AlreadyExists means already paired - that's fine */
		if (!strstr(error->message, "AlreadyExists"))
			wlr_log(WLR_DEBUG, "Pairing failed for %s: %s", path ? path : "unknown", error->message);
	} else {
		wlr_log(WLR_INFO, "Paired with: %s", path ? path : "unknown");
	}

	free(path);
	return 0;
}

void
bt_pair_device(const char *path)
{
	int r;
	char *path_copy;

	if (!bt_bus || !path)
		return;

	wlr_log(WLR_INFO, "Pairing with Bluetooth device: %s", path);

	/* First trust the device (quick operation) */
	bt_trust_device(path);

	/* Then pair asynchronously */
	path_copy = strdup(path);
	if (!path_copy)
		return;

	r = sd_bus_call_method_async(bt_bus, NULL,
		"org.bluez", path,
		"org.bluez.Device1", "Pair",
		bt_pair_cb, path_copy, "");

	if (r < 0) {
		wlr_log(WLR_DEBUG, "Failed to start async Pair: %s", strerror(-r));
		free(path_copy);
	}
}

int
bt_connect_cb(sd_bus_message *reply, void *userdata, sd_bus_error *error)
{
	char *path = userdata;

	if (sd_bus_error_is_set(error)) {
		wlr_log(WLR_DEBUG, "Connect failed for %s: %s", path ? path : "unknown", error->message);
	} else {
		wlr_log(WLR_INFO, "Connected to: %s", path ? path : "unknown");
	}

	free(path);
	return 0;
}

void
bt_connect_device(const char *path)
{
	int r;
	char *path_copy;

	if (!bt_bus || !path)
		return;

	wlr_log(WLR_INFO, "Connecting to Bluetooth device: %s", path);

	path_copy = strdup(path);
	if (!path_copy)
		return;

	r = sd_bus_call_method_async(bt_bus, NULL,
		"org.bluez", path,
		"org.bluez.Device1", "Connect",
		bt_connect_cb, path_copy, "");

	if (r < 0) {
		wlr_log(WLR_DEBUG, "Failed to start async Connect: %s", strerror(-r));
		free(path_copy);
	}
}

int
bt_device_signal_cb(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
	const char *path;
	const char *interface;
	int r;

	(void)userdata;
	(void)error;

	/* Get object path */
	path = sd_bus_message_get_path(m);
	if (!path || !strstr(path, "/org/bluez/"))
		return 0;

	/* Parse InterfacesAdded signal */
	r = sd_bus_message_read(m, "s", &interface);
	if (r < 0)
		return 0;

	/* Only interested in Device1 interface */
	if (strcmp(interface, "org.bluez.Device1") != 0)
		return 0;

	/* Enter the properties array */
	r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0)
		return 0;

	/* Look for Name property */
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char *prop_name;
		r = sd_bus_message_read(m, "s", &prop_name);
		if (r < 0)
			break;

		if (strcmp(prop_name, "Name") == 0) {
			r = sd_bus_message_enter_container(m, 'v', "s");
			if (r >= 0) {
				const char *name;
				r = sd_bus_message_read(m, "s", &name);
				if (r >= 0 && bt_is_gamepad_name(name)) {
					wlr_log(WLR_INFO, "Discovered gamepad: %s at %s", name, path);
					bt_pair_device(path);
					bt_connect_device(path);
				}
				sd_bus_message_exit_container(m);
			}
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);

	return 0;
}

int
bt_check_existing_cb(sd_bus_message *reply, void *userdata, sd_bus_error *error)
{
	int r;

	(void)userdata;

	if (sd_bus_error_is_set(error)) {
		wlr_log(WLR_DEBUG, "Failed to get BlueZ objects: %s", error->message);
		return 0;
	}

	/* Parse response: a{oa{sa{sv}}} */
	r = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
	if (r < 0)
		return 0;

	while (sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}") > 0) {
		const char *obj_path;
		r = sd_bus_message_read(reply, "o", &obj_path);
		if (r < 0)
			break;

		/* Enter interfaces dict */
		r = sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
		if (r < 0)
			break;

		while (sd_bus_message_enter_container(reply, 'e', "sa{sv}") > 0) {
			const char *iface;
			r = sd_bus_message_read(reply, "s", &iface);
			if (r < 0)
				break;

			if (strcmp(iface, "org.bluez.Device1") == 0) {
				/* Enter properties dict */
				r = sd_bus_message_enter_container(reply, 'a', "{sv}");
				if (r < 0)
					break;

				const char *name = NULL;
				int connected = 0;
				int paired = 0;

				while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
					const char *prop;
					r = sd_bus_message_read(reply, "s", &prop);
					if (r >= 0) {
						if (strcmp(prop, "Name") == 0) {
							sd_bus_message_enter_container(reply, 'v', "s");
							sd_bus_message_read(reply, "s", &name);
							sd_bus_message_exit_container(reply);
						} else if (strcmp(prop, "Connected") == 0) {
							sd_bus_message_enter_container(reply, 'v', "b");
							sd_bus_message_read(reply, "b", &connected);
							sd_bus_message_exit_container(reply);
						} else if (strcmp(prop, "Paired") == 0) {
							sd_bus_message_enter_container(reply, 'v', "b");
							sd_bus_message_read(reply, "b", &paired);
							sd_bus_message_exit_container(reply);
						} else {
							sd_bus_message_skip(reply, "v");
						}
					}
					sd_bus_message_exit_container(reply);
				}
				sd_bus_message_exit_container(reply);

				/* Auto-connect known gamepads that are paired but not connected */
				if (name && bt_is_gamepad_name(name)) {
					if (!paired) {
						wlr_log(WLR_INFO, "Found unpaired gamepad: %s", name);
						bt_pair_device(obj_path);
						bt_connect_device(obj_path);
					} else if (!connected) {
						wlr_log(WLR_INFO, "Reconnecting paired gamepad: %s", name);
						bt_connect_device(obj_path);
					}
				}
			} else {
				sd_bus_message_skip(reply, "a{sv}");
			}
			sd_bus_message_exit_container(reply);
		}
		sd_bus_message_exit_container(reply);
		sd_bus_message_exit_container(reply);
	}

	return 0;
}

void
bt_check_existing_devices(void)
{
	int r;

	if (!bt_bus)
		return;

	/* Async call to get managed objects */
	r = sd_bus_call_method_async(bt_bus, NULL,
		"org.bluez", "/",
		"org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
		bt_check_existing_cb, NULL, "");

	if (r < 0) {
		wlr_log(WLR_DEBUG, "Failed to start async GetManagedObjects: %s", strerror(-r));
	}
}

int
bt_start_discovery_cb(sd_bus_message *reply, void *userdata, sd_bus_error *error)
{
	(void)reply;
	(void)userdata;

	if (sd_bus_error_is_set(error)) {
		bt_scanning = 0;
		wlr_log(WLR_DEBUG, "StartDiscovery failed: %s", error->message);
		return 0;
	}

	wlr_log(WLR_INFO, "Started Bluetooth discovery for controllers");

	/* Schedule stop after scan duration */
	if (bt_scan_timer)
		wl_event_source_timer_update(bt_scan_timer, BT_SCAN_DURATION_MS);

	return 0;
}

void
bt_start_discovery(void)
{
	int r;

	if (!bt_bus || bt_scanning)
		return;

	/* Set scanning flag early to prevent duplicate calls */
	bt_scanning = 1;

	/* Async call to start discovery */
	r = sd_bus_call_method_async(bt_bus, NULL,
		"org.bluez", "/org/bluez/hci0",
		"org.bluez.Adapter1", "StartDiscovery",
		bt_start_discovery_cb, NULL, "");

	if (r < 0) {
		bt_scanning = 0;
		wlr_log(WLR_DEBUG, "Failed to start async StartDiscovery: %s", strerror(-r));
	}
}

int
bt_stop_discovery_cb(sd_bus_message *reply, void *userdata, sd_bus_error *error)
{
	(void)reply;
	(void)userdata;
	(void)error;

	wlr_log(WLR_DEBUG, "Stopped Bluetooth discovery");
	return 0;
}

void
bt_stop_discovery(void)
{
	int r;

	if (!bt_bus || !bt_scanning)
		return;

	bt_scanning = 0;

	/* Async call to stop discovery - fire and forget */
	r = sd_bus_call_method_async(bt_bus, NULL,
		"org.bluez", "/org/bluez/hci0",
		"org.bluez.Adapter1", "StopDiscovery",
		bt_stop_discovery_cb, NULL, "");

	if (r < 0) {
		wlr_log(WLR_DEBUG, "Failed to start async StopDiscovery: %s", strerror(-r));
	}
}

int
bt_scan_timer_cb(void *data)
{
	(void)data;

	if (bt_scanning) {
		/* Stop discovery and schedule next scan */
		bt_stop_discovery();
		if (bt_scan_timer)
			wl_event_source_timer_update(bt_scan_timer, BT_SCAN_INTERVAL_MS);
	} else {
		/* Check existing devices and start new discovery */
		bt_check_existing_devices();
		bt_start_discovery();
	}

	return 0;
}

int
bt_bus_event_cb(int fd, uint32_t mask, void *data)
{
	sd_bus *bus = data;
	int r;
	int events;
	uint32_t newmask;

	(void)fd;
	if (!bus)
		return 0;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
		return 0;

	/* Process all pending D-Bus messages (non-blocking) */
	while ((r = sd_bus_process(bus, NULL)) > 0)
		;

	/* Update event mask for next iteration */
	events = sd_bus_get_events(bus);
	newmask = 0;
	if (events & SD_BUS_EVENT_READABLE)
		newmask |= WL_EVENT_READABLE;
	if (events & SD_BUS_EVENT_WRITABLE)
		newmask |= WL_EVENT_WRITABLE;
	if (bt_bus_event)
		wl_event_source_fd_update(bt_bus_event, newmask ? newmask : WL_EVENT_READABLE);

	return 0;
}

void
bt_controller_setup(void)
{
	int r;
	int fd, events;
	uint32_t mask;

	/* Check if Bluetooth is available */
	if (!bluetooth_available)
		return;

	/* Connect to system bus */
	r = sd_bus_open_system(&bt_bus);
	if (r < 0) {
		wlr_log(WLR_DEBUG, "Failed to connect to system bus for Bluetooth: %s", strerror(-r));
		return;
	}

	/* Set timeout - still useful for async calls that block internally */
	sd_bus_set_method_call_timeout(bt_bus, 500 * 1000); /* 500ms - reduced from 2s */

	/* Add bt_bus to event loop for async processing */
	fd = sd_bus_get_fd(bt_bus);
	events = sd_bus_get_events(bt_bus);
	mask = 0;
	if (events & SD_BUS_EVENT_READABLE)
		mask |= WL_EVENT_READABLE;
	if (events & SD_BUS_EVENT_WRITABLE)
		mask |= WL_EVENT_WRITABLE;
	if (mask == 0)
		mask = WL_EVENT_READABLE;

	bt_bus_event = wl_event_loop_add_fd(event_loop, fd, mask, bt_bus_event_cb, bt_bus);
	if (!bt_bus_event) {
		wlr_log(WLR_ERROR, "Failed to add Bluetooth bus to event loop");
		sd_bus_unref(bt_bus);
		bt_bus = NULL;
		return;
	}

	/* Add match for new device signals */
	r = sd_bus_match_signal(bt_bus, NULL, "org.bluez", NULL,
		"org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
		bt_device_signal_cb, NULL);

	if (r < 0)
		wlr_log(WLR_DEBUG, "Failed to add BlueZ signal match: %s", strerror(-r));

	/* Create scan timer */
	bt_scan_timer = wl_event_loop_add_timer(event_loop, bt_scan_timer_cb, NULL);

	/* Start initial scan after a short delay */
	if (bt_scan_timer)
		wl_event_source_timer_update(bt_scan_timer, 5000);

	wlr_log(WLR_INFO, "Bluetooth controller auto-pairing initialized");
}

void
bt_controller_cleanup(void)
{
	if (bt_scanning)
		bt_stop_discovery();

	if (bt_scan_timer) {
		wl_event_source_remove(bt_scan_timer);
		bt_scan_timer = NULL;
	}

	if (bt_bus_event) {
		wl_event_source_remove(bt_bus_event);
		bt_bus_event = NULL;
	}

	if (bt_bus) {
		sd_bus_unref(bt_bus);
		bt_bus = NULL;
	}
}

int
bt_disconnect_reply_cb(sd_bus_message *reply, void *userdata, sd_bus_error *error)
{
	char *name = userdata;

	if (sd_bus_error_is_set(error)) {
		wlr_log(WLR_DEBUG, "Async disconnect failed for %s: %s", name ? name : "unknown", error->message);
	} else {
		wlr_log(WLR_INFO, "Controller disconnected: %s", name ? name : "unknown");
	}

	free(name);
	return 0;
}

int
bt_get_objects_disconnect_cb(sd_bus_message *reply, void *userdata, sd_bus_error *error)
{
	char *target_name = userdata;
	const char *obj_path;
	int r;

	if (!target_name)
		return 0;

	if (sd_bus_error_is_set(error)) {
		wlr_log(WLR_DEBUG, "Failed to get Bluetooth objects: %s", error->message);
		free(target_name);
		return 0;
	}

	/* Iterate through objects to find our device */
	r = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
	if (r < 0)
		goto done;

	while ((r = sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}")) > 0) {
		r = sd_bus_message_read(reply, "o", &obj_path);
		if (r < 0)
			break;

		/* Enter interfaces dict */
		r = sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
		if (r < 0)
			break;

		while ((r = sd_bus_message_enter_container(reply, 'e', "sa{sv}")) > 0) {
			const char *interface;
			r = sd_bus_message_read(reply, "s", &interface);
			if (r < 0)
				break;

			if (strcmp(interface, "org.bluez.Device1") == 0) {
				/* Enter properties dict */
				r = sd_bus_message_enter_container(reply, 'a', "{sv}");
				if (r < 0)
					break;

				const char *device_name = NULL;
				int connected = 0;

				while ((r = sd_bus_message_enter_container(reply, 'e', "sv")) > 0) {
					const char *prop;
					r = sd_bus_message_read(reply, "s", &prop);
					if (r < 0)
						break;

					if (strcmp(prop, "Name") == 0) {
						r = sd_bus_message_enter_container(reply, 'v', "s");
						if (r >= 0) {
							sd_bus_message_read(reply, "s", &device_name);
							sd_bus_message_exit_container(reply);
						}
					} else if (strcmp(prop, "Connected") == 0) {
						r = sd_bus_message_enter_container(reply, 'v', "b");
						if (r >= 0) {
							sd_bus_message_read(reply, "b", &connected);
							sd_bus_message_exit_container(reply);
						}
					} else {
						sd_bus_message_skip(reply, "v");
					}
					sd_bus_message_exit_container(reply);
				}
				sd_bus_message_exit_container(reply);

				/* Check if this is our device (match in either direction) */
				if (device_name && connected &&
				    (strcasestr(target_name, device_name) ||
				     strcasestr(device_name, target_name))) {
					wlr_log(WLR_INFO, "Disconnecting Bluetooth controller: %s", device_name);

					/* Async disconnect - fire and forget */
					char *name_copy = strdup(device_name);
					r = sd_bus_call_method_async(bt_bus, NULL,
						"org.bluez",
						obj_path,
						"org.bluez.Device1",
						"Disconnect",
						bt_disconnect_reply_cb,
						name_copy,
						"");

					if (r < 0) {
						wlr_log(WLR_DEBUG, "Failed to start async disconnect: %s", strerror(-r));
						free(name_copy);
					}
					goto done;
				}
			} else {
				sd_bus_message_skip(reply, "a{sv}");
			}
			sd_bus_message_exit_container(reply);
		}
		sd_bus_message_exit_container(reply);
		sd_bus_message_exit_container(reply);
	}

done:
	free(target_name);
	return 0;
}

