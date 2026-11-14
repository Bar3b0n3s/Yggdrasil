// Text: §8.3 pass 14 (Renderer/TextRenderer.h, Architecture §8.10): signed-distance-field glyph quads from a font's R8 atlas
// (FontData: 128 is the outline, larger values inside), drawn into the final LDR target. The vertex shader places each vertex
// by its mode (Shared/TextVertex.h: screen, world, billboard or debug label); the fragment shader turns the distance into
// coverage with a smoothstep over the field's change across one screen pixel, so edges stay one to two pixels soft at any
// size. That change is the field's screen-space derivative (fwidth); outside the outline it is never less than the field's
// step per atlas pixel (the outline's value over the font's Spread, the push constants) times the atlas pixels one screen
// pixel spans. Across a stroke thinner than a pixel the field rises and falls between two pixel centres, so its derivative
// over a 2 x 2 quad cancels; without that floor the smoothstep turned into a hard step there, and the pixels on both sides,
// whose centres lie just outside the outline, took no coverage at all: the stroke vanished (M25's friction F01,
// Docs/Decisions/0036-m25-tetris-pixel-art.md: 9 and 10 pixel UI text under a pixel-perfect camera lost its stems). Inside
// the outline a cancelled derivative only sharpens the edge towards full coverage, so the floor is left out there and no
// pixel takes less coverage than before.
// The target holds display-encoded values (§8.9): the linear colour is encoded with the sRGB OETF and blended with straight
// alpha. One program for both pipelines: depth-tested against SceneDepth (world texts, tested labels) or on top.

#include "Shared/TextVertex.h"
#include "Shared/ViewConstants.h"

import Common.Color;

ConstantBuffer<ViewConstants> View : register(b0, space0);
Texture2D<float> Atlas : register(t0, space0);
SamplerState AtlasSampler : register(s0, space0);
[[vk::push_constant]] ConstantBuffer<TextConstants> Font;

// The atlas value of an outline (FontImporter: stb_truetype's on-edge value 128 of 255).
static const float SdfEdgeValue = 128.0 / 255.0;
// The smallest smoothstep half-width, so a flat field (zero derivative) still gives a defined coverage.
static const float MinEdgeWidth = 1.0e-4;

struct VertexInput
{
	float3 Position : POSITION;
	float2 Corner : CORNER;
	float2 TexCoord : TEXCOORD0;
	float4 Color : COLOR0;
	uint Mode : MODE;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
	float4 Color : COLOR0;
};

[shader("vertex")]
VertexOutput VSMain(VertexInput input)
{
	VertexOutput output;
	output.TexCoord = input.TexCoord;
	output.Color = float4(LinearToSrgb(input.Color.rgb), saturate(input.Color.a));
	if (input.Mode == TextVertexScreen)
	{
		output.Position = float4(input.Position.xy, 0.0, 1.0);
		return output;
	}

	// World, billboard and label vertices all go through the same view and projection products, so a billboard that
	// happens to face the camera rasterizes exactly like the same text laid out in world space.
	float4 viewPosition = mul(View.View, float4(input.Position, 1.0));
	if (input.Mode == TextVertexBillboard)
		viewPosition.xy += input.Corner;
	if (input.Mode == TextVertexLabel && viewPosition.z >= 0.0)
	{
		// On or behind the camera plane: every vertex of the label lands on the same point outside the clip volume.
		output.Position = float4(2.0, 2.0, 0.0, 1.0);
		return output;
	}
	float4 clip = mul(View.Projection, viewPosition);
	if (input.Mode == TextVertexLabel)
		clip.xy += input.Corner * clip.w;
	output.Position = clip;
	return output;
}

// The least change of the distance field across one screen pixel: the field's step per atlas pixel (SdfEdgeValue over the
// font's Spread) times the atlas pixels one screen pixel spans along the direction it spans fewest, the smaller singular
// value of the screen-space Jacobian of the atlas coordinates (in atlas pixels). Wherever the field is a true distance its
// fwidth is at least this; across a stroke's middle it cancels. At most SdfEdgeValue, so the field's clamp to 0 (Spread
// atlas pixels or more outside) never takes coverage, however small the text.
float GetFieldStepPerPixel(float2 texCoord)
{
	uint atlasWidth;
	uint atlasHeight;
	Atlas.GetDimensions(atlasWidth, atlasHeight);
	const float2 atlasSize = float2(float(atlasWidth), float(atlasHeight));
	const float2 alongX = ddx(texCoord) * atlasSize;
	const float2 alongY = ddy(texCoord) * atlasSize;
	const float sum = dot(alongX, alongX) + dot(alongY, alongY);
	const float determinant = abs(alongX.x * alongY.y - alongX.y * alongY.x);
	const float largestSquared = 0.5 * (sum + sqrt(max(sum * sum - 4.0 * determinant * determinant, 0.0)));
	const float smallest = largestSquared > 0.0 ? determinant * rsqrt(largestSquared) : 0.0;
	return min(smallest * SdfEdgeValue / Font.Spread, SdfEdgeValue);
}

[shader("fragment")]
float4 PSMain(VertexOutput input) : SV_Target0
{
	const float field = Atlas.Sample(AtlasSampler, input.TexCoord);
	// Both derivatives are taken before the outline's test, in uniform control flow.
	const float derivative = fwidth(field);
	const float fieldStep = GetFieldStepPerPixel(input.TexCoord);
	const float width = max(field < SdfEdgeValue ? max(derivative, fieldStep) : derivative, MinEdgeWidth);
	const float coverage = smoothstep(SdfEdgeValue - width, SdfEdgeValue + width, field);
	return float4(input.Color.rgb, input.Color.a * coverage);
}
