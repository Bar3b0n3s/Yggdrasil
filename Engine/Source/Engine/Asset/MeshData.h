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
// for mesh colliders (M11) and CPU picking and raycasts (M9), which read level 0 only. M46 (version 3): the wind stream
// and up to four further levels of detail, authored in the glTF or generated at import, which only the renderer reads.

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

	// The optional streams of a mesh, persisted as the payload's Streams bits (append only). Their sections follow the
	// indices in bit order. Skin and Morph are defined by C23.3 (C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md
	// decision 25), Wind by M46 (MeshData version 3; Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision
	// 3A); every higher bit stays reserved.
	enum class MeshStreams : uint32_t
	{
		None = 0,
		Attributes = 1U << 0, // Attributes holds one MeshVertexAttributes per vertex
		Skin = 1U << 1,       // Skin holds one MeshSkinVertex per vertex, for SkinJointCount joints (C23.3)
		Morph = 1U << 2,      // MorphTargets holds 1 to MaxMeshMorphTargets targets of one delta per vertex (C23.3)
		Wind = 1U << 3        // Wind holds one MeshVertexWind per vertex (M46)
	};

	// M46 (MeshData version 3; Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3A): one vertex's wind
	// weights, the mesh's wind stream (glTF's application attribute _WIND, clamped to [0, 1] and stored as unorm8), bound as
	// vertex binding 3 of every mesh pipeline: Branch scales the branch sway (MaterialWind::Branch), Phase offsets the
	// branch's wave (a fraction of its period), Flutter scales the flutter along the normal (MaterialWind::Flutter), and
	// BendMask holds the vertex still against the bend (1: no lean). A mesh without the stream draws with all four 0: it
	// bends by height and neither sways nor flutters. 4 bytes, no padding.
	struct MeshVertexWind
	{
		uint8_t Branch = 0;
		uint8_t Phase = 0;
		uint8_t Flutter = 0;
		uint8_t BendMask = 0;

		bool operator==(const MeshVertexWind&) const = default;
	};
	static_assert(sizeof(MeshVertexWind) == 4, "MeshVertexWind is MeshData version 3's 4-byte wind vertex");

	// M46 (MeshData version 3): the levels of detail a mesh holds, level 0 (the mesh's own Indices and Submeshes) included,
	// so a mesh has at most MaxMeshLods - 1 further levels (MeshData::Lods).
	inline constexpr uint32_t MaxMeshLods = 5;

	// M46 (MeshData version 3): one submesh's index range in a further level, into MeshData::LodIndices. A level's submesh i
	// draws with level 0's submesh i's material slot.
	struct MeshLodRange
	{
		uint32_t IndexOffset = 0; // first index into LodIndices, a multiple of 3
		uint32_t IndexCount = 0;  // a multiple of 3 (triangle lists); 0 draws nothing for the submesh at this level

		bool operator==(const MeshLodRange&) const = default;
	};

	// M46 (MeshData version 3): one further level of detail. Level k + 1 (Lods[k]) takes over from level k once the item's
	// biased screen size (Renderer/MeshLod.h) falls below ScreenSize; the sizes decrease strictly from level to level, each
	// in (0, 1]. Submeshes holds one range per submesh of level 0.
	struct MeshLod
	{
		float ScreenSize = 0.5f;
		std::vector<MeshLodRange> Submeshes{};

		bool operator==(const MeshLod&) const = default;
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
		// meshes are cache artifacts that GltfImporter version 2 rebuilds, so no reader keeps version 1. M46
		// (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3A): version 3, the wind stream and the
		// further levels; GltfImporter version 4 and the format check rebuild every cooked mesh, so no reader keeps version 2.
		static constexpr uint16_t FormatVersion = 3;

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

		// M46 (MeshData version 3; decision 3A). Vertices, the streams above, Indices, Submeshes and Bounds are level 0, which
		// physics, the navigation bake, raycasts and CPU picking read; the members below only the renderer reads.
		// Wind: empty (a mesh without the wind stream), or one per vertex (MeshStreams::Wind).
		std::vector<MeshVertexWind> Wind{};
		// The further levels, 0 to MaxMeshLods - 1, each with one range per submesh into LodIndices.
		std::vector<MeshLod> Lods{};
		// The authored levels' own vertices, addressed by LodIndices after level 0's: index Vertices.size() + i is
		// LodVertices[i]. A generated level indexes level 0's vertices only, as every level of a skinned or morphed mesh does
		// (its LodVertices are empty). LodAttributes and LodWind hold one entry per LodVertex when the mesh has the attribute
		// or the wind stream, and are empty otherwise.
		std::vector<MeshVertex> LodVertices{};
		std::vector<MeshVertexAttributes> LodAttributes{};
		std::vector<MeshVertexWind> LodWind{};
		std::vector<uint32_t> LodIndices{}; // uint32 triangle-list indices of every further level, each below Vertices.size() + LodVertices.size()
	};

	// Checks the invariants every producer guarantees and every reader re-checks on untrusted data: at least one vertex,
	// index and submesh; every index below the vertex count; each submesh's range inside the index buffer, offset and count
	// multiples of 3, slot below the slot count; every float finite; unit-length normals and tangent directions within 1e-3,
	// tangent signs +-1; each submesh's bounds equal the bounds of the vertices it references and Bounds the bounds of every
	// vertex; Attributes empty or one per vertex, each colour channel in [0, 1] (glTF's COLOR_0 range). Pure. Errors:
	// Validation naming the first violation ("index 12 is 400, beyond the 300 vertices"). M23 (C23.3): Skin empty or one per
	// vertex with SkinJointCount 1 to MaxMeshSkinJoints (0 without a skin), every joint index below it and every weight as
	// MeshSkinVertex states; MorphTargets at most MaxMeshMorphTargets, each name valid UTF-8 of 1 to
	// MaxMeshMorphTargetNameBytes bytes and unique, its DefaultWeight finite and its Deltas one per vertex, all finite. M46
	// (version 3): Wind empty or one per vertex; at most MaxMeshLods - 1 Lods, their ScreenSize finite, in (0, 1] and
	// strictly decreasing, each with one range per submesh, every range whole triangles inside LodIndices; every LodIndices
	// entry below Vertices.size() + LodVertices.size(); LodVertices empty for a skinned or morphed mesh and for a mesh
	// without levels, each one as Vertices' are checked; LodAttributes one per LodVertex with the attribute stream (else
	// empty), each as Attributes' are checked, and LodWind one per LodVertex with the wind stream (else empty); LodIndices
	// empty without levels.
	[[nodiscard]] Status ValidateMeshData(const MeshData& mesh);

	// The streams `mesh` carries (MeshStreams::Attributes when Attributes is not empty; since C23.3, Skin when Skin is not
	// empty and Morph when MorphTargets is not empty; since M46, Wind when Wind is not empty): the payload's Streams word.
	[[nodiscard]] MeshStreams GetMeshStreams(const MeshData& mesh);

	// The cooked mesh payload (FormatVersion 3; Docs/Decisions/0034-m23-contract.md "Data formats", M46's
	// Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3A), little-endian:
	//     uint32 VertexCount; uint32 IndexCount; uint32 SubmeshCount; uint32 SlotCount;
	//     uint32 Streams (MeshStreams bits: 1 Attributes, 2 Skin, 4 Morph, 8 Wind); uint32 LodCount (0 to MaxMeshLods - 1);
	//     f32x3 BoundsMin; f32x3 BoundsMax;
	//     SubmeshCount x { uint32 IndexOffset; uint32 IndexCount; uint32 MaterialSlot; f32x3 Min; f32x3 Max; }
	//     SlotCount x { uint32 NameLength; UTF-8 Name; uint64 DefaultMaterial (0 = none); }
	//     VertexCount x MeshVertex (48 bytes); IndexCount x uint32
	//     [Streams & Attributes] VertexCount x MeshVertexAttributes (24 bytes)
	//     [Streams & Skin] uint32 SkinJointCount; VertexCount x MeshSkinVertex { u16x4 Joints; f32x4 Weights; } (24 bytes)
	//     [Streams & Morph] uint32 TargetCount;
	//         TargetCount x { uint32 NameLength; UTF-8 Name; f32 DefaultWeight; VertexCount x MeshMorphDelta (36 bytes) }
	//     [Streams & Wind] VertexCount x MeshVertexWind { u8 Branch; u8 Phase; u8 Flutter; u8 BendMask; } (4 bytes)
	//     [LodCount > 0] uint32 LodVertexCount; uint32 LodIndexCount;
	//         LodCount x { f32 ScreenSize; SubmeshCount x { uint32 IndexOffset; uint32 IndexCount; } }
	//         LodVertexCount x MeshVertex; [Streams & Attributes] LodVertexCount x MeshVertexAttributes;
	//         [Streams & Wind] LodVertexCount x MeshVertexWind; LodIndexCount x uint32
	// The optional sections follow the indices in bit order, then the levels' section. Version 2's Reserved word (always 0)
	// became LodCount, so a version 2 payload's layout is a version 3 payload without wind and levels. Asserts
	// ValidateMeshData. Pure; identical meshes give identical bytes. The Skin and Morph sections are C23.3's
	// (Docs/Decisions/0034-m23-contract.md decision 25).
	[[nodiscard]] Buffer SerializeMeshPayload(const MeshData& mesh);

	// Reads a payload written by SerializeMeshPayload through BinaryReader, then ValidateMeshData (an untrusted payload may
	// hold any bytes). Never asserts on data. Errors: Parse for truncation or trailing bytes, for a Streams bit this version
	// does not define (every bit above Wind), for a count above its limit (a skin addressing 0 or more than
	// MaxMeshSkinJoints joints, a morph section of 0 or more than MaxMeshMorphTargets targets among them, a LodCount above
	// MaxMeshLods - 1); Validation from ValidateMeshData or for invalid UTF-8 in a slot or morph target name.
	[[nodiscard]] Result<MeshData> DeserializeMeshPayload(std::span<const std::byte> payload);

	// The complete cooked artifact (CookedHeader + payload) of `mesh`, produced by a source of version `importerVersion`.
	[[nodiscard]] Buffer CookMesh(const MeshData& mesh, uint32_t importerVersion);

	// The mesh of a cooked artifact (ReadCookedArtifact with AssetType::Mesh and FormatVersion, then the payload). Errors: as
	// ReadCookedArtifact and DeserializeMeshPayload.
	[[nodiscard]] Result<AssetRef<MeshData>> LoadCookedMesh(std::span<const std::byte> cooked);

}
