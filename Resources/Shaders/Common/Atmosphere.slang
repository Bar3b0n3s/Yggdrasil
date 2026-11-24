// Atmosphere: the sky's atmosphere on the GPU (M45, Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3), Hillaire's
// LUT method ("A Scalable and Production Ready Sky and Atmosphere Rendering Technique", EGSR 2020) in float, from the
// per-view SkyConstants (Shared/SkyConstants.h). Renderer/SkyAtmosphere.h is its CPU twin in double: the same Earth, the same
// densities, the same transmittance steps and the same LUT mappings, so "SkyLighting: the transmittance and multiple-scattering
// LUTs match their CPU reference" compares them. Lengths in km, directions unit and in world space with +Y up; a point of the
// atmosphere is planet-centred, the camera at (0, BottomRadius + altitude, 0) (the ground is under the camera, §8.3 "Sky").
// Included by Passes/Sky.slang after it declares `Sky` (ConstantBuffer<SkyConstants>).

static const float AtmospherePi = 3.14159265358979;
static const uint AtmosphereTransmittanceSteps = 40; // SkyTransmittanceSteps
static const uint AtmosphereScatteringSteps = 30;    // the sky-view LUT's steps along a view ray
static const uint AtmosphereMultipleScatteringSteps = 20;
static const uint AtmosphereMultipleScatteringDirections = 8; // per axis: 64 directions
static const float AtmosphereRayleighScaleHeight = 8.0;
static const float AtmosphereMieScaleHeight = 1.2;
static const float AtmosphereOzoneCenter = 25.0;
static const float AtmosphereOzoneHalfWidth = 15.0;
static const uint2 TransmittanceLutSize = uint2(256, 64);
static const uint MultipleScatteringLutSize = 32;
static const uint2 SkyViewLutSize = uint2(192, 108);

// The atmosphere's extinction per km at `height` km above the ground (ComputeSkyExtinction).
float3 GetAtmosphereExtinction(float height)
{
	const float rayleigh = exp(-height / AtmosphereRayleighScaleHeight);
	const float mie = exp(-height / AtmosphereMieScaleHeight);
	const float ozone = max(0.0, 1.0 - abs(height - AtmosphereOzoneCenter) / AtmosphereOzoneHalfWidth);
	return Sky.RayleighScattering * rayleigh + Sky.MieExtinction * mie + Sky.OzoneAbsorption * ozone;
}

float3 GetRayleighScattering(float height)
{
	return Sky.RayleighScattering * exp(-height / AtmosphereRayleighScaleHeight);
}

float GetMieScattering(float height)
{
	return Sky.MieScattering * exp(-height / AtmosphereMieScaleHeight);
}

// Whether a ray from radius `r` with cosine `mu` to the local up meets the ground.
bool RayMeetsGround(float r, float mu)
{
	return mu < 0.0 && r * r * (mu * mu - 1.0) + Sky.BottomRadius * Sky.BottomRadius >= 0.0;
}

// The distance from radius `r` along cosine `mu` to the atmosphere's top (0 outside it), and to the ground (when met).
float DistanceToTop(float r, float mu)
{
	const float discriminant = r * r * (mu * mu - 1.0) + Sky.TopRadius * Sky.TopRadius;
	return max(-r * mu + sqrt(max(discriminant, 0.0)), 0.0);
}

float DistanceToGround(float r, float mu)
{
	const float discriminant = r * r * (mu * mu - 1.0) + Sky.BottomRadius * Sky.BottomRadius;
	return max(-r * mu - sqrt(max(discriminant, 0.0)), 0.0);
}

// The transmittance to the top by AtmosphereTransmittanceSteps midpoint steps, without the ground test: what the transmittance
// LUT holds for a ray that does not meet the ground.
float3 ComputeTransmittanceToTop(float r, float mu)
{
	const float distance = DistanceToTop(r, mu);
	const float step = distance / float(AtmosphereTransmittanceSteps);
	float3 depth = float3(0.0, 0.0, 0.0);
	for (uint index = 0; index < AtmosphereTransmittanceSteps; ++index)
	{
		const float t = (float(index) + 0.5) * step;
		const float radius = sqrt(max(r * r + t * t + 2.0 * r * mu * t, 0.0));
		depth += GetAtmosphereExtinction(radius - Sky.BottomRadius) * step;
	}
	return exp(-depth);
}

// Bruneton's transmittance mapping (as Hillaire's): x_mu spans the distance to the top between its least (straight up) and
// most (the horizon), x_r = rho / H, rho the distance to the horizon and H the top's; each in [0, 1] at the LUT's texel
// centres (texel i at i / (size - 1)).
float2 GetTransmittanceLutCoordinates(float r, float mu)
{
	const float h = sqrt(Sky.TopRadius * Sky.TopRadius - Sky.BottomRadius * Sky.BottomRadius);
	const float rho = sqrt(max(r * r - Sky.BottomRadius * Sky.BottomRadius, 0.0));
	const float d = DistanceToTop(r, mu);
	const float dMin = Sky.TopRadius - r;
	const float dMax = rho + h;
	return float2(dMax > dMin ? (d - dMin) / (dMax - dMin) : 0.0, rho / h);
}

void GetTransmittanceLutRadiusMu(float2 coordinates, out float r, out float mu)
{
	const float h = sqrt(Sky.TopRadius * Sky.TopRadius - Sky.BottomRadius * Sky.BottomRadius);
	const float rho = h * coordinates.y;
	r = sqrt(rho * rho + Sky.BottomRadius * Sky.BottomRadius);
	const float dMin = Sky.TopRadius - r;
	const float dMax = rho + h;
	const float d = dMin + coordinates.x * (dMax - dMin);
	mu = d == 0.0 ? 1.0 : clamp((h * h - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
}

// The texture coordinates of unit coordinates `unit` in a LUT of `size` texels, texel centres at 0 and 1.
float2 UnitToLutUv(float2 unit, float2 size)
{
	return (unit * (size - 1.0) + 0.5) / size;
}

float3 SampleTransmittance(Texture2D<float4> lut, SamplerState linearClamp, float r, float mu)
{
	if (RayMeetsGround(r, mu))
		return float3(0.0, 0.0, 0.0);
	return lut.SampleLevel(linearClamp, UnitToLutUv(GetTransmittanceLutCoordinates(r, mu), float2(TransmittanceLutSize)), 0.0).rgb;
}

// The multiple-scattering LUT: (the cosine of the sun's zenith angle, the height) over [-1, 1] x [0, top - bottom].
float2 GetMultipleScatteringLutUv(float r, float cosSunZenith)
{
	const float2 unit = float2(saturate(cosSunZenith * 0.5 + 0.5), saturate((r - Sky.BottomRadius) / (Sky.TopRadius - Sky.BottomRadius)));
	return UnitToLutUv(unit, float2(MultipleScatteringLutSize, MultipleScatteringLutSize));
}

// The phase functions: Rayleigh's and Cornette-Shanks' for Mie.
float GetRayleighPhase(float cosine)
{
	return 3.0 / (16.0 * AtmospherePi) * (1.0 + cosine * cosine);
}

float GetMiePhase(float cosine, float g)
{
	const float g2 = g * g;
	const float denominator = pow(max(1.0 + g2 - 2.0 * g * cosine, 1e-4), 1.5);
	return 3.0 / (8.0 * AtmospherePi) * (1.0 - g2) * (1.0 + cosine * cosine) / ((2.0 + g2) * denominator);
}

// The sky-view LUT: u the world azimuth (atan2(x, -z), wrapping), v the view's zenith angle mapped non-linearly so the
// horizon at radius r takes the middle row and texels gather about it (Hillaire's mapping).
float2 GetSkyViewLutUv(float3 direction, float r)
{
	const float horizon = sqrt(max(r * r - Sky.BottomRadius * Sky.BottomRadius, 0.0));
	const float beta = acos(clamp(horizon / r, -1.0, 1.0));
	const float zenithHorizonAngle = AtmospherePi - beta;
	const float viewZenith = acos(clamp(direction.y, -1.0, 1.0));
	float v;
	if (viewZenith < zenithHorizonAngle)
	{
		const float coordinate = 1.0 - sqrt(max(1.0 - viewZenith / zenithHorizonAngle, 0.0));
		v = coordinate * 0.5;
	}
	else
	{
		const float coordinate = sqrt(saturate((viewZenith - zenithHorizonAngle) / beta));
		v = coordinate * 0.5 + 0.5;
	}
	const float u = atan2(direction.x, -direction.z) / (2.0 * AtmospherePi) + 0.5;
	return float2(u, (v * (float(SkyViewLutSize.y) - 1.0) + 0.5) / float(SkyViewLutSize.y));
}

float3 GetSkyViewLutDirection(float2 uv, float r)
{
	const float v = (uv.y * float(SkyViewLutSize.y) - 0.5) / (float(SkyViewLutSize.y) - 1.0);
	const float horizon = sqrt(max(r * r - Sky.BottomRadius * Sky.BottomRadius, 0.0));
	const float beta = acos(clamp(horizon / r, -1.0, 1.0));
	const float zenithHorizonAngle = AtmospherePi - beta;
	float viewZenith;
	if (v < 0.5)
	{
		const float coordinate = 1.0 - 2.0 * v;
		viewZenith = zenithHorizonAngle * (1.0 - coordinate * coordinate);
	}
	else
	{
		const float coordinate = 2.0 * v - 1.0;
		viewZenith = zenithHorizonAngle + beta * coordinate * coordinate;
	}
	const float azimuth = (uv.x - 0.5) * 2.0 * AtmospherePi;
	const float sinZenith = sin(viewZenith);
	return float3(sinZenith * sin(azimuth), cos(viewZenith), -sinZenith * cos(azimuth));
}
