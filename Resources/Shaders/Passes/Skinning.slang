// Skinning: the Deform stage's deformation of one skinned or morphed mesh item (M23, C23.3, C-SKEL, C-MORPH;
// Docs/Decisions/0034-m23-contract.md decision 27; Renderer/SkinningPass.h). One thread per vertex, each the only reader of
// its vertex's previous deformation and the only writer of its outputs, so no atomic decides a value. It follows
// Asset/MeshDeform.h's CPU reference operation for operation:
//   1. morph: the position, normal and tangent deltas of every target of non-zero weight added in target order;
//   2. skin (Counts.y joints): the palette matrices of the vertex's four influences blended in order, an influence of weight
//      0 skipped; the position transformed by the blend, the normal and tangent by its upper 3 x 3;
//   3. the normal and the tangent direction scaled by 1 / sqrt of their squared length (a zero vector stays zero); the
//      texture coordinates and the bitangent sign kept.
// Before it writes its vertex, a thread copies the position the item's last deformation wrote there into the previous
// positions, or, on the item's first deformation (SkinningFlagFirstDeform), the new position (M26's motion vectors).
//
// The vertices are MeshVertex (48 bytes: position, normal, tangent, texture coordinates), the skin MeshSkinVertex (24
// bytes: four 16-bit joints, then four weights), the morph deltas MeshMorphDelta (36 bytes: position, normal, tangent),
// target by target; all little-endian and read as raw bytes, since none has a std430 layout.

#include "Shared/SkinningConstants.h"

[[vk::push_constant]] ConstantBuffer<SkinningConstants> Skinning;
ByteAddressBuffer SourceVertices : register(t0, space0);
ByteAddressBuffer Skin : register(t1, space0);
ByteAddressBuffer Morphs : register(t2, space0);
StructuredBuffer<float4x4> Palette : register(t3, space0);
StructuredBuffer<float> MorphWeights : register(t4, space0);
RWByteAddressBuffer Vertices : register(u0, space0);
RWStructuredBuffer<float4> PreviousPositions : register(u1, space0);

static const uint SkinningGroupSize = 64;
static const uint MeshVertexBytes = 48;
static const uint MeshSkinVertexBytes = 24;
static const uint MeshMorphDeltaBytes = 36;

// `direction` at unit length, as MeshDeform.cpp scales it; a zero vector stays zero.
float3 NormalizeDeformedDirection(float3 direction)
{
	const float lengthSquared = dot(direction, direction);
	if (!(lengthSquared > 0.0))
		return direction;
	return direction * (1.0 / sqrt(lengthSquared));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void CSMain(uint3 thread : SV_DispatchThreadID)
{
	const uint vertexCount = Skinning.Counts.x;
	const uint jointCount = Skinning.Counts.y;
	const uint targetCount = Skinning.Counts.z;
	const uint index = thread.y * (SkinningRowGroups * SkinningGroupSize) + thread.x;
	if (index >= vertexCount)
		return;

	const uint vertexOffset = index * MeshVertexBytes;
	float3 position = asfloat(SourceVertices.Load3(vertexOffset));
	float3 normal = asfloat(SourceVertices.Load3(vertexOffset + 12));
	const float4 tangent = asfloat(SourceVertices.Load4(vertexOffset + 24));
	const uint2 texCoord = SourceVertices.Load2(vertexOffset + 40);
	float3 tangentDirection = tangent.xyz;

	// 1. Morph, in target order.
	for (uint target = 0; target < targetCount; ++target)
	{
		const float weight = MorphWeights[target];
		if (weight == 0.0)
			continue;
		const uint deltaOffset = (target * vertexCount + index) * MeshMorphDeltaBytes;
		position += weight * asfloat(Morphs.Load3(deltaOffset));
		normal += weight * asfloat(Morphs.Load3(deltaOffset + 12));
		tangentDirection += weight * asfloat(Morphs.Load3(deltaOffset + 24));
	}

	// 2. Skin: the influences' palette matrices blended in order. The cache's skins address joints below the count, which
	// the clamp only keeps inside the palette.
	if (jointCount > 0)
	{
		const uint skinOffset = index * MeshSkinVertexBytes;
		const uint2 packedJoints = Skin.Load2(skinOffset);
		const float4 weights = asfloat(Skin.Load4(skinOffset + 8));
		const uint4 joints = uint4(packedJoints.x & 0xFFFF, packedJoints.x >> 16, packedJoints.y & 0xFFFF, packedJoints.y >> 16);
		float4x4 blended = float4x4(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
		for (uint influence = 0; influence < 4; ++influence)
		{
			const float weight = weights[influence];
			if (weight == 0.0)
				continue;
			blended += weight * Palette[min(joints[influence], jointCount - 1)];
		}
		position = mul(blended, float4(position, 1.0)).xyz;
		normal = mul(blended, float4(normal, 0.0)).xyz;
		tangentDirection = mul(blended, float4(tangentDirection, 0.0)).xyz;
	}

	// 3. The directions at unit length.
	normal = NormalizeDeformedDirection(normal);
	tangentDirection = NormalizeDeformedDirection(tangentDirection);

	// The previous positions before the vertex is overwritten: the item's last deformation, or this one on its first.
	const float3 previous = (Skinning.Counts.w & SkinningFlagFirstDeform) != 0 ? position : asfloat(Vertices.Load3(vertexOffset));
	PreviousPositions[index] = float4(previous, 1.0);
	Vertices.Store3(vertexOffset, asuint(position));
	Vertices.Store3(vertexOffset + 12, asuint(normal));
	Vertices.Store4(vertexOffset + 24, asuint(float4(tangentDirection, tangent.w)));
	Vertices.Store2(vertexOffset + 40, texCoord);
}
