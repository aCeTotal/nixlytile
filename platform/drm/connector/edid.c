#include <libdisplay-info/edid.h>
#include <libdisplay-info/info.h>
#include <stdlib.h>
#include <string.h>
#include "platform/drm/connector/connector.h"
#include "platform/drm/connector/edid.h"
#include "platform/log/log.h"

static char *edid_make(const struct di_edid *edid) {
	const struct di_edid_vendor_product *vendor_product = di_edid_get_vendor_product(edid);
	const char *manu = drm_pnp_manufacturer(vendor_product->manufacturer);
	if (manu) {
		return strdup(manu);
	}

	char pnp_id[] = {
		vendor_product->manufacturer[0],
		vendor_product->manufacturer[1],
		vendor_product->manufacturer[2],
		'\0',
	};
	return strdup(pnp_id);
}

static void read_default_primaries(struct nx_output *output,
		const struct di_info *info) {
	const struct di_color_primaries *primaries = di_info_get_default_color_primaries(info);
	if (!primaries->has_primaries) {
		return;
	}

	output->default_primaries_value = (struct nx_color_primaries) {
		.red = { .x = primaries->primary[0].x, .y = primaries->primary[0].y },
		.green = { .x = primaries->primary[1].x, .y = primaries->primary[1].y },
		.blue = { .x = primaries->primary[2].x, .y = primaries->primary[2].y },
		.white = { .x = primaries->default_white.x, .y = primaries->default_white.y },
	};
	output->default_primaries = &output->default_primaries_value;
}

static void read_hdr_support(struct nx_drm_connector *conn,
		const struct di_info *info) {
	struct nx_output *output = &conn->output;

	const struct di_supported_signal_colorimetry *colorimetry = di_info_get_supported_signal_colorimetry(info);
	bool has_bt2020 = colorimetry->bt2020_cycc || colorimetry->bt2020_ycc || colorimetry->bt2020_rgb;
	if (conn->props.colorspace != 0 && has_bt2020) {
		output->supported_primaries |= NX_COLOR_NAMED_PRIMARIES_BT2020;
	}

	const struct di_hdr_static_metadata *hdr = di_info_get_hdr_static_metadata(info);
	if (conn->props.hdr_output_metadata == 0 || !hdr->type1 || !hdr->pq) {
		return;
	}

	// Fake-HDR panels omit max luminance
	if (hdr->desired_content_max_luminance > 0) {
		output->supported_transfer_functions |= NX_COLOR_TRANSFER_FUNCTION_ST2084_PQ;
		return;
	}
	drm_conn_log(conn, NX_INFO, "EDID advertises PQ but no "
		"max luminance — treating display as SDR-only");
}

void drm_parse_edid(struct nx_drm_connector *conn, size_t len, const uint8_t *data) {
	struct nx_output *output = &conn->output;

	free(output->make);
	free(output->model);
	free(output->serial);
	output->make = NULL;
	output->model = NULL;
	output->serial = NULL;

	struct di_info *info = di_info_parse_edid(data, len);
	if (info == NULL) {
		nx_log(NX_ERROR, "Failed to parse EDID");
		return;
	}

	output->make = edid_make(di_info_get_edid(info));
	output->model = di_info_get_model(info);
	output->serial = di_info_get_serial(info);

	read_default_primaries(output, info);
	read_hdr_support(conn, info);

	di_info_destroy(info);
}
