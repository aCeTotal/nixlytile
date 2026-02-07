/*
 * GPU-specific game launch parameters
 *
 * These parameters are automatically applied based on detected GPU.
 * If the user has already set a parameter in Steam's launch options,
 * it will NOT be duplicated.
 *
 * Sources:
 *   - ProtonDB community reports (https://www.protondb.com/)
 *   - CachyOS Gaming Guide (https://wiki.cachyos.org/configuration/gaming/)
 *   - Valve Proton documentation (https://github.com/ValveSoftware/Proton)
 *   - GamingOnLinux verified reports
 *
 * Format: { "game_id", "nvidia_params", "amd_params", "intel_params" }
 *
 * Common verified parameters:
 *
 * NVIDIA-specific:
 *   PROTON_ENABLE_NVAPI=1          - Enable NVIDIA API (required for DLSS)
 *   PROTON_HIDE_NVIDIA_GPU=0       - Expose NVIDIA GPU to game
 *   DXVK_ENABLE_NVAPI=1            - DXVK NVAPI support
 *   __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1 - Prevent shader cache cleanup
 *
 * AMD-specific:
 *   RADV_PERFTEST=gpl              - GPL shader optimization
 *   AMD_VULKAN_ICD=RADV            - Force RADV Vulkan driver
 *   ENABLE_LAYER_MESA_ANTI_LAG=1   - AMD Anti-Lag
 *
 * Intel-specific:
 *   PROTON_XESS_UPGRADE=1          - Auto-upgrade XeSS
 *
 * General (all GPUs):
 *   PROTON_USE_NTSYNC=1            - Better sync primitives (requires kernel 6.14+)
 *   VKD3D_CONFIG=dxr               - Enable DX12 ray tracing (DXR 1.0)
 *   VKD3D_CONFIG=dxr11             - Enable DXR 1.1
 *   VKD3D_CONFIG=no_upload_hvv     - Fix stuttering in some games
 *   DXVK_ASYNC=1                   - Async shader compilation
 *   PULSE_LATENCY_MSEC=60          - Fix audio sync issues
 *   PROTON_ENABLE_WAYLAND=1        - Native Wayland (experimental)
 *
 * Use %command% as placeholder for the game command.
 */

typedef struct {
	const char *game_id;      /* Steam AppID or game identifier */
	const char *nvidia;       /* Launch params for NVIDIA GPUs */
	const char *amd;          /* Launch params for AMD GPUs (RADV) */
	const char *amd_amdvlk;   /* Launch params for AMD GPUs (AMDVLK) - NULL = use .amd */
	const char *intel;        /* Launch params for Intel GPUs */
} GameLaunchParams;

/* AMD Vulkan driver type */
typedef enum {
	AMD_DRIVER_RADV,    /* Mesa RADV (default, recommended for gaming) */
	AMD_DRIVER_AMDVLK,  /* AMD's official open-source driver */
	AMD_DRIVER_UNKNOWN
} AmdVulkanDriver;

/*
 * Detect which AMD Vulkan driver is active.
 * Checks VK_ICD_FILENAMES and AMD_VULKAN_ICD env vars,
 * then falls back to checking if amdvlk ICD is present.
 */
static inline AmdVulkanDriver
detect_amd_vulkan_driver(void)
{
	const char *icd_env, *amd_icd_env;
	FILE *fp;
	char buf[256];

	/* Check explicit environment overrides first */
	amd_icd_env = getenv("AMD_VULKAN_ICD");
	if (amd_icd_env) {
		if (strcasecmp(amd_icd_env, "AMDVLK") == 0)
			return AMD_DRIVER_AMDVLK;
		if (strcasecmp(amd_icd_env, "RADV") == 0)
			return AMD_DRIVER_RADV;
	}

	/* Check VK_ICD_FILENAMES for explicit driver selection */
	icd_env = getenv("VK_ICD_FILENAMES");
	if (icd_env) {
		if (strstr(icd_env, "amd_icd") || strstr(icd_env, "amdvlk"))
			return AMD_DRIVER_AMDVLK;
		if (strstr(icd_env, "radeon_icd") || strstr(icd_env, "radv"))
			return AMD_DRIVER_RADV;
	}

	/* Check if AMDVLK ICD file exists (means user likely wants AMDVLK) */
	/* Common paths: /usr/share/vulkan/icd.d/amd_icd64.json */
	fp = fopen("/usr/share/vulkan/icd.d/amd_icd64.json", "r");
	if (fp) {
		fclose(fp);
		/* AMDVLK is installed, but check if it's the preferred one */
		/* by looking at vulkaninfo or driver ordering */
		/* For safety, if both are installed, prefer RADV */
		fp = fopen("/usr/share/vulkan/icd.d/radeon_icd.x86_64.json", "r");
		if (fp) {
			fclose(fp);
			/* Both installed - default to RADV (better for gaming) */
			return AMD_DRIVER_RADV;
		}
		return AMD_DRIVER_AMDVLK;
	}

	/* Default to RADV (most common on Linux gaming distros) */
	return AMD_DRIVER_RADV;
}

/* Default parameters - applied to ALL games if no specific entry exists */
static const GameLaunchParams default_game_params = {
	.game_id    = NULL,
	.nvidia     = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 PROTON_HIDE_NVIDIA_GPU=0 DXVK_ENABLE_NVAPI=1 __GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1 %command%",
	.amd        = "PROTON_USE_NTSYNC=1 RADV_PERFTEST=gpl ENABLE_LAYER_MESA_ANTI_LAG=1 %command%",
	.amd_amdvlk = "PROTON_USE_NTSYNC=1 %command%",
	.intel      = "PROTON_USE_NTSYNC=1 PROTON_XESS_UPGRADE=1 %command%",
};

/*
 * Per-game launch parameters
 * Verified from ProtonDB reports and community testing.
 * Find Steam AppIDs at: https://steamdb.info/
 */
static const GameLaunchParams game_launch_params[] = {
	/* ============ AAA TITLES WITH DLSS/RT SUPPORT ============ */

	/* Elden Ring (1245620) - DLSS support, DXR */
	{
		.game_id    = "1245620",
		.nvidia     = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 DXVK_ENABLE_NVAPI=1 VKD3D_CONFIG=no_upload_hvv %command%",
		.amd        = "PROTON_USE_NTSYNC=1 RADV_PERFTEST=gpl VKD3D_CONFIG=no_upload_hvv %command%",
		.amd_amdvlk = "PROTON_USE_NTSYNC=1 VKD3D_CONFIG=no_upload_hvv %command%",
		.intel      = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Cyberpunk 2077 (1091500) - DLSS, RT, Path Tracing */
	{
		.game_id = "1091500",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_HIDE_NVIDIA_GPU=0 PROTON_ENABLE_NVAPI=1 VKD3D_CONFIG=dxr %command%",
		.amd     = "PROTON_USE_NTSYNC=1 VKD3D_CONFIG=dxr %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Baldur's Gate 3 (1086940) - Vulkan native, good compatibility */
	{
		.game_id = "1086940",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 RADV_PERFTEST=gpl %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* God of War (1593500) - DLSS support */
	{
		.game_id = "1593500",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 DXVK_ENABLE_NVAPI=1 DXVK_ASYNC=1 PULSE_LATENCY_MSEC=60 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 DXVK_ASYNC=1 PULSE_LATENCY_MSEC=60 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 PULSE_LATENCY_MSEC=60 %command%",
	},

	/* God of War Ragnarok (2322010) - DLSS support */
	{
		.game_id = "2322010",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 DXVK_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Horizon Zero Dawn (1151640) - DLSS support */
	{
		.game_id = "1151640",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Horizon Zero Dawn Remastered (2420110) */
	{
		.game_id = "2420110",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Horizon Forbidden West (2420510) - DLSS support */
	{
		.game_id = "2420510",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 DXVK_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Red Dead Redemption 2 (1174180) - DLSS/FSR support */
	{
		.game_id = "1174180",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Red Dead Redemption (2668510) - Console port */
	{
		.game_id = "2668510",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* The Witcher 3 (292030) - DLSS support */
	{
		.game_id = "292030",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 VKD3D_CONFIG=dxr %command% --launcher-skip",
		.amd     = "PROTON_USE_NTSYNC=1 VKD3D_CONFIG=dxr %command% --launcher-skip",
		.intel   = "PROTON_USE_NTSYNC=1 %command% --launcher-skip",
	},

	/* Hogwarts Legacy (990080) - DLSS, RT */
	{
		.game_id = "990080",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 DXVK_ENABLE_NVAPI=1 VKD3D_CONFIG=dxr %command%",
		.amd     = "PROTON_USE_NTSYNC=1 VKD3D_CONFIG=dxr %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Starfield (1716740) - DLSS support */
	{
		.game_id = "1716740",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Alan Wake 2 (2177660) - DLSS, RT heavy */
	{
		.game_id = "2177660",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 DXVK_ENABLE_NVAPI=1 VKD3D_CONFIG=dxr11 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 VKD3D_CONFIG=dxr11 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* ============ SOULS-LIKE / FROM SOFTWARE ============ */

	/* Dark Souls III (374320) */
	{
		.game_id = "374320",
		.nvidia  = "PROTON_USE_NTSYNC=1 DXVK_ASYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 DXVK_ASYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Dark Souls Remastered (570940) */
	{
		.game_id = "570940",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Sekiro (814380) */
	{
		.game_id = "814380",
		.nvidia  = "PROTON_USE_NTSYNC=1 DXVK_ASYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 DXVK_ASYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Armored Core VI (1888160) - DLSS support */
	{
		.game_id = "1888160",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Lies of P (1627720) - DLSS support */
	{
		.game_id = "1627720",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* ============ MONSTER HUNTER ============ */

	/* Monster Hunter Rise (1446780) */
	{
		.game_id = "1446780",
		.nvidia  = "PROTON_USE_NTSYNC=1 DXVK_ASYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 DXVK_ASYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Monster Hunter World (582010) */
	{
		.game_id = "582010",
		.nvidia  = "PROTON_USE_NTSYNC=1 DXVK_ASYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 DXVK_ASYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Monster Hunter Wilds (2246340) - DLSS, RT */
	{
		.game_id = "2246340",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_HIDE_NVIDIA_GPU=0 PROTON_ENABLE_NVAPI=1 VKD3D_CONFIG=dxr11 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 VKD3D_CONFIG=dxr11 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* ============ MULTIPLAYER / COMPETITIVE ============ */

	/* Counter-Strike 2 (730) - Native Linux, no Proton needed */
	{
		.game_id = "730",
		.nvidia  = "%command%",
		.amd     = "%command%",
		.intel   = "%command%",
	},

	/* Dota 2 (570) - Native Linux, no Proton needed */
	{
		.game_id = "570",
		.nvidia  = "%command%",
		.amd     = "%command%",
		.intel   = "%command%",
	},

	/* Team Fortress 2 (440) - Native Linux, no Proton needed */
	{
		.game_id = "440",
		.nvidia  = "%command%",
		.amd     = "%command%",
		.intel   = "%command%",
	},

	/* Apex Legends (1172470) */
	{
		.game_id = "1172470",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Helldivers 2 (553850) - DLSS support */
	{
		.game_id = "553850",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* ============ ACTION / ADVENTURE ============ */

	/* GTA V (271590) - DLSS support */
	{
		.game_id = "271590",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Hades (1145360) */
	{
		.game_id = "1145360",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Hades II (1145350) */
	{
		.game_id = "1145350",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Death Stranding (1190460) - DLSS support */
	{
		.game_id = "1190460",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Death Stranding Director's Cut (1850570) - DLSS support */
	{
		.game_id = "1850570",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Control (870780) - DLSS, RT */
	{
		.game_id = "870780",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 VKD3D_CONFIG=dxr %command%",
		.amd     = "PROTON_USE_NTSYNC=1 VKD3D_CONFIG=dxr %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Ghostwire Tokyo (1475810) - DLSS, RT */
	{
		.game_id = "1475810",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 VKD3D_CONFIG=dxr %command%",
		.amd     = "PROTON_USE_NTSYNC=1 VKD3D_CONFIG=dxr %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* ============ RPG ============ */

	/* Diablo IV (2344520) - DLSS support */
	{
		.game_id = "2344520",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Path of Exile (238960) */
	{
		.game_id = "238960",
		.nvidia  = "PROTON_USE_NTSYNC=1 DXVK_ASYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 DXVK_ASYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Path of Exile 2 (2694490) - DLSS support */
	{
		.game_id = "2694490",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 DXVK_ASYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 DXVK_ASYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Dragon's Dogma 2 (2054970) - DLSS support */
	{
		.game_id = "2054970",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_HIDE_NVIDIA_GPU=0 PROTON_ENABLE_NVAPI=1 VKD3D_CONFIG=dxr11,no_upload_hvv %command%",
		.amd     = "PROTON_USE_NTSYNC=1 VKD3D_CONFIG=no_upload_hvv RADV_PERFTEST=nosam %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Final Fantasy XVI (2515020) - DLSS support */
	{
		.game_id = "2515020",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Final Fantasy VII Rebirth (2909400) - DLSS support */
	{
		.game_id = "2909400",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Final Fantasy VII Remake Intergrade (1462040) - DLSS support */
	{
		.game_id = "1462040",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Remnant 2 (1282100) - DLSS support */
	{
		.game_id = "1282100",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 DXVK_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Remnant: From the Ashes (617290) */
	{
		.game_id = "617290",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* ============ SIMULATION / SURVIVAL ============ */

	/* Palworld (1623730) */
	{
		.game_id = "1623730",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Valheim (892970) */
	{
		.game_id = "892970",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Satisfactory (526870) */
	{
		.game_id = "526870",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Factorio (427520) - Native Linux, no Proton needed */
	{
		.game_id = "427520",
		.nvidia  = "%command%",
		.amd     = "%command%",
		.intel   = "%command%",
	},

	/* No Man's Sky (275850) - DLSS support */
	{
		.game_id = "275850",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Subnautica (264710) */
	{
		.game_id = "264710",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Deep Rock Galactic (548430) */
	{
		.game_id = "548430",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* ============ RACING ============ */

	/* Forza Horizon 5 (1551360) - DLSS support, needs GE-Proton */
	{
		.game_id = "1551360",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Forza Horizon 4 (1293830) */
	{
		.game_id = "1293830",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* ============ INDIE / OTHER ============ */

	/* Stardew Valley (413150) - Native Linux, no Proton needed */
	{
		.game_id = "413150",
		.nvidia  = "%command%",
		.amd     = "%command%",
		.intel   = "%command%",
	},

	/* Terraria (105600) - Native Linux, no Proton needed */
	{
		.game_id = "105600",
		.nvidia  = "%command%",
		.amd     = "%command%",
		.intel   = "%command%",
	},

	/* Hollow Knight (367520) - Native Linux, no Proton needed */
	{
		.game_id = "367520",
		.nvidia  = "%command%",
		.amd     = "%command%",
		.intel   = "%command%",
	},

	/* Lethal Company (1966720) */
	{
		.game_id = "1966720",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* ============ CALL OF DUTY ============ */

	/* Call of Duty Modern Warfare / Warzone (1962663) */
	{
		.game_id = "1962663",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* ============ RESIDENT EVIL ============ */

	/* Resident Evil 4 Remake (2050650) - DLSS, RT */
	{
		.game_id = "2050650",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 VKD3D_CONFIG=dxr %command%",
		.amd     = "PROTON_USE_NTSYNC=1 VKD3D_CONFIG=dxr %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Resident Evil Village (1196590) - DLSS, RT */
	{
		.game_id = "1196590",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 VKD3D_CONFIG=dxr %command%",
		.amd     = "PROTON_USE_NTSYNC=1 VKD3D_CONFIG=dxr %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* ============ STAR WARS ============ */

	/* Star Wars Jedi: Survivor (1774580) - DLSS, RT */
	{
		.game_id = "1774580",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 VKD3D_CONFIG=dxr %command%",
		.amd     = "PROTON_USE_NTSYNC=1 VKD3D_CONFIG=dxr %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Star Wars Jedi: Fallen Order (1172380) */
	{
		.game_id = "1172380",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Star Wars Outlaws (2842050) - DLSS, RT */
	{
		.game_id = "2842050",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 VKD3D_CONFIG=dxr %command%",
		.amd     = "PROTON_USE_NTSYNC=1 VKD3D_CONFIG=dxr %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* ============ ASSASSIN'S CREED ============ */

	/* Assassin's Creed Valhalla (2208920) */
	{
		.game_id = "2208920",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Assassin's Creed Mirage (2724730) */
	{
		.game_id = "2724730",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* ============ OTHER AAA ============ */

	/* Spider-Man Remastered (1817070) - DLSS, RT */
	{
		.game_id = "1817070",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 VKD3D_CONFIG=dxr %command%",
		.amd     = "PROTON_USE_NTSYNC=1 VKD3D_CONFIG=dxr %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Spider-Man Miles Morales (1817190) - DLSS, RT */
	{
		.game_id = "1817190",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 VKD3D_CONFIG=dxr %command%",
		.amd     = "PROTON_USE_NTSYNC=1 VKD3D_CONFIG=dxr %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* The Last of Us Part I (1888930) - DLSS support */
	{
		.game_id = "1888930",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Dragon Age: The Veilguard (1845910) - DLSS, RT */
	{
		.game_id = "1845910",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 VKD3D_CONFIG=dxr %command%",
		.amd     = "PROTON_USE_NTSYNC=1 VKD3D_CONFIG=dxr %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Black Myth: Wukong (2358720) - DLSS, RT */
	{
		.game_id = "2358720",
		.nvidia  = "PROTON_USE_NTSYNC=1 PROTON_ENABLE_NVAPI=1 DXVK_ENABLE_NVAPI=1 VKD3D_CONFIG=dxr %command%",
		.amd     = "PROTON_USE_NTSYNC=1 VKD3D_CONFIG=dxr %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Skull and Bones (2531830) */
	{
		.game_id = "2531830",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Hunt: Showdown (770720) */
	{
		.game_id = "770720",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* World of Warcraft (2135780) - Battle.net */
	{
		.game_id = "2135780",
		.nvidia  = "PROTON_USE_NTSYNC=1 %command%",
		.amd     = "PROTON_USE_NTSYNC=1 %command%",
		.intel   = "PROTON_USE_NTSYNC=1 %command%",
	},

	/* Sentinel terminator - must be last */
	{ .game_id = NULL, .nvidia = NULL, .amd = NULL, .intel = NULL }
};

/*
 * Get AMD params based on detected driver
 */
static inline const char *
get_amd_params(const GameLaunchParams *p)
{
	static AmdVulkanDriver cached_driver = AMD_DRIVER_UNKNOWN;
	const char *params;

	/* Cache driver detection result */
	if (cached_driver == AMD_DRIVER_UNKNOWN)
		cached_driver = detect_amd_vulkan_driver();

	if (cached_driver == AMD_DRIVER_AMDVLK && p->amd_amdvlk)
		params = p->amd_amdvlk;
	else
		params = p->amd;

	return params;
}

/*
 * Check if params are just "%command%" with nothing else useful.
 * Returns 1 if params should fall back to defaults.
 */
static inline int
is_empty_params(const char *params)
{
	const char *p;

	if (!params || !params[0])
		return 1;

	/* Skip whitespace */
	p = params;
	while (*p == ' ' || *p == '\t')
		p++;

	/* Check if it's just %command% optionally with trailing whitespace */
	if (strncmp(p, "%command%", 9) == 0) {
		p += 9;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0')
			return 1;
	}

	return 0;
}

/*
 * Look up launch params for a specific game and GPU vendor.
 * Returns the params string. If game-specific params are just "%command%",
 * returns default params instead.
 */
static inline const char *
get_game_launch_params(const char *game_id, GpuVendor vendor)
{
	const GameLaunchParams *p;
	const char *params;

	if (!game_id)
		return NULL;

	/* Search for game-specific params */
	for (p = game_launch_params; p->game_id; p++) {
		if (strcmp(p->game_id, game_id) == 0) {
			switch (vendor) {
			case GPU_VENDOR_NVIDIA: params = p->nvidia; break;
			case GPU_VENDOR_AMD:    params = get_amd_params(p); break;
			case GPU_VENDOR_INTEL:  params = p->intel; break;
			default:                params = p->nvidia; break;
			}
			/* If params are empty/just %command%, use defaults */
			if (!is_empty_params(params))
				return params;
			break;
		}
	}

	/* Return default params */
	switch (vendor) {
	case GPU_VENDOR_NVIDIA: return default_game_params.nvidia;
	case GPU_VENDOR_AMD:    return get_amd_params(&default_game_params);
	case GPU_VENDOR_INTEL:  return default_game_params.intel;
	default:                return default_game_params.nvidia;
	}
}

/*
 * Check if a parameter/token already exists in user's launch options.
 * Returns 1 if found, 0 if not.
 */
static inline int
param_exists_in_options(const char *user_options, const char *param)
{
	const char *p;
	size_t param_len;

	if (!user_options || !param || !param[0])
		return 0;

	param_len = strlen(param);

	/* Handle environment variable format (VAR=value) */
	if (strchr(param, '=')) {
		/* Extract variable name (before '=') */
		const char *eq = strchr(param, '=');
		size_t var_len = eq - param;
		char var_name[128];

		if (var_len >= sizeof(var_name))
			var_len = sizeof(var_name) - 1;
		strncpy(var_name, param, var_len);
		var_name[var_len] = '\0';

		/* Search for VAR= in user options */
		p = user_options;
		while ((p = strstr(p, var_name)) != NULL) {
			/* Check it's a word boundary and followed by '=' */
			if ((p == user_options || isspace((unsigned char)*(p-1))) &&
			    p[var_len] == '=') {
				return 1;
			}
			p++;
		}
		return 0;
	}

	/* Handle command/word (e.g., "gamemoderun", "mangohud") */
	p = user_options;
	while ((p = strstr(p, param)) != NULL) {
		/* Check word boundaries */
		int at_start = (p == user_options || isspace((unsigned char)*(p-1)));
		int at_end = (p[param_len] == '\0' || isspace((unsigned char)p[param_len]) ||
		              p[param_len] == '%');
		if (at_start && at_end)
			return 1;
		p++;
	}

	return 0;
}

/*
 * Build merged launch options: add our params only if not already present.
 * user_options: existing user launch options from Steam (can be NULL/empty)
 * gpu_params: our GPU-specific params with %command% placeholder
 * out: output buffer
 * out_size: size of output buffer
 *
 * Returns: pointer to out buffer
 */
static inline char *
build_merged_launch_options(const char *user_options, const char *gpu_params,
                            char *out, size_t out_size)
{
	char params_copy[512];
	char merged_prefix[512] = {0};
	char *tok, *saveptr;
	const char *command_pos;
	int prefix_len = 0;

	if (!gpu_params || !gpu_params[0]) {
		/* No GPU params, just use user options or empty */
		if (user_options && user_options[0])
			snprintf(out, out_size, "%s", user_options);
		else
			snprintf(out, out_size, "%%command%%");
		return out;
	}

	/* Find %command% in gpu_params */
	command_pos = strstr(gpu_params, "%command%");
	if (command_pos) {
		prefix_len = command_pos - gpu_params;
	} else {
		prefix_len = strlen(gpu_params);
	}

	/* Copy prefix part for tokenization */
	if (prefix_len > 0 && prefix_len < (int)sizeof(params_copy)) {
		strncpy(params_copy, gpu_params, prefix_len);
		params_copy[prefix_len] = '\0';
	} else {
		params_copy[0] = '\0';
	}

	/* Process each token, skip if user already has it */
	merged_prefix[0] = '\0';
	for (tok = strtok_r(params_copy, " \t", &saveptr); tok;
	     tok = strtok_r(NULL, " \t", &saveptr)) {
		if (!param_exists_in_options(user_options, tok)) {
			if (merged_prefix[0])
				strncat(merged_prefix, " ", sizeof(merged_prefix) - strlen(merged_prefix) - 1);
			strncat(merged_prefix, tok, sizeof(merged_prefix) - strlen(merged_prefix) - 1);
		}
	}

	/* Build final string */
	if (merged_prefix[0] && user_options && user_options[0]) {
		/* We have both our params and user params */
		snprintf(out, out_size, "%s %s", merged_prefix, user_options);
	} else if (merged_prefix[0]) {
		/* Only our params */
		snprintf(out, out_size, "%s %%command%%", merged_prefix);
	} else if (user_options && user_options[0]) {
		/* Only user params */
		snprintf(out, out_size, "%s", user_options);
	} else {
		/* Nothing */
		snprintf(out, out_size, "%%command%%");
	}

	return out;
}
