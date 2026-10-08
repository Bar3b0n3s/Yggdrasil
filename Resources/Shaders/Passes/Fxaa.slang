// Fxaa: §8.3 pass 12 (Renderer/FxaaPass.h): FXAA 3.11's quality algorithm (Timothy Lottes, NVIDIA) with quality preset 12
// (five search steps of 1, 1.5, 2, 4 and 12 texels) and its default thresholds, as a compute pass from an LdrColor target
// into its ping-pong partner. The source holds display-encoded values (§8.9), which is the perceptual input FXAA expects,
// so luma is computed from the encoded colour directly (Rec. 601 weights).

Texture2D<float4> Source : register(t0, space0);
SamplerState LinearClamp : register(s0, space0);
[vk::image_format("rgba8")]
RWTexture2D<float4> Destination : register(u0, space0);

// FXAA 3.11's quality defaults: the amount of sub-pixel aliasing removal, the local contrast an edge needs relative to its
// brightest luma, and the contrast below which dark areas are left alone.
static const float Subpixel = 0.75;
static const float EdgeThreshold = 0.166;
static const float EdgeThresholdMin = 0.0833;
// Quality preset 12's search steps along the edge, in texels.
static const uint SearchStepCount = 5;
static const float SearchSteps[SearchStepCount] = { 1.0, 1.5, 2.0, 4.0, 12.0 };

float Luma(float3 color)
{
	return dot(color, float3(0.299, 0.587, 0.114));
}

float SampleLuma(float2 uv)
{
	return Luma(Source.SampleLevel(LinearClamp, uv, 0.0).rgb);
}

// The luma of the texel at `pixel` + `offset`, clamped to the image (as LinearClamp samples at texel centres).
float LoadLuma(int2 pixel, int2 offset, int2 lastPixel)
{
	return Luma(Source.Load(int3(clamp(pixel + offset, int2(0), lastPixel), 0)).rgb);
}

// The anti-aliased colour of texel `pixel` of a source of `size` texels.
float3 Fxaa(uint2 pixel, uint2 size)
{
	const float2 texel = 1.0 / float2(size);
	const float2 uv = (float2(pixel) + 0.5) * texel;
	const int2 center = int2(pixel);
	const int2 lastPixel = int2(size) - 1;
	const float3 colorM = Source.Load(int3(center, 0)).rgb;
	const float lumaM = Luma(colorM);
	float lumaS = LoadLuma(center, int2(0, 1), lastPixel);
	const float lumaE = LoadLuma(center, int2(1, 0), lastPixel);
	float lumaN = LoadLuma(center, int2(0, -1), lastPixel);
	const float lumaW = LoadLuma(center, int2(-1, 0), lastPixel);

	// Early exit where the local contrast is below the thresholds.
	const float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
	const float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
	const float range = rangeMax - rangeMin;
	if (range < max(EdgeThresholdMin, rangeMax * EdgeThreshold))
		return colorM;

	const float lumaNW = LoadLuma(center, int2(-1, -1), lastPixel);
	const float lumaSE = LoadLuma(center, int2(1, 1), lastPixel);
	const float lumaNE = LoadLuma(center, int2(1, -1), lastPixel);
	const float lumaSW = LoadLuma(center, int2(-1, 1), lastPixel);

	// Whether the edge runs horizontally or vertically, from the second differences of the 3x3 neighbourhood.
	const float lumaNS = lumaN + lumaS;
	const float lumaWE = lumaW + lumaE;
	const float lumaNESE = lumaNE + lumaSE;
	const float lumaNWNE = lumaNW + lumaNE;
	const float lumaNWSW = lumaNW + lumaSW;
	const float lumaSWSE = lumaSW + lumaSE;
	const float edgeHorizontal = abs(-2.0 * lumaW + lumaNWSW) + abs(-2.0 * lumaM + lumaNS) * 2.0 + abs(-2.0 * lumaE + lumaNESE);
	const float edgeVertical = abs(-2.0 * lumaS + lumaSWSE) + abs(-2.0 * lumaM + lumaWE) * 2.0 + abs(-2.0 * lumaN + lumaNWNE);
	const bool horizontalSpan = edgeHorizontal >= edgeVertical;

	// The sub-pixel blend from the neighbourhood's low-pass luma.
	const float subpixelA = (lumaNS + lumaWE) * 2.0 + lumaNWSW + lumaNESE;
	const float subpixelC = saturate(abs(subpixelA * (1.0 / 12.0) - lumaM) / range);
	const float subpixelF = (-2.0 * subpixelC + 3.0) * subpixelC * subpixelC;
	const float subpixelH = subpixelF * subpixelF * Subpixel;

	// The side of the edge with the steeper gradient: N or S for a horizontal span (W or E for a vertical one).
	if (!horizontalSpan)
	{
		lumaN = lumaW;
		lumaS = lumaE;
	}
	float lengthSign = horizontalSpan ? texel.y : texel.x;
	const float gradientN = lumaN - lumaM;
	const float gradientS = lumaS - lumaM;
	const bool pairN = abs(gradientN) >= abs(gradientS);
	const float gradient = max(abs(gradientN), abs(gradientS));
	if (pairN)
		lengthSign = -lengthSign;
	const float lumaNN = pairN ? lumaN + lumaM : lumaS + lumaM;

	// Search along the edge, half a texel towards the steeper side, in both directions until the luma leaves the edge's
	// average by a quarter of the gradient or the steps run out.
	float2 positionB = uv;
	const float2 step = horizontalSpan ? float2(texel.x, 0.0) : float2(0.0, texel.y);
	if (horizontalSpan)
		positionB.y += lengthSign * 0.5;
	else
		positionB.x += lengthSign * 0.5;
	const float gradientScaled = gradient * 0.25;
	const float lumaMM = lumaM - lumaNN * 0.5;
	float2 positionN = positionB - step * SearchSteps[0];
	float2 positionP = positionB + step * SearchSteps[0];
	float lumaEndN = SampleLuma(positionN) - lumaNN * 0.5;
	float lumaEndP = SampleLuma(positionP) - lumaNN * 0.5;
	bool doneN = abs(lumaEndN) >= gradientScaled;
	bool doneP = abs(lumaEndP) >= gradientScaled;
	if (!doneN)
		positionN -= step * SearchSteps[1];
	if (!doneP)
		positionP += step * SearchSteps[1];
	for (uint index = 2; index < SearchStepCount && !(doneN && doneP); ++index)
	{
		if (!doneN)
			lumaEndN = SampleLuma(positionN) - lumaNN * 0.5;
		if (!doneP)
			lumaEndP = SampleLuma(positionP) - lumaNN * 0.5;
		doneN = abs(lumaEndN) >= gradientScaled;
		doneP = abs(lumaEndP) >= gradientScaled;
		if (!doneN)
			positionN -= step * SearchSteps[index];
		if (!doneP)
			positionP += step * SearchSteps[index];
	}

	// Blend towards the steeper side by the texel's position along the span, when the nearer end agrees with the centre's
	// side of the edge; at least by the sub-pixel amount.
	const float distanceN = horizontalSpan ? uv.x - positionN.x : uv.y - positionN.y;
	const float distanceP = horizontalSpan ? positionP.x - uv.x : positionP.y - uv.y;
	const bool lumaMBelowZero = lumaMM < 0.0;
	const bool goodSpanN = (lumaEndN < 0.0) != lumaMBelowZero;
	const bool goodSpanP = (lumaEndP < 0.0) != lumaMBelowZero;
	const bool directionN = distanceN < distanceP;
	const float nearest = min(distanceN, distanceP);
	const bool goodSpan = directionN ? goodSpanN : goodSpanP;
	const float pixelOffset = nearest * (-1.0 / (distanceN + distanceP)) + 0.5;
	const float offset = max(goodSpan ? pixelOffset : 0.0, subpixelH);
	float2 positionM = uv;
	if (horizontalSpan)
		positionM.y += offset * lengthSign;
	else
		positionM.x += offset * lengthSign;
	return Source.SampleLevel(LinearClamp, positionM, 0.0).rgb;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 threadID : SV_DispatchThreadID)
{
	uint width = 0;
	uint height = 0;
	Destination.GetDimensions(width, height);
	const uint2 pixel = threadID.xy;
	if (pixel.x >= width || pixel.y >= height)
		return;
	Destination[pixel] = float4(Fxaa(pixel, uint2(width, height)), 1.0);
}
