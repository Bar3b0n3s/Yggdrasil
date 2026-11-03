#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <glm/glm.hpp>

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

	// The optional streams of a version 2 mesh, persisted as the payload's Streams bits (append only). Skin and Morph are
	// reserved for C23.3 (C-SKEL, C-MORPH): their sections follow the attribute stream, in bit order, and a payload that
	// sets one is refused until C23.3 defines it.
	enum class MeshStreams : uint32_t
	{
		None = 0,
		Attributes = 1U << 0, // Attributes holds one MeshVertexAttributes per vertex
		Skin = 1U << 1,       // reserved (C23.3): joint indices and weights per vertex
		Morph = 1U << 2       // reserved (C23.3): morph target deltas
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
	// Validation naming the first violation ("index 12 is 400, beyond the 300 vertices").
	[[nodiscard]] Status ValidateMeshData(const MeshData& mesh);

	// The streams `mesh` carries (MeshStreams::Attributes when Attributes is not empty): the payload's Streams word.
	[[nodiscard]] MeshStreams GetMeshStreams(const MeshData& mesh);

	// The cooked mesh payload (FormatVersion 2; Docs/Decisions/0034-m23-contract.md "Data formats"), little-endian:
	//     uint32 VertexCount; uint32 IndexCount; uint32 SubmeshCount; uint32 SlotCount;
	//     uint32 Streams (MeshStreams bits: 1 Attributes; 2 Skin and 4 Morph reserved for C23.3); uint32 Reserved (0);
	//     f32x3 BoundsMin; f32x3 BoundsMax;
	//     SubmeshCount x { uint32 IndexOffset; uint32 IndexCount; uint32 MaterialSlot; f32x3 Min; f32x3 Max; }
	//     SlotCount x { uint32 NameLength; UTF-8 Name; uint64 DefaultMaterial (0 = none); }
	//     VertexCount x MeshVertex (48 bytes); IndexCount x uint32
	//     [Streams & Attributes] VertexCount x MeshVertexAttributes (24 bytes)
	// The optional sections follow the indices in bit order. Asserts ValidateMeshData. Pure; identical meshes give identical
	// bytes.
	[[nodiscard]] Buffer SerializeMeshPayload(const MeshData& mesh);

	// Reads a payload written by SerializeMeshPayload through BinaryReader, then ValidateMeshData (an untrusted payload may
	// hold any bytes). Never asserts on data. Errors: Parse for truncation or trailing bytes, for a Streams bit this version
	// does not define (Skin and Morph until C23.3 defines their sections, and every higher bit) and for a non-zero Reserved
	// word; Validation from ValidateMeshData or for invalid UTF-8 in a slot name.
	[[nodiscard]] Result<MeshData> DeserializeMeshPayload(std::span<const std::byte> payload);

	// The complete cooked artifact (CookedHeader + payload) of `mesh`, produced by a source of version `importerVersion`.
	[[nodiscard]] Buffer CookMesh(const MeshData& mesh, uint32_t importerVersion);

	// The mesh of a cooked artifact (ReadCookedArtifact with AssetType::Mesh and FormatVersion, then the payload). Errors: as
	// ReadCookedArtifact and DeserializeMeshPayload.
	[[nodiscard]] Result<AssetRef<MeshData>> LoadCookedMesh(std::span<const std::byte> cooked);

}
