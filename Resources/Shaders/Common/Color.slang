// Colour encoding (Architecture §8.9). Render targets and the swapchain are UNORM and hold sRGB-encoded values, so
// shaders that output display colours encode them themselves with the sRGB OETF; linear inputs (vertex colours,
// material factors) are decoded with its inverse.
module Color;

// The sRGB OETF (IEC 61966-2-1) per channel, for linear values in [0, 1].
public float3 LinearToSrgb(float3 linearColor)
{
	const float3 clamped = saturate(linearColor);
	const float3 low = clamped * 12.92;
	const float3 high = 1.055 * pow(clamped, 1.0 / 2.4) - 0.055;
	return select(clamped <= 0.0031308, low, high);
}

// The inverse of LinearToSrgb, for encoded values in [0, 1].
public float3 SrgbToLinear(float3 srgbColor)
{
	const float3 clamped = saturate(srgbColor);
	const float3 low = clamped / 12.92;
	const float3 high = pow((clamped + 0.055) / 1.055, 2.4);
	return select(clamped <= 0.04045, low, high);
}
