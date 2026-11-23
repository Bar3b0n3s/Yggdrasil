// The clip at a mirror view's glass (M42, Docs/Decisions/0059-m42-planar-reflections.md decision 3; Renderer/PlanarReflections.h).
// The vertex shaders a mirror view runs (Passes/Scene.slang's and Passes/Terrain.slang's TransformVertex, Passes/Particles.slang's
// VSMain) write SV_ClipDistance0 = GetClipDistance(View.ClipPlane, world), the signed distance of the world position from
// ViewConstants::ClipPlane: (0, 0, 0, 1) in every other view, a distance of exactly 1 that clips nothing.
#pragma once

// dot(plane, world) with SPIR-V's NoContraction on each product and sum, (x + y) + (z + w): no pipeline may fuse them, so
// the prepass and the forward pipelines, which link different vertex entries, compute bit-identical distances and clip a
// triangle that crosses the plane at the same points; otherwise a pixel the prepass covers could be clipped by the forward
// pass and keep the reflection's clear colour. Written in SPIR-V assembly for the reason Passes/Scene.slang's MultiplyExact
// gives.
float GetClipDistance(float4 plane, float4 world)
{
	return spirv_asm {
		%products:$$float4 = OpFMul $plane $world;
		OpDecorate %products NoContraction;
		%x:$$float = OpCompositeExtract %products 0;
		%y:$$float = OpCompositeExtract %products 1;
		%z:$$float = OpCompositeExtract %products 2;
		%w:$$float = OpCompositeExtract %products 3;
		%xy:$$float = OpFAdd %x %y;
		OpDecorate %xy NoContraction;
		%zw:$$float = OpFAdd %z %w;
		OpDecorate %zw NoContraction;
		%sum:$$float = OpFAdd %xy %zw;
		OpDecorate %sum NoContraction;
		result:$$float = OpCopyObject %sum
	};
}
