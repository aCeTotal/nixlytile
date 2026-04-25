#include "nixlytile.h"

/* Check if a kernel module is loaded via /sys/module/<name> */
static int
validate_kernel_module(const char *module_name)
{
	char path[128];
	snprintf(path, sizeof(path), "/sys/module/%s", module_name);
	return access(path, F_OK) == 0;
}

/* Check if a kernel module parameter has a specific value */
static int
check_module_param(const char *module_name, const char *param, const char *expected)
{
	char path[256], val[64];
	snprintf(path, sizeof(path), "/sys/module/%s/parameters/%s", module_name, param);
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	ssize_t n = read(fd, val, sizeof(val) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	val[n] = '\0';
	while (n > 0 && (val[n-1] == '\n' || val[n-1] == ' '))
		val[--n] = '\0';
	return strcmp(val, expected) == 0;
}

static int gpu_has_connected_display(int card_index);

void
detect_gpus(void)
{
	DIR *dri_dir;
	struct dirent *ent;
	char path[256], link_target[256], driver_link[256];
	ssize_t len;
	int i;

	detected_gpu_count = 0;
	discrete_gpu_idx = -1;
	integrated_gpu_idx = -1;

	dri_dir = opendir("/sys/class/drm");
	if (!dri_dir) {
		wlr_log(WLR_ERROR, "Failed to open /sys/class/drm for GPU detection");
		return;
	}

	while ((ent = readdir(dri_dir)) != NULL && detected_gpu_count < MAX_GPUS) {
		int card_num;
		GpuInfo *gpu;

		/* Only process cardX entries (not renderDX or other entries) */
		if (sscanf(ent->d_name, "card%d", &card_num) != 1)
			continue;
		/* Skip card0-HDMI-A-1 style entries */
		if (strchr(ent->d_name, '-'))
			continue;

		gpu = &detected_gpus[detected_gpu_count];
		memset(gpu, 0, sizeof(*gpu));
		gpu->card_index = card_num;

		snprintf(gpu->card_path, sizeof(gpu->card_path), "/dev/dri/card%d", card_num);

		/* Find corresponding render node */
		snprintf(path, sizeof(path), "/sys/class/drm/card%d/device", card_num);
		DIR *dev_dir = opendir(path);
		if (dev_dir) {
			struct dirent *dev_ent;
			while ((dev_ent = readdir(dev_dir)) != NULL) {
				int render_num;
				if (sscanf(dev_ent->d_name, "drm/renderD%d", &render_num) == 1 ||
				    (strncmp(dev_ent->d_name, "drm", 3) == 0)) {
					/* Check for renderDXXX in drm subdirectory */
					char drm_path[300];
					snprintf(drm_path, sizeof(drm_path), "%s/drm", path);
					DIR *drm_dir = opendir(drm_path);
					if (drm_dir) {
						struct dirent *drm_ent;
						while ((drm_ent = readdir(drm_dir)) != NULL) {
							if (sscanf(drm_ent->d_name, "renderD%d", &render_num) == 1) {
								gpu->render_index = render_num;
								snprintf(gpu->render_path, sizeof(gpu->render_path),
									"/dev/dri/renderD%d", render_num);
								break;
							}
						}
						closedir(drm_dir);
					}
					break;
				}
			}
			closedir(dev_dir);
		}

		/* Get driver name from symlink */
		snprintf(driver_link, sizeof(driver_link),
			"/sys/class/drm/card%d/device/driver", card_num);
		len = readlink(driver_link, link_target, sizeof(link_target) - 1);
		if (len > 0) {
			link_target[len] = '\0';
			const char *driver_name = strrchr(link_target, '/');
			driver_name = driver_name ? driver_name + 1 : link_target;
			snprintf(gpu->driver, sizeof(gpu->driver), "%s", driver_name);
		}

		/* Get PCI slot from uevent */
		snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/uevent", card_num);
		FILE *f = fopen(path, "r");
		if (f) {
			char line[256];
			while (fgets(line, sizeof(line), f)) {
				if (strncmp(line, "PCI_SLOT_NAME=", 14) == 0) {
					char *nl = strchr(line + 14, '\n');
					if (nl) *nl = '\0';
					snprintf(gpu->pci_slot, sizeof(gpu->pci_slot), "%s", line + 14);
					/* Create underscore version for DRI_PRIME */
					snprintf(gpu->pci_slot_underscore, sizeof(gpu->pci_slot_underscore),
						"pci-%s", gpu->pci_slot);
					for (char *p = gpu->pci_slot_underscore; *p; p++) {
						if (*p == ':' || *p == '.') *p = '_';
					}
					break;
				}
			}
			fclose(f);
		}

		/* Determine vendor and if discrete */
		if (strcmp(gpu->driver, "nvidia") == 0) {
			gpu->vendor = GPU_VENDOR_NVIDIA;
			gpu->is_discrete = 1;
		} else if (strcmp(gpu->driver, "nouveau") == 0) {
			gpu->vendor = GPU_VENDOR_NVIDIA;
			gpu->is_discrete = 1;
		} else if (strcmp(gpu->driver, "amdgpu") == 0 || strcmp(gpu->driver, "radeon") == 0) {
			gpu->vendor = GPU_VENDOR_AMD;
			/* Check if integrated (APU) or discrete - look at device class or boot_vga */
			snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/boot_vga", card_num);
			f = fopen(path, "r");
			if (f) {
				int boot_vga = 0;
				if (fscanf(f, "%d", &boot_vga) == 1) {
					/* boot_vga=1 typically means primary/integrated on laptops */
					/* But for AMD, we also check if another GPU exists */
					gpu->is_discrete = !boot_vga;
				}
				fclose(f);
			} else {
				/* If no boot_vga, assume discrete */
				gpu->is_discrete = 1;
			}
		} else if (strcmp(gpu->driver, "i915") == 0 || strcmp(gpu->driver, "xe") == 0) {
			gpu->vendor = GPU_VENDOR_INTEL;
			/* Check boot_vga to distinguish Intel iGPU (HD 630, UHD 770)
			 * from discrete Intel Arc (A770, A750, etc.).
			 * boot_vga=1 → primary/integrated, boot_vga=0 → discrete */
			snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/boot_vga", card_num);
			f = fopen(path, "r");
			if (f) {
				int boot_vga = 0;
				if (fscanf(f, "%d", &boot_vga) == 1)
					gpu->is_discrete = !boot_vga;
				fclose(f);
			}
		} else {
			gpu->vendor = GPU_VENDOR_UNKNOWN;
			gpu->is_discrete = 0;
		}

		wlr_log(WLR_INFO, "GPU %d: %s [%s] driver=%s discrete=%d pci=%s",
			detected_gpu_count, gpu->card_path, gpu->render_path,
			gpu->driver, gpu->is_discrete, gpu->pci_slot);

		/* Validate render node exists and is accessible */
		if (gpu->render_path[0]) {
			if (access(gpu->render_path, F_OK) != 0) {
				wlr_log(WLR_ERROR,
					"GPU %d: render node %s does NOT exist — "
					"check that kernel module '%s' is loaded correctly",
					detected_gpu_count, gpu->render_path, gpu->driver);
			} else if (access(gpu->render_path, R_OK | W_OK) != 0) {
				wlr_log(WLR_ERROR,
					"GPU %d: render node %s is not accessible (permission denied) — "
					"add your user to the 'render' and/or 'video' group: "
					"sudo usermod -aG render,video $USER",
					detected_gpu_count, gpu->render_path);
			}
		} else {
			wlr_log(WLR_ERROR,
				"GPU %d: no render node found for %s — "
				"check that kernel module '%s' is loaded and "
				"/dev/dri/renderD* devices exist",
				detected_gpu_count, gpu->card_path, gpu->driver);
		}

		/* Validate card node accessibility */
		if (access(gpu->card_path, F_OK) == 0 &&
		    access(gpu->card_path, R_OK | W_OK) != 0) {
			wlr_log(WLR_ERROR,
				"GPU %d: card node %s is not accessible (permission denied) — "
				"check that a session manager (logind/seatd) is running and "
				"your user is in the 'video' group",
				detected_gpu_count, gpu->card_path);
		}

		detected_gpu_count++;
	}
	closedir(dri_dir);

	/* Find best discrete and integrated GPU */
	for (i = 0; i < detected_gpu_count; i++) {
		GpuInfo *gpu = &detected_gpus[i];
		if (gpu->is_discrete) {
			/* Prefer Nvidia > AMD for discrete */
			if (discrete_gpu_idx < 0 ||
			    (gpu->vendor == GPU_VENDOR_NVIDIA && detected_gpus[discrete_gpu_idx].vendor != GPU_VENDOR_NVIDIA)) {
				discrete_gpu_idx = i;
			}
		} else {
			if (integrated_gpu_idx < 0)
				integrated_gpu_idx = i;
		}
	}

	/* If AMD GPU was marked as not-discrete but no other integrated exists, it might be wrong */
	if (discrete_gpu_idx < 0 && detected_gpu_count > 1) {
		/* Multiple GPUs but none marked discrete - pick non-Intel as discrete */
		for (i = 0; i < detected_gpu_count; i++) {
			if (detected_gpus[i].vendor != GPU_VENDOR_INTEL) {
				discrete_gpu_idx = i;
				detected_gpus[i].is_discrete = 1;
				break;
			}
		}
	}

	if (discrete_gpu_idx >= 0) {
		GpuInfo *dgpu = &detected_gpus[discrete_gpu_idx];
		wlr_log(WLR_INFO, "Selected discrete GPU: card%d (%s) driver=%s",
			dgpu->card_index, dgpu->pci_slot, dgpu->driver);
	} else {
		wlr_log(WLR_INFO, "No discrete GPU detected");
	}

	/*
	 * Prevent dGPU D3cold/runtime-suspend permanently.
	 *
	 * Three layers of protection against the GPU switching to integrated:
	 *
	 * 1. Set power/control=on + autosuspend_delay_ms=-1 on the GPU and ALL
	 *    sibling PCI functions (audio, USB-C, etc).  This disables the
	 *    kernel's runtime PM for the PCI device.
	 *
	 * 2. Hold an open fd on the dGPU's render node (/dev/dri/renderDXXX)
	 *    for the compositor's entire lifetime.  This keeps the GPU "in use"
	 *    from the driver's perspective, preventing NVIDIA's internal dynamic
	 *    power management from powering off the GPU even if something
	 *    external (nvidia-powerd, udev, power-profiles-daemon, tlp)
	 *    re-enables runtime PM in sysfs.
	 *
	 * 3. A periodic watchdog timer (dgpu_power_watchdog_tick) re-asserts
	 *    the sysfs power settings every 10 seconds, counteracting any
	 *    external service that overrides them.
	 */
	if (discrete_gpu_idx >= 0) {
		GpuInfo *dgpu = &detected_gpus[discrete_gpu_idx];

		dgpu_assert_power_on(dgpu);

		/*
		 * Hold the render node open — prevents the driver from
		 * considering the GPU idle and triggering D3cold.
		 */
		if (dgpu->render_path[0]) {
			dgpu_render_fd = open(dgpu->render_path, O_RDWR);
			if (dgpu_render_fd >= 0) {
				wlr_log(WLR_INFO, "dGPU: holding %s open (fd=%d) to prevent D3cold",
					dgpu->render_path, dgpu_render_fd);
			} else {
				wlr_log(WLR_ERROR, "dGPU: failed to open %s: %s",
					dgpu->render_path, strerror(errno));
			}
		}

		/*
		 * Check NVreg_DynamicPowerManagement — if set to 0x02 (fine-grained),
		 * the NVIDIA driver has its own idle timeout that can override sysfs.
		 * This is a boot-time parameter and cannot be changed at runtime.
		 */
		if (dgpu->vendor == GPU_VENDOR_NVIDIA) {
			/* CPU cursor buffer handles HW cursor plane via dumb DRM
			 * buffers, bypassing broken GBM allocation.  No need to
			 * force software cursors anymore. */

			/* GBM_BACKEND=nvidia-drm: only set when the compositor itself
			 * renders on Nvidia (single-GPU mode).  In hybrid mode the
			 * compositor renders on the Intel iGPU — setting GBM_BACKEND
			 * here would be wrong (Intel FD + Nvidia GBM backend) AND
			 * the var is inherited by ALL child processes, causing
			 * non-dgpu clients to mis-allocate buffers on Nvidia →
			 * cross-GPU DMA-BUF import failures (invisible windows).
			 * For child processes that need the dGPU, set_dgpu_env()
			 * already sets GBM_BACKEND=nvidia-drm per-process. */
			if (integrated_gpu_idx < 0)
				setenv("GBM_BACKEND", "nvidia-drm", 0);

			/* Detect Nvidia driver version for explicit sync support.
			 * Driver 555+ is required for DRM syncobj (explicit sync)
			 * which eliminates Xwayland flickering.  Older drivers
			 * also need legacy DRM modesetting as a fallback. */
			{
				char nv_ver[32] = "";
				int ver_fd = open("/sys/module/nvidia/version", O_RDONLY);
				if (ver_fd >= 0) {
					ssize_t n = read(ver_fd, nv_ver, sizeof(nv_ver) - 1);
					close(ver_fd);
					if (n > 0) {
						nv_ver[n] = '\0';
						while (n > 0 && (nv_ver[n-1] == '\n' || nv_ver[n-1] == ' '))
							nv_ver[--n] = '\0';
						int major = atoi(nv_ver);
						dgpu->driver_version = major;
						wlr_log(WLR_INFO, "NVIDIA: driver version %s (major=%d)", nv_ver, major);
						if (major > 0 && major < 555) {
							wlr_log(WLR_ERROR,
								"NVIDIA: driver %s is too old for explicit sync. "
								"Xwayland WILL flicker. Upgrade to 555+ for proper "
								"Wayland/Xwayland support.", nv_ver);
							/* Only force legacy DRM when NVIDIA actually
							 * serves displays.  When iGPU handles all
							 * displays these would degrade the iGPU path. */
							if (gpu_has_connected_display(dgpu->card_index) ||
							    integrated_gpu_idx < 0) {
								setenv("WLR_DRM_NO_ATOMIC", "1", 0);
								setenv("WLR_DRM_NO_MODIFIERS", "1", 0);
							} else {
								wlr_log(WLR_INFO,
									"NVIDIA: skipping WLR_DRM_NO_ATOMIC/NO_MODIFIERS — "
									"iGPU handles displays, no NVIDIA DRM involvement");
							}
						}
					}
				} else if (strcmp(dgpu->driver, "nvidia") == 0) {
					wlr_log(WLR_ERROR,
						"NVIDIA: could not read driver version from "
						"/sys/module/nvidia/version — ensure nvidia-drm.modeset=1 "
						"kernel parameter is set");
				}
			}

			/* nvidia-drm.fbdev check — improves scanout performance.
			 * Auto-enabled in driver 570+, needs manual set for 555-569. */
			if (dgpu->driver_version >= 555 && dgpu->driver_version < 570) {
				if (!check_module_param("nvidia_drm", "fbdev", "Y") &&
				    !check_module_param("nvidia_drm", "fbdev", "1")) {
					struct utsname uts;
					if (uname(&uts) == 0) {
						int kmaj = 0, kmin = 0;
						sscanf(uts.release, "%d.%d", &kmaj, &kmin);
						if (kmaj > 6 || (kmaj == 6 && kmin >= 11)) {
							wlr_log(WLR_ERROR,
								"NVIDIA: nvidia-drm.fbdev=1 recommended on kernel %d.%d "
								"(driver %d). Add 'nvidia-drm.fbdev=1' to kernel parameters "
								"for better scanout performance.",
								kmaj, kmin, dgpu->driver_version);
						}
					}
				}
			}

			/* GSP firmware status — informational for diagnostics */
			{
				char gsp_val[16] = "";
				int gsp_fd = open("/sys/module/nvidia/parameters/NVreg_EnableGpuFirmware", O_RDONLY);
				if (gsp_fd >= 0) {
					ssize_t gn = read(gsp_fd, gsp_val, sizeof(gsp_val) - 1);
					close(gsp_fd);
					if (gn > 0) {
						gsp_val[gn] = '\0';
						while (gn > 0 && (gsp_val[gn-1] == '\n' || gsp_val[gn-1] == ' '))
							gsp_val[--gn] = '\0';
						if (strcmp(gsp_val, "1") == 0 || strcmp(gsp_val, "Y") == 0) {
							wlr_log(WLR_INFO, "NVIDIA: GSP firmware enabled (good)");
						} else if (dgpu->driver_version >= 560) {
							wlr_log(WLR_INFO,
								"NVIDIA: GSP firmware disabled (NVreg_EnableGpuFirmware=%s). "
								"GSP is recommended for driver %d+ for improved performance "
								"and power management.", gsp_val, dgpu->driver_version);
						} else {
							wlr_log(WLR_INFO, "NVIDIA: GSP firmware status: %s", gsp_val);
						}
					}
				}
			}

			/* NVreg_PreserveVideoMemoryAllocations — suspend/resume support */
			{
				char pvm_val[16] = "";
				int pvm_fd = open("/sys/module/nvidia/parameters/NVreg_PreserveVideoMemoryAllocations", O_RDONLY);
				if (pvm_fd >= 0) {
					ssize_t pn = read(pvm_fd, pvm_val, sizeof(pvm_val) - 1);
					close(pvm_fd);
					if (pn > 0) {
						pvm_val[pn] = '\0';
						while (pn > 0 && (pvm_val[pn-1] == '\n' || pvm_val[pn-1] == ' '))
							pvm_val[--pn] = '\0';
						if (strcmp(pvm_val, "1") == 0 || strcmp(pvm_val, "Y") == 0) {
							wlr_log(WLR_INFO,
								"NVIDIA: NVreg_PreserveVideoMemoryAllocations=1 (suspend/resume safe)");
						} else {
							wlr_log(WLR_INFO,
								"NVIDIA: NVreg_PreserveVideoMemoryAllocations=%s — "
								"set to 1 and enable nvidia-suspend/resume systemd services "
								"for proper suspend/resume support.", pvm_val);
						}
					}
				}
			}

			char dpm_val[16] = "";
			int dpm_fd = open("/sys/module/nvidia/parameters/NVreg_DynamicPowerManagement", O_RDONLY);
			if (dpm_fd >= 0) {
				ssize_t n = read(dpm_fd, dpm_val, sizeof(dpm_val) - 1);
				close(dpm_fd);
				if (n > 0) {
					dpm_val[n] = '\0';
					while (n > 0 && (dpm_val[n-1] == '\n' || dpm_val[n-1] == ' '))
						dpm_val[--n] = '\0';
					if (strstr(dpm_val, "2") || strstr(dpm_val, "0x02")) {
						wlr_log(WLR_ERROR,
							"NVIDIA: WARNING — NVreg_DynamicPowerManagement=%s (fine-grained). "
							"This allows the driver to independently power off the dGPU. "
							"Set nvidia.NVreg_DynamicPowerManagement=0x00 in boot params to disable.",
							dpm_val);
					} else {
						wlr_log(WLR_INFO, "NVIDIA: NVreg_DynamicPowerManagement=%s", dpm_val);
					}
				}
			}
		}

		wlr_log(WLR_INFO, "dGPU: D3cold prevention active — power/control=on + render node held open");
	}

	/* Vendor-specific kernel module validation */
	for (i = 0; i < detected_gpu_count; i++) {
		GpuInfo *gpu = &detected_gpus[i];
		switch (gpu->vendor) {
		case GPU_VENDOR_NVIDIA:
			if (!validate_kernel_module("nvidia_drm")) {
				wlr_log(WLR_ERROR,
					"GPU %d (NVIDIA): nvidia_drm kernel module is NOT loaded — "
					"DRM/KMS will not work. Load it with: "
					"modprobe nvidia_drm modeset=1",
					gpu->card_index);
			} else if (!check_module_param("nvidia_drm", "modeset", "Y") &&
			           !check_module_param("nvidia_drm", "modeset", "1")) {
				wlr_log(WLR_ERROR,
					"GPU %d (NVIDIA): nvidia_drm.modeset is NOT enabled — "
					"Wayland requires modesetting. Add "
					"'nvidia-drm.modeset=1' to kernel parameters",
					gpu->card_index);
			}
			break;
		case GPU_VENDOR_INTEL:
			if (!validate_kernel_module("i915") && !validate_kernel_module("xe")) {
				wlr_log(WLR_ERROR,
					"GPU %d (Intel): neither i915 nor xe kernel module is loaded — "
					"Intel GPU will not function. Check kernel config or "
					"load module: modprobe i915",
					gpu->card_index);
			}
			break;
		case GPU_VENDOR_AMD:
			if (!validate_kernel_module("amdgpu")) {
				wlr_log(WLR_ERROR,
					"GPU %d (AMD): amdgpu kernel module is NOT loaded — "
					"AMD GPU will not function. Load it with: "
					"modprobe amdgpu",
					gpu->card_index);
			}
			break;
		default:
			break;
		}
	}
}

/*
 * Check if a DRM card has any connected display (connector status).
 * Scans /sys/class/drm/cardX-* entries for "connected" status.
 */
static int
gpu_has_connected_display(int card_index)
{
	DIR *drm_dir;
	struct dirent *ent;
	char prefix[16];

	snprintf(prefix, sizeof(prefix), "card%d-", card_index);

	drm_dir = opendir("/sys/class/drm");
	if (!drm_dir)
		return 0;

	while ((ent = readdir(drm_dir)) != NULL) {
		char status_path[300], status[32];
		FILE *f;

		if (strncmp(ent->d_name, prefix, strlen(prefix)) != 0)
			continue;

		snprintf(status_path, sizeof(status_path),
			"/sys/class/drm/%s/status", ent->d_name);
		f = fopen(status_path, "r");
		if (!f)
			continue;
		if (fgets(status, sizeof(status), f)) {
			char *nl = strchr(status, '\n');
			if (nl) *nl = '\0';
			if (strcmp(status, "connected") == 0) {
				fclose(f);
				closedir(drm_dir);
				return 1;
			}
		}
		fclose(f);
	}
	closedir(drm_dir);
	return 0;
}

/*
 * Exclude integrated GPUs from wlroots when they have no connected display.
 *
 * Sets WLR_DRM_DEVICES to include only GPUs that either:
 *   - are discrete (dGPU), or
 *   - have at least one connected display
 *
 * Must be called after detect_gpus() and before wlr_backend_autocreate().
 * When iGPU has a display connected (e.g. motherboard output), it is kept
 * and gets full control — no conflicts with dGPU.
 */
void
filter_igpu_without_display(void)
{
	int i;
	int gpus_with_display = 0;

	if (detected_gpu_count <= 1)
		return;

	/* Don't override explicit user setting */
	if (getenv("WLR_DRM_DEVICES"))
		return;

	/* Count GPUs that have at least one connected display */
	for (i = 0; i < detected_gpu_count; i++) {
		if (gpu_has_connected_display(detected_gpus[i].card_index))
			gpus_with_display++;
	}

	/* If all or none have displays, don't filter */
	if (gpus_with_display == 0 || gpus_with_display == detected_gpu_count)
		return;

	/* Build WLR_DRM_DEVICES including only GPUs with connected displays.
	 * This ensures:
	 *   - iGPU without display → excluded (dGPU handles all screens)
	 *   - dGPU without display → excluded (iGPU has full control, no conflicts)
	 *   - Both with displays   → no filtering (handled above) */
	char devices[512];
	devices[0] = '\0';

	for (i = 0; i < detected_gpu_count; i++) {
		if (!gpu_has_connected_display(detected_gpus[i].card_index)) {
			wlr_log(WLR_INFO,
				"GPU card%d (%s, %s) has no connected display — "
				"excluding from wlroots backend",
				detected_gpus[i].card_index, detected_gpus[i].driver,
				detected_gpus[i].is_discrete ? "dGPU" : "iGPU");
			continue;
		}

		if (devices[0] != '\0')
			strncat(devices, ":", sizeof(devices) - strlen(devices) - 1);
		strncat(devices, detected_gpus[i].card_path,
			sizeof(devices) - strlen(devices) - 1);
	}

	if (devices[0] != '\0') {
		setenv("WLR_DRM_DEVICES", devices, 1);
		wlr_log(WLR_INFO, "WLR_DRM_DEVICES=%s (GPUs without display excluded)",
			devices);
	}
}

/*
 * Assert power/control=on on the dGPU and all its PCI siblings.
 * Called at startup and periodically by the watchdog timer.
 */
void
dgpu_assert_power_on(GpuInfo *gpu)
{
	char pci_path[128];
	int fd;

	if (!gpu || !gpu->pci_slot[0])
		return;

	/* Main GPU function */
	snprintf(pci_path, sizeof(pci_path),
		"/sys/bus/pci/devices/%s/power/control", gpu->pci_slot);
	fd = open(pci_path, O_WRONLY);
	if (fd >= 0) {
		write(fd, "on", 2);
		close(fd);
	}
	snprintf(pci_path, sizeof(pci_path),
		"/sys/bus/pci/devices/%s/power/autosuspend_delay_ms", gpu->pci_slot);
	fd = open(pci_path, O_WRONLY);
	if (fd >= 0) {
		write(fd, "-1", 2);
		close(fd);
	}

	/* All sibling PCI functions (.1 audio, .2 USB-C, etc) */
	char slot_prefix[64];
	strncpy(slot_prefix, gpu->pci_slot, sizeof(slot_prefix) - 1);
	slot_prefix[sizeof(slot_prefix) - 1] = '\0';
	char *dot = strrchr(slot_prefix, '.');
	if (dot) {
		*dot = '\0';
		for (int fn = 1; fn <= 7; fn++) {
			snprintf(pci_path, sizeof(pci_path),
				"/sys/bus/pci/devices/%s.%d/power/control", slot_prefix, fn);
			fd = open(pci_path, O_WRONLY);
			if (fd >= 0) {
				write(fd, "on", 2);
				close(fd);
			}
			snprintf(pci_path, sizeof(pci_path),
				"/sys/bus/pci/devices/%s.%d/power/autosuspend_delay_ms", slot_prefix, fn);
			fd = open(pci_path, O_WRONLY);
			if (fd >= 0) {
				write(fd, "-1", 2);
				close(fd);
			}
		}
	}
}

/*
 * Watchdog timer callback — re-assert dGPU power/control=on every 60s.
 * Counteracts nvidia-powerd, power-profiles-daemon, tlp, udev rules,
 * or any other service that periodically re-enables runtime PM.
 */
static int
dgpu_power_watchdog_tick(void *data)
{
	(void)data;

	if (discrete_gpu_idx >= 0 && discrete_gpu_idx < detected_gpu_count) {
		GpuInfo *dgpu = &detected_gpus[discrete_gpu_idx];
		dgpu_assert_power_on(dgpu);

		/* Verify the render node fd is still open */
		if (dgpu_render_fd >= 0 && fcntl(dgpu_render_fd, F_GETFD) < 0) {
			wlr_log(WLR_ERROR, "dGPU: render node fd lost, re-opening %s",
				dgpu->render_path);
			dgpu_render_fd = open(dgpu->render_path, O_RDWR);
		}
	}

	/* Re-arm timer */
	if (dgpu_power_watchdog)
		wl_event_source_timer_update(dgpu_power_watchdog, 60000);
	return 0;
}

void
dgpu_power_watchdog_start(void)
{
	if (discrete_gpu_idx < 0 || dgpu_power_watchdog)
		return;
	dgpu_power_watchdog = wl_event_loop_add_timer(event_loop,
		dgpu_power_watchdog_tick, NULL);
	if (dgpu_power_watchdog)
		wl_event_source_timer_update(dgpu_power_watchdog, 60000);
}

int
should_use_dgpu(const char *cmd)
{
	const char *base;
	int i;

	if (!cmd)
		return 0;

	/* Get basename of command */
	base = strrchr(cmd, '/');
	base = base ? base + 1 : cmd;

	for (i = 0; dgpu_programs[i]; i++) {
		if (strcasecmp(base, dgpu_programs[i]) == 0)
			return 1;
		/* Also check if command contains the program name (for wrappers) */
		if (strcasestr(cmd, dgpu_programs[i]))
			return 1;
	}
	return 0;
}

void
set_dgpu_env(void)
{
	GpuInfo *dgpu;

	/* No discrete GPU detected - nothing to do */
	if (discrete_gpu_idx < 0 || discrete_gpu_idx >= detected_gpu_count)
		return;

	dgpu = &detected_gpus[discrete_gpu_idx];

	switch (dgpu->vendor) {
	case GPU_VENDOR_NVIDIA:
		/* GLX vendor — always needed so GLX uses NVIDIA */
		setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 1);
		setenv("__VK_LAYER_NV_optimus", "NVIDIA_only", 1);

		/* GBM backend — helps libgbm find NVIDIA's implementation */
		setenv("GBM_BACKEND", "nvidia-drm", 0);

		/* PRIME render offload — ONLY for hybrid (iGPU + dGPU) systems.
		 * On NVIDIA-only, these cause Xwayland EGL init to fail. */
		if (integrated_gpu_idx >= 0) {
			setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 1);
			setenv("__NV_PRIME_RENDER_OFFLOAD_PROVIDER", "NVIDIA-G0", 1);
			if (dgpu->pci_slot_underscore[0])
				setenv("DRI_PRIME", dgpu->pci_slot_underscore, 1);
			else
				setenv("DRI_PRIME", "1", 1);

			/* Force Qt/GTK apps through Xwayland when PRIME offload
			 * is active.  Qt6's wayland-egl plugin cannot create a
			 * valid EGLConfig with the NVIDIA PRIME env vars, causing
			 * a SIGSEGV in QEGLPlatformContext (FreeCAD, KiCad, etc.).
			 * Games don't use QT_QPA_PLATFORM, so this is harmless
			 * for them — they go via Xwayland/gamescope anyway. */
			setenv("QT_QPA_PLATFORM", "xcb", 1);
			setenv("GDK_BACKEND", "x11", 1);

			wlr_log(WLR_INFO, "NVIDIA: hybrid mode (iGPU + dGPU), PRIME offload enabled");
		} else {
			wlr_log(WLR_INFO, "NVIDIA: single-GPU mode, PRIME offload skipped");
		}

		/* DRI_PRIME for nouveau (doesn't use PRIME offload vars) */
		if (strcmp(dgpu->driver, "nouveau") == 0 && integrated_gpu_idx >= 0) {
			if (dgpu->pci_slot_underscore[0])
				setenv("DRI_PRIME", dgpu->pci_slot_underscore, 1);
			else
				setenv("DRI_PRIME", "1", 1);
		}

		/*
		 * NVIDIA frame pacing & performance environment variables.
		 * These are critical for optimal game performance under Wayland.
		 */

		/* Limit pre-rendered frames to 1 — minimizes input latency.
		 * The compositor handles vsync/frame pacing, so deep queuing
		 * only adds lag without benefit. */
		setenv("__GL_MaxFramesAllowed", "1", 0);

		/* Disable driver-side vsync — the Wayland compositor handles
		 * presentation timing. Double-vsync causes input lag. */
		setenv("__GL_SYNC_TO_VBLANK", "0", 0);

		/* Allow G-Sync/VRR — the compositor enables adaptive sync
		 * for fullscreen games automatically. */
		setenv("__GL_GSYNC_ALLOWED", "1", 0);
		setenv("__GL_VRR_ALLOWED", "1", 0);

		/* Don't yield CPU while waiting for GPU — reduces latency
		 * at the cost of slightly higher CPU usage. */
		setenv("__GL_YIELD", "NOTHING", 0);

		/* Enable threaded OpenGL optimizations — offloads GL work
		 * to a separate thread for better CPU utilization. */
		setenv("__GL_THREADED_OPTIMIZATIONS", "1", 0);

		/* Persistent shader cache — avoid re-compilation stutters.
		 * Don't clean up old shaders to keep cache warm. */
		setenv("__GL_SHADER_DISK_CACHE", "1", 0);
		setenv("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP", "1", 0);

		/* Use direct rendering for lowest latency (skip composition queue) */
		setenv("__GL_ALLOW_UNOFFICIAL_PROTOCOL", "1", 0);

		/* Xwayland / EGL session type — ensures X11 and Wayland clients
		 * correctly identify the session as Wayland. */
		setenv("XDG_SESSION_TYPE", "wayland", 0);

		/* VA-API hardware video decode via Nvidia */
		setenv("LIBVA_DRIVER_NAME", "nvidia", 0);
		setenv("NVD_BACKEND", "direct", 0);
		setenv("VDPAU_DRIVER", "nvidia", 0);

		/* Ensure direct rendering (not indirect/software) */
		setenv("LIBGL_ALWAYS_INDIRECT", "0", 0);

		/* Nvidia experimental performance strategy */
		setenv("__GL_ExperimentalPerfStrategy", "1", 0);

		/* Allow FXAA if app requests it */
		setenv("__GL_ALLOW_FXAA_USAGE", "1", 0);

		/* Nvidia image sharpening — let apps control this */
		setenv("__GL_SHARPEN_ENABLE", "0", 0);
		setenv("__GL_SHARPEN_VALUE", "0", 0);
		setenv("__GL_SHARPEN_IGNORE_FILM_GRAIN", "0", 0);

		/* Version-based diagnostics */
		if (dgpu->driver_version > 0 && dgpu->driver_version < 570) {
			wlr_log(WLR_INFO,
				"NVIDIA: driver %d < 570 — consider adding 'nvidia-drm.fbdev=1' "
				"to kernel parameters for better scanout performance.",
				dgpu->driver_version);
		}
		if (dgpu->driver_version >= 565) {
			wlr_log(WLR_INFO,
				"NVIDIA: driver %d — GLX_EXT_buffer_age re-enabled (good for compositors)",
				dgpu->driver_version);
		}

		wlr_log(WLR_INFO,
			"NVIDIA: compositor env set — GBM_BACKEND=nvidia-drm, "
			"__GLX_VENDOR_LIBRARY_NAME=nvidia (CPU cursor buffer for HW cursor)"
			"%s",
			dgpu->driver_version > 0 && dgpu->driver_version < 555
				? ", WLR_DRM_NO_MODIFIERS=1 (old driver)" : "");

		break;

	case GPU_VENDOR_AMD:
		/* DRI_PRIME with specific PCI device for AMD */
		if (dgpu->pci_slot_underscore[0])
			setenv("DRI_PRIME", dgpu->pci_slot_underscore, 1);
		else
			setenv("DRI_PRIME", "1", 1);
		/* AMD Vulkan driver selection */
		setenv("AMD_VULKAN_ICD", "RADV", 1);
		break;

	case GPU_VENDOR_INTEL:
	case GPU_VENDOR_UNKNOWN:
	default:
		/* Generic DRI_PRIME for other cases */
		if (dgpu->pci_slot_underscore[0])
			setenv("DRI_PRIME", dgpu->pci_slot_underscore, 1);
		else
			setenv("DRI_PRIME", "1", 1);
		break;
	}

	/* Vulkan device selection - prefer discrete GPU.
	 *
	 * Multiple layers of selection to guarantee dGPU usage:
	 *
	 * 1) VK_LOADER_DRIVERS_SELECT — modern Vulkan loader (1.3.234+)
	 *    filters ICD files by filename glob.
	 *
	 * 2) MESA_VK_DEVICE_SELECT — Mesa's VkLayer_MESA_device_select
	 *    implicit layer, selects device by PCI vendor:device ID.
	 *    Without this, the layer defaults to boot_vga (= iGPU).
	 *
	 * 3) DXVK_FILTER_DEVICE_NAME — DXVK/Proton device selection by
	 *    name substring.  Guarantees Proton games use the dGPU.
	 */
	switch (dgpu->vendor) {
	case GPU_VENDOR_NVIDIA:
		setenv("VK_LOADER_DRIVERS_SELECT", "nvidia*", 1);
		/* PCI vendor 0x10de = NVIDIA */
		setenv("MESA_VK_DEVICE_SELECT", "10de:", 1);
		setenv("MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE", "1", 1);
		setenv("DXVK_FILTER_DEVICE_NAME", "NVIDIA", 1);
		/* Ensure Proton exposes NVIDIA GPU to games */
		setenv("PROTON_HIDE_NVIDIA_GPU", "0", 1);
		setenv("PROTON_ENABLE_NVAPI", "1", 1);
		setenv("DXVK_ENABLE_NVAPI", "1", 1);

		/* CUDA / NVENC / NVDEC routing — Blender, PyTorch, ffmpeg
		 * with -hwaccel cuda, OBS NVENC, etc. PCI_BUS_ID order makes
		 * device 0 deterministic on multi-GPU systems. */
		setenv("CUDA_DEVICE_ORDER", "PCI_BUS_ID", 0);
		setenv("CUDA_VISIBLE_DEVICES", "0", 0);
		setenv("NVIDIA_VISIBLE_DEVICES", "all", 0);
		setenv("NVIDIA_DRIVER_CAPABILITIES", "all", 0);
		break;
	case GPU_VENDOR_AMD:
		setenv("VK_LOADER_DRIVERS_SELECT", "radeon*,amd*", 1);
		/* PCI vendor 0x1002 = AMD */
		setenv("MESA_VK_DEVICE_SELECT", "1002:", 1);
		setenv("MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE", "1", 1);
		setenv("DXVK_FILTER_DEVICE_NAME", "AMD", 1);

		/* ROCm / HIP routing — Blender HIP backend, PyTorch ROCm,
		 * llama.cpp HIP build, etc. Index 0 = first ROCm-capable
		 * device, which the kernel orders dGPU before iGPU. */
		setenv("HIP_VISIBLE_DEVICES", "0", 0);
		setenv("ROCR_VISIBLE_DEVICES", "0", 0);
		break;
	default:
		setenv("VK_LOADER_DRIVERS_SELECT", "nvidia*,radeon*,amd*", 1);
		break;
	}
}

void
set_steam_env(void)
{
	GpuInfo *dgpu;

	/*
	 * AUDIO: Ensure Steam and games use PipeWire for audio
	 *
	 * SDL_AUDIODRIVER=pipewire: Tell SDL (used by many games) to use PipeWire directly
	 * PULSE_SERVER: Point PulseAudio clients to PipeWire's pulse socket
	 * PIPEWIRE_RUNTIME_DIR: Ensure PipeWire socket is found
	 *
	 * Note: Modern PipeWire provides PulseAudio compatibility, so most games
	 * will work via the pulse socket even without explicit PipeWire support.
	 */
	setenv("SDL_AUDIODRIVER", "pipewire,pulseaudio,alsa", 0); /* Don't override if already set */

	/* Ensure XDG_RUNTIME_DIR is set (needed for PipeWire socket) */
	if (!getenv("XDG_RUNTIME_DIR")) {
		char runtime_dir[256];
		snprintf(runtime_dir, sizeof(runtime_dir), "/run/user/%d", getuid());
		setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
	}

	/* For Proton/Wine games: use PipeWire via the PulseAudio interface */
	setenv("PULSE_LATENCY_MSEC", "60", 0); /* Reasonable latency for games */

	/* UI scaling fix for better Big Picture performance on hybrid systems */
	setenv("STEAM_FORCE_DESKTOPUI_SCALING", "1", 1);

	/* Tell Steam to prefer host libraries over runtime for better driver compat */
	setenv("STEAM_RUNTIME_PREFER_HOST_LIBRARIES", "1", 1);

	/* CEF/Chromium GPU override flags - force hardware acceleration
	 * These help bypass Steam's internal GPU blocklist on hybrid systems */
	setenv("STEAM_DISABLE_GPU_BLOCKLIST", "1", 1);
	setenv("STEAM_CEF_ARGS", "--ignore-gpu-blocklist --enable-gpu-rasterization --enable-zero-copy --enable-native-gpu-memory-buffers --disable-gpu-driver-bug-workarounds", 1);

	/* Override CEF webhelper flags - Steam internally adds --disable-gpu which kills performance
	 * STEAM_WEBHELPER_CEF_FLAGS can override/append to steamwebhelper arguments */
	setenv("STEAM_WEBHELPER_CEF_FLAGS", "--enable-gpu --enable-gpu-compositing --enable-accelerated-video-decode --ignore-gpu-blocklist", 1);

	/* Set dGPU env for Steam — this calls set_dgpu_env() which handles
	 * ALL GPU vendor-specific vars including Vulkan device selection.
	 * Redundant with the compositor-level call, but ensures the vars
	 * are present even if Steam was started before GPU detection. */
	set_dgpu_env();

	if (discrete_gpu_idx >= 0 && discrete_gpu_idx < detected_gpu_count) {
		dgpu = &detected_gpus[discrete_gpu_idx];
		if (dgpu->vendor == GPU_VENDOR_NVIDIA) {
			/* NVIDIA-specific CEF args for better ANGLE/Vulkan support */
			setenv("STEAM_CEF_GPU_ARGS", "--use-angle=vulkan --enable-features=Vulkan,UseSkiaRenderer", 1);
		}
	}
}

