/* Hue drift across bar icons. */
#include "nixlytile.h"

#define DRIFT_DEG 60.0 /* hue span, leftmost to rightmost */

static const double rgb_to_lms[3][3] = {
	{ 0.4122214708, 0.5363325363, 0.0514459929 },
	{ 0.2119034982, 0.6806995451, 0.1073969566 },
	{ 0.0883024619, 0.2817188376, 0.6299787005 },
};
static const double lms_to_lab[3][3] = {
	{ 0.2104542553, 0.7936177850, -0.0040720468 },
	{ 1.9779984951, -2.4285922050, 0.4505937099 },
	{ 0.0259040371, 0.7827717662, -0.8086757660 },
};
static const double lab_to_lms[3][3] = {
	{ 1.0, 0.3963377774, 0.2158037573 },
	{ 1.0, -0.1055613458, -0.0638541728 },
	{ 1.0, -0.0894841775, -1.2914855480 },
};
static const double lms_to_rgb[3][3] = {
	{ 4.0767416621, -3.3077115913, 0.2309699292 },
	{ -1.2684380046, 2.6097574011, -0.3413193965 },
	{ -0.0041960863, -0.7034186147, 1.7076147010 },
};

static void
mul(const double m[3][3], const double v[3], double out[3])
{
	for (int i = 0; i < 3; i++)
		out[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
}

static double
to_linear(double c)
{
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double
to_srgb(double c)
{
	c = c < 0.0 ? 0.0 : c > 1.0 ? 1.0 : c;
	return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

/* Hue offset for bar slot. */
double
bar_tint_drift(int slot, int slots)
{
	return slots > 1 ? DRIFT_DEG * ((double)slot / (slots - 1) - 0.5) : 0.0;
}

/* Hue-turned base in OKLCH. */
void
bar_tint(const float base[static 4], double deg, float out[static 4])
{
	double v[3], lms[3], lab[3], turned[3];
	double t = deg * M_PI / 180.0;

	for (int i = 0; i < 3; i++)
		v[i] = to_linear(base[i]);
	mul(rgb_to_lms, v, lms);
	for (int i = 0; i < 3; i++)
		lms[i] = cbrt(lms[i]);
	mul(lms_to_lab, lms, lab);
	turned[0] = lab[0];
	turned[1] = lab[1] * cos(t) - lab[2] * sin(t);
	turned[2] = lab[1] * sin(t) + lab[2] * cos(t);
	mul(lab_to_lms, turned, lms);
	for (int i = 0; i < 3; i++)
		lms[i] = lms[i] * lms[i] * lms[i];
	mul(lms_to_rgb, lms, v);
	for (int i = 0; i < 3; i++)
		out[i] = (float)to_srgb(v[i]);
	out[3] = base[3];
}
