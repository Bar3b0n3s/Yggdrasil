#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Mesh CPU data and its cooked payload (Architecture §6.8). Produced by GltfImporter (one submesh per triangle primitive)
// and by the built-in procedural meshes (BuiltinMeshes.h); uploaded by Renderer/GpuResourceCache. The CPU copy is kept
// for mesh colliders (M11) and CPU picking and raycasts (M9).

namespace Engine {

	// One vertex (§6.8: pos f32x3, normal f32x3, tangent f32x4, uv0 f32x2): 48 bytes, no padding. Tangent.w is the bitangent
	// sign (+1 or -1): bitangent = cross(Normal, Tangent.xyz) * Tangent.w (glTF and MikkTSpace convention). Normal and
	// Tangent.xyz are unit length; TexCoord has its origin at the top left (glTF, Vulkan).
	struct MeshVertex
	{
		glm::vec3 Position{ 0.0f };
		glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
		glm::vec4 Tangent{ 1.0f, 0.0f, 0.0f, 1.0f };
		glm::vec2 TexCoord{ 0.0f };

		bool operator==(const MeshVertex&) const = default;
	};
	static_assert(sizeof(MeshVertex) == 48, "MeshVertex is the 48-byte cooked vertex of Architecture §6.8");

	// M23 (C23.1, C-MESH2; Docs/Decisions/0034-m23-contract.md decision 9): a vertex's second stream, MeshData version 2's
	// attribute stream, bound as vertex binding 1 of every mesh pipeline (the 24 mesh pipelines take both bindings): the
	// second texture coordinate set (glTF TEXCOORD_1, which Material version 2's UV sets select) and the vertex colour (glTF
	// COLOR_0, linear RGBA, which always multiplies the material's base colour, as glTF defines it). 24 bytes, no padding.
	// A mesh without the stream draws with (0, 0) and opaque white, so its image is as before within rounding: the
	// interpolated white times the base colour moves a few pixels by 1/255 (decision 21).
	struct MeshVertexAttributes
	{
		glm::vec2 TexCoord1{ 0.0f };
		glm::vec4 Color{ 1.0f };

		bool operator==(const MeshVertexAttributes&) const = default;
	};
	static_assert(sizeof(MeshVertexAttributes) == 24, "MeshVertexAttributes is MeshData version 2's 24-byte attribute vertex");

	// The optional streams of a version 2 mesh, persisted as the payload's Streams bits (append only). Their sections follow
	// the indices in bit order. Skin and Morph are defined by C23.3 (C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md
	// decision 25); every higher bit stays reserved.
	enum class MeshStreams : uint32_t
	{
		None = 0,
		Attributes = 1U << 0, // Attributes holds one MeshVertexAttributes per vertex
		Skin = 1U << 1,       // Skin holds one MeshSkinVertex per vertex, for SkinJointCount joints (C23.3)
		Morph = 1U << 2       // MorphTargets holds 1 to MaxMeshMorphTargets targets of one delta per vertex (C23.3)
	};

	// M23 (C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md decision 25): the limits of a skin and of morph
	// targets. A skin addresses at most MaxMeshSkinJoints joints (a Skeleton's limit, SkeletonData.h; a joint palette of
	// 16 KiB); a mesh has at most MaxMeshMorphTargets morph targets (the Skinning pass applies every target of non-zero
	// weight), each name at most MaxMeshMorphTargetNameBytes bytes.
	inline constexpr uint32_t MaxMeshSkinJoints = 256;
	inline constexpr uint32_t MaxMeshMorphTargets = 32;
	inline constexpr uint32_t MaxMeshMorphTargetNameBytes = 256;

	// M23 (C23.3, C-SKEL): one vertex's skin, MeshData version 2's skin section: up to four joints (indices into the skin's
	// joints, which are its Skeleton's joints in order, below MeshData::SkinJointCount) with their weights. Every weight is in
	// [0, 1] and they sum to 1 within 1e-3; a joint of weight 0 is ignored (its index is still below SkinJointCount). glTF's
	// JOINTS_0 and WEIGHTS_0, normalized at import. 24 bytes, no padding.
	struct MeshSkinVertex
	{
		std::array<uint16_t, 4> Joints{};
		glm::vec4 Weights{ 1.0f, 0.0f, 0.0f, 0.0f };

		bool operator==(const MeshSkinVertex&) const = default;
	};
	static_assert(sizeof(MeshSkinVertex) == 24, "MeshSkinVertex is MeshData version 2's 24-byte skin vertex");

	// M23 (C23.3, C-MORPH): one vertex's displacement in one morph target, in the mesh's space (glTF's target POSITION,
	// NORMAL and TANGENT; a missing attribute's deltas are zero). 36 bytes, no padding.
	struct MeshMorphDelta
	{
		glm::vec3 Position{ 0.0f };
		glm::vec3 Normal{ 0.0f };
		glm::vec3 Tangent{ 0.0f };

		bool operator==(const MeshMorphDelta&) const = default;
	};
	static_assert(sizeof(MeshMorphDelta) == 36, "MeshMorphDelta is MeshData version 2's 36-byte morph delta");

	// M23 (C23.3, C-MORPH): one morph target: its name (glTF's mesh extras "targetNames", else "Target <index>"), the weight
	// the mesh draws it at when no SkinnedMesh weight sets one, with or without a SkinnedMesh (glTF's mesh weights, else 0;
	// ResolveMorphWeights, Asset/MeshDeform.h), and one delta per vertex.
	struct MeshMorphTarget
	{
		std::string Name{};
		float DefaultWeight = 0.0f;
		std::vector<MeshMorphDelta> Deltas{};

		bool operator==(const MeshMorphTarget&) const = default;
	};

	template<>
	inline constexpr bool EnableFlagOperators<MeshStreams> = true;

	// A range of the index buffer drawn with one material slot (§6.8 {IndexOffset, IndexCount, MaterialSlot, AABB}).
	struct MeshSubmesh
	{
		uint32_t IndexOffset = 0; // first index, a multiple of 3
		uint32_t IndexCount = 0;  // a multiple of 3 (triangle lists)
		uint32_t MaterialSlot = 0;
		Aabb Bounds{}; // of the vertices this range references (frustum culling, §8.3 pass 1)

		bool operator==(const MeshSubmesh&) const = default;
	};

	// A material slot: MeshRendererComponent::Materials has one entry per slot, and an empty or null entry uses the slot's
	// default (§5.3).
	struct MeshMaterialSlot
	{
		std::string Name{};            // the glTF material's name, "Default" for built-in meshes
		AssetHandle DefaultMaterial{}; // null: the built-in Default material

		bool operator==(const MeshMaterialSlot&) const = default;
	};

	// A loaded mesh (AssetType::Mesh). Plain data; immutable once loaded (AssetRef<MeshData>).
	struct MeshData : Asset
	{
		static constexpr AssetType StaticType = AssetType::Mesh;
		// The cooked payload layout below (CookedHeader::FormatVersion). M23 (C-MESH2): version 2, the attribute stream; cooked
		// meshes are cache artifacts that GltfImporter version 2 rebuilds, so no reader keeps version 1.
		static constexpr uint16_t FormatVersion = 2;

		MeshData()
			: Asset(StaticType)
		{
		}

		std::vector<MeshVertex> Vertices{};
		// M23 (C23.1, MeshData version 2): empty (a mesh without the attribute stream), or one per vertex
		// (MeshStreams::Attributes).
		std::vector<MeshVertexAttributes> Attributes{};
		// M23 (C23.3, C-SKEL; decision 25): empty (no skin), or one per vertex (MeshStreams::Skin), each addressing joints
		// below SkinJointCount, which is 1 to MaxMeshSkinJoints with a skin and 0 without.
		std::vector<MeshSkinVertex> Skin{};
		uint32_t SkinJointCount = 0;
		// M23 (C23.3, C-MORPH; decision 25): empty (no morph targets), or 1 to MaxMeshMorphTargets targets with unique names,
		// each of one delta per vertex (MeshStreams::Morph).
		std::vector<MeshMorphTarget> MorphTargets{};
		std::vector<uint32_t> Indices{}; // uint32 triangle-list indices (§6.8)
		std::vector<MeshSubmesh> Submeshes{};
		std::vector<MeshMaterialSlot> Slots{};
		Aabb Bounds{}; // of every vertex (two meshes are equal when their payloads are: SerializeMeshPayload)
	};

	// Checks the invariants every producer guarantees and every reader re-checks on untrusted data: at least one vertex,
	// index and submesh; every index below the vertex count; each submesh's range inside the index buffer, offset and count
	// multiples of 3, slot below the slot count; every float finite; unit-length normals and tangent directions within 1e-3,
	// tangent signs +-1; each submesh's bounds equal the bounds of the vertices it references and Bounds the bounds of every
	// vertex; Attributes empty or one per vertex, each colour channel in [0, 1] (glTF's COLOR_0 range). Pure. Errors:
	// Validation naming the first violation ("index 12 is 400, beyond the 300 vertices"). M23 (C23.3): Skin empty or one per
	// vertex with SkinJointCount 1 to MaxMeshSkinJoints (0 without a skin), every joint index below it and every weight as
	// MeshSkinVertex states; MorphTargets at most MaxMeshMorphTargets, each name valid UTF-8 of 1 to
	// MaxMeshMorphTargetNameBytes bytes and unique, its DefaultWeight finite and its Deltas one per vertex, all finite.
	[[nodiscard]] Status ValidateMeshData(const MeshData& mesh);

	// The streams `mesh` carries (MeshStreams::Attributes when Attributes is not empty; since C23.3, Skin when Skin is not
	// empty and Morph when MorphTargets is not empty): the payload's Streams word.
	[[nodiscard]] MeshStreams GetMeshStreams(const MeshData& mesh);

	// The cooked mesh payload (FormatVersion 2; Docs/Decisions/0034-m23-contract.md "Data formats"), little-endian:
	//     uint32 VertexCount; uint32 IndexCount; uint32 SubmeshCount; uint32 SlotCount;
	//     uint32 Streams (MeshStreams bits: 1 Attributes, 2 Skin, 4 Morph); uint32 Reserved (0);
	//     f32x3 BoundsMin; f32x3 BoundsMax;
	//     SubmeshCount x { uint32 IndexOffset; uint32 IndexCount; uint32 MaterialSlot; f32x3 Min; f32x3 Max; }
	//     SlotCount x { uint32 NameLength; UTF-8 Name; uint64 DefaultMaterial (0 = none); }
	//     VertexCount x MeshVertex (48 bytes); IndexCount x uint32
	//     [Streams & Attributes] VertexCount x MeshVertexAttributes (24 bytes)
	//     [Streams & Skin] uint32 SkinJointCount; VertexCount x MeshSkinVertex { u16x4 Joints; f32x4 Weights; } (24 bytes)
	//     [Streams & Morph] uint32 TargetCount;
	//         TargetCount x { uint32 NameLength; UTF-8 Name; f32 DefaultWeight; VertexCount x MeshMorphDelta (36 bytes) }
	// The optional sections follow the indices in bit order. Asserts ValidateMeshData. Pure; identical meshes give identical
	// bytes. The Skin and Morph sections are C23.3's (Docs/Decisions/0034-m23-contract.md decision 25).
	[[nodiscard]] Buffer SerializeMeshPayload(const MeshData& mesh);

	// Reads a payload written by SerializeMeshPayload through BinaryReader, then ValidateMeshData (an untrusted payload may
	// hold any bytes). Never asserts on data. Errors: Parse for truncation or trailing bytes, for a Streams bit this version
	// does not define (every bit above Morph), for a count above its limit (a skin addressing 0 or more than
	// MaxMeshSkinJoints joints, a morph section of 0 or more than MaxMeshMorphTargets targets among them) and for a non-zero
	// Reserved word; Validation from ValidateMeshData or for invalid UTF-8 in a slot or morph target name.
	[[nodiscard]] Result<MeshData> DeserializeMeshPayload(std::span<const std::byte> payload);

	// The complete cooked artifact (CookedHeader + payload) of `mesh`, produced by a source of version `importerVersion`.
	[[nodiscard]] Buffer CookMesh(const MeshData& mesh, uint32_t importerVersion);

	// The mesh of a cooked artifact (ReadCookedArtifact with AssetType::Mesh and FormatVersion, then the payload). Errors: as
	// ReadCookedArtifact and DeserializeMeshPayload.
	[[nodiscard]] Result<AssetRef<MeshData>> LoadCookedMesh(std::span<const std::byte> cooked);

}
