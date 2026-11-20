// Particles: the Particles pass's program (Renderer/ParticlePass.h; M33, Docs/Decisions/0048-m33-particle-look.md decisions 4
// to 6), the snapshot's particles as textured quads in world space after the sprites and the 2D lights. Each vertex
// (Shared/ParticleVertex.h) is transformed by the view's ViewProjection (ViewConstants, b0: jittered with TAA, the camera's
// Projection * View without), and its previous-render position by PreviousViewProjection for the motion. SceneDepth is a
// texture here (t1), not an attachment (§8.3 keeps it in DepthWrite and never attaches it read-only), so the fragment does
// the depth test itself: it covers nothing (a zero SV_Coverage, as Scene.slang's Mask path: a discard needs the
// shaderDemoteToHelperInvocation feature the device does not enable) where its depth is below the scene's (reverse-Z
// GreaterOrEqual, the test the QuadBatcher's pipelines made in hardware on the same SV_Position.z).
//   - The particle's alpha: the texel (the frame's, mixed toward the next frame's by Params.x for a blended flipbook) times
//     the vertex colour, its alpha times the soft fade: with Params.y (the inverse soft distance) above 0,
//     saturate((scene view depth - particle view depth) * Params.y), both linear view depths by §8.3's mappings
//     (perspective Near / depth, orthographic Far - depth (Far - Near)); 1 without, or over the cleared far plane.
//   - PSColor writes the shaded colour into SceneColor (the ParticlesAlpha pipeline blends it), and PSAdditive the same
//     colour for the ParticlesAdditive pipeline.
//   - Fog (M32's fog at M33's merge, Docs/Decisions/0048-m33-particle-look.md decision 10; Common/Fog.slang): with the view's
//     fog enabled (Fog, b1), PSColor blends the shaded colour toward the fog's colour by the fog's amount at the fragment's
//     world position and PSAdditive fades it by that amount (an additive particle's fog colour would otherwise be added
//     twice), alpha unchanged either way, as M32's quads were fogged; a view without fog runs none of it.
//   - PSVelocity writes the fragment's motion (Common/Motion.slang's ComputeMotionVector of its current and previous clip
//     positions) into SceneVelocity where the particle's alpha is at least ParticleVelocityAlpha, covering nothing elsewhere
//     (the ParticlesVelocity pipeline, recorded with TAA). Fog changes no alpha, so it moves no motion.
// Textures are sampled at their first mip level (no mips), as the QuadBatcher samples them, so a particle without a look
// shades exactly as M26's quads did.

#include "Shared/FogConstants.h"
#include "Shared/ParticleVertex.h"
#include "Shared/ViewConstants.h"

import Common.Motion;

ConstantBuffer<ViewConstants> View : register(b0, space0);
Texture2D<float4> ParticleTexture : register(t0, space0);
SamplerState ParticleSampler : register(s0, space0);
Texture2D<float> ParticleSceneDepth : register(t1, space0);
// The view's fog (FogConstants, Shared/FogConstants.h): Enabled 0 in a view without fog.
ConstantBuffer<FogConstants> Fog : register(b1, space0);

#include "Common/Fog.slang"

// ParticleVertex member by member (locations 0 to 4).
struct VertexInput
{
	float3 Position : POSITION;
	float3 PreviousPosition : TEXCOORD0;
	float4 Color : COLOR0;
	float4 TexCoords : TEXCOORD1;
	float2 Params : TEXCOORD2;
};

struct VertexOutput
{
	float4 Position : SV_Position;
	// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): the signed distance to ViewConstants::ClipPlane, which
	// clips a mirror view's particles at its glass; 1 in every other view.
	float ClipDistance : SV_ClipDistance0;
	float4 Color : COLOR0;
	float4 TexCoords : TEXCOORD0;
	float2 Params : TEXCOORD1;
	float4 CurrentClip : TEXCOORD2;
	float4 PreviousClip : TEXCOORD3;
	float3 WorldPosition : TEXCOORD4;
};

[shader("vertex")]
VertexOutput VSMain(VertexInput input)
{
	VertexOutput output;
	output.Position = mul(View.ViewProjection, float4(input.Position, 1.0));
	output.ClipDistance = dot(View.ClipPlane, float4(input.Position, 1.0));
	output.Color = input.Color;
	output.TexCoords = input.TexCoords;
	output.Params = input.Params;
	output.CurrentClip = output.Position;
	output.PreviousClip = mul(View.PreviousViewProjection, float4(input.PreviousPosition, 1.0));
	output.WorldPosition = input.Position;
	return output;
}

// The linear view depth of reverse-Z `depth` (§8.3).
float GetViewDepth(float depth)
{
	return View.ProjectionKind == ProjectionKindOrthographic ? View.Far - depth * (View.Far - View.Near) : View.Near / depth;
}

// What both entries shade: the particle's colour with its faded alpha, and whether the scene's depth at the fragment's pixel
// hides it.
struct ParticleShade
{
	float4 Color;
	bool Hidden;
};

ParticleShade ShadeParticle(VertexOutput input)
{
	const float sceneDepth = ParticleSceneDepth.Load(int3(int2(input.Position.xy), 0));
	float4 texel = ParticleTexture.SampleLevel(ParticleSampler, input.TexCoords.xy, 0.0);
	if (input.Params.x > 0.0)
		texel = lerp(texel, ParticleTexture.SampleLevel(ParticleSampler, input.TexCoords.zw, 0.0), input.Params.x);
	ParticleShade shade;
	shade.Color = texel * input.Color;
	// The cleared far plane (depth 0) is no surface to fade against.
	if (input.Params.y > 0.0 && sceneDepth > 0.0)
		shade.Color.a *= saturate((GetViewDepth(sceneDepth) - GetViewDepth(input.Position.z)) * input.Params.y);
	shade.Hidden = !(input.Position.z >= sceneDepth);
	return shade;
}

struct ColorOutput
{
	float4 Color : SV_Target0;
	uint Coverage : SV_Coverage;
};

// The shaded colour behind the view's fog: blended toward the fog's colour, or for an additive particle faded by its amount.
ColorOutput ShadeParticleColor(VertexOutput input, bool additive)
{
	const ParticleShade shade = ShadeParticle(input);
	ColorOutput output;
	output.Color = shade.Color;
	if (Fog.Enabled != 0)
	{
		const float4 fog = EvaluateViewFog(input.WorldPosition, View.ReflectionView != 0, View.ClipPlane);
		output.Color.rgb = additive ? ApplyFogAdditive(shade.Color.rgb, fog) : ApplyFog(shade.Color.rgb, fog);
	}
	output.Coverage = shade.Hidden ? 0u : 0xffffffffu;
	return output;
}

[shader("fragment")]
ColorOutput PSColor(VertexOutput input)
{
	return ShadeParticleColor(input, false);
}

[shader("fragment")]
ColorOutput PSAdditive(VertexOutput input)
{
	return ShadeParticleColor(input, true);
}

struct VelocityOutput
{
	float2 Velocity : SV_Target0;
	uint Coverage : SV_Coverage;
};

[shader("fragment")]
VelocityOutput PSVelocity(VertexOutput input)
{
	const ParticleShade shade = ShadeParticle(input);
	VelocityOutput output;
	output.Velocity = ComputeMotionVector(input.CurrentClip, input.PreviousClip, View.ViewportSize);
	output.Coverage = shade.Hidden || !(shade.Color.a >= ParticleVelocityAlpha) ? 0u : 0xffffffffu;
	return output;
}
