#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetType.h"
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

// Navigation meshes (M30, Docs/Decisions/0045-m30-navigation.md decision 4): a scene's baked navigation mesh as an asset
// (AssetType::NavMesh, format version 1). `navigation.bake` (EditorCore/Navigation/NavigationBake.h) builds one mesh per
// agent type of the scene's Navigation component with Recast and writes the source file, `<scene>.navmesh`, committed with
// the scene it was baked for; NavMeshImporter (version 1) cooks it, and the Navigation component's NavMesh references it, so
// a play session builds Detour's mesh from it (Navigation module) and the debug view draws its polygons. Data only, and a
// first-party format: Detour's own tile data is built from it at load with dtCreateNavMeshData, never trusted as bytes, and
// ValidateNavMeshData checks everything dtCreateNavMeshData and Detour's queries read, so no payload that passes it makes
// Detour read outside its arrays.
//   - One NavMeshAgent per agent type, in the component's order: the agent type it was baked for (name, radius, height,
//     maximum slope and step) and its polygon mesh, as Recast's rcPolyMesh and rcPolyMeshDetail lay them out (one tile, no
//     border).
//   - Vertices are cell coordinates: vertex i sits at BoundsMin + (x * CellSize, y * CellHeight, z * CellSize) for its
//     three uint16 values (x, y, z) = Vertices[3i .. 3i + 2].
//   - A polygon is 2 * VerticesPerPolygon uint16 values: its vertex indices (at least three, then NavMeshNullIndex for the
//     unused slots) and, for its edge from vertex j to vertex j + 1, the index of the polygon across it (NavMeshNullIndex
//     for a border edge). Areas holds each polygon's area: NavMeshWalkableArea, or 1 + i for AreaCosts[i], the cost of a
//     NavModifier's area.
//   - The detail mesh: four uint32 per polygon (its first detail vertex, its vertex count, its first detail triangle and
//     its triangle count), DetailVertices in world space (three floats each; a polygon's detail vertices start with copies
//     of its own vertices) and four bytes per detail triangle (three indices into the polygon's detail vertices, then
//     Recast's edge flags).
//   - InputHash is the bake input's XXH64 (Scene/NavigationBakeInput.h's HashNavigationBakeInput: the static geometry, the
//     modifiers and the Navigation component's settings), which project.validate recomputes to report NAV_MESH_STALE.
//
// The source file is the 8-byte source header ("ENAV" then the uint32 version 1, little-endian) followed by the payload
// below; the cooked artifact (§6.8) carries the same payload after its CookedHeader.

namespace Engine {

	// An agent type's limits (the Navigation component's ranges, and the checks of ValidateNavMeshData).
	inline constexpr uint32_t MaxNavAgentTypes = 4;
	inline constexpr size_t MaxNavAgentNameLength = 64; // bytes of UTF-8
	inline constexpr float MinNavAgentRadius = 0.05f;   // metres
	inline constexpr float MaxNavAgentRadius = 10.0f;
	inline constexpr float MinNavAgentHeight = 0.1f; // metres
	inline constexpr float MaxNavAgentHeight = 20.0f;
	inline constexpr float MaxNavAgentSlope = 85.0f; // degrees, from 0
	inline constexpr float MaxNavAgentStep = 10.0f;  // metres, from 0
	// The bake's voxels (the Navigation component's CellSize and CellHeight), in metres.
	inline constexpr float MinNavCellSize = 0.01f;
	inline constexpr float MaxNavCellSize = 10.0f;
	// The largest cost a NavModifier gives its area (Detour's filter multiplies a path's length inside it), and the most areas
	// a mesh holds: Detour has 64 area ids, of which 0 is Recast's null area and 63 its walkable area.
	inline constexpr float MaxNavAreaCost = 10000.0f;
	inline constexpr uint32_t MaxNavMeshAreaCosts = 62;
	inline constexpr uint8_t NavMeshWalkableArea = 63;
	// Detour's polygons: 3 to 6 vertices (DT_VERTS_PER_POLYGON), vertex indices and counts below 0xffff, which marks an
	// unused slot or a border edge. Detour stores a polygon's neighbour as its index + 1 and reads DT_EXT_LINK (0x8000) as a
	// link to another tile (dtCreateNavMeshData, connectIntLinks), so a mesh holds at most 0x7fff polygons and every
	// neighbour index is below 0x7fff (static_asserted beside Detour, Navigation/NavigationRuntime.cpp).
	inline constexpr uint32_t NavMeshMinVerticesPerPolygon = 3;
	inline constexpr uint32_t NavMeshMaxVerticesPerPolygon = 6;
	inline constexpr uint16_t NavMeshNullIndex = 0xffff;
	inline constexpr uint32_t MaxNavMeshVertices = 0xfffe;
	inline constexpr uint32_t MaxNavMeshPolygons = 0x7fff;
	// The detail mesh: Detour stores a polygon's extra detail vertices and its triangle count in a byte each, and every
	// polygon's extra detail vertices together in a uint16 range.
	inline constexpr uint32_t MaxNavMeshDetailVerticesPerPolygon = 255; // beyond the polygon's own
	inline constexpr uint32_t MaxNavMeshDetailTrianglesPerPolygon = 255;
	inline constexpr uint32_t MaxNavMeshExtraDetailVertices = 0xffff;
	inline constexpr uint32_t MaxNavMeshDetailVertices = 1U << 20;
	inline constexpr uint32_t MaxNavMeshDetailTriangles = 1U << 20;
	// The source file's magic, its first four bytes.
	inline constexpr std::array<char, 4> NavMeshSourceMagic = { 'E', 'N', 'A', 'V' };

	// One agent type's baked mesh (see the file comment).
	struct NavMeshAgent
	{
		// The agent type it was baked for (NavAgentType of the Navigation component at bake time).
		std::string Name{};
		float Radius = 0.4f;
		float Height = 1.8f;
		float MaxSlope = 45.0f; // degrees
		float MaxStep = 0.4f;
		// The voxels and the bounds of the polygon mesh.
		float CellSize = 0.2f;
		float CellHeight = 0.1f;
		glm::vec3 BoundsMin{ 0.0f };
		glm::vec3 BoundsMax{ 0.0f };
		uint32_t VerticesPerPolygon = NavMeshMaxVerticesPerPolygon;
		std::vector<uint16_t> Vertices{};       // 3 per vertex
		std::vector<uint16_t> Polygons{};       // 2 * VerticesPerPolygon per polygon
		std::vector<uint8_t> Areas{};           // 1 per polygon
		std::vector<uint32_t> DetailMeshes{};   // 4 per polygon
		std::vector<float> DetailVertices{};    // 3 per detail vertex
		std::vector<uint8_t> DetailTriangles{}; // 4 per detail triangle

		[[nodiscard]] size_t GetVertexCount() const { return Vertices.size() / 3; }
		[[nodiscard]] size_t GetPolygonCount() const { return Areas.size(); }

		bool operator==(const NavMeshAgent&) const = default;
	};

	// A loaded navigation mesh (AssetType::NavMesh). Plain data; immutable once loaded.
	struct NavMeshData : Asset
	{
		static constexpr AssetType StaticType = AssetType::NavMesh;
		// The cooked payload layout below and the source file's version (ADR 0042's phase-5 ledger: a new format at version 1).
		static constexpr uint16_t FormatVersion = 1;

		NavMeshData()
			: Asset(StaticType)
		{
		}

		uint64_t InputHash = 0;
		std::vector<float> AreaCosts{};     // area 1 + i costs AreaCosts[i]
		std::vector<NavMeshAgent> Agents{}; // 1 to MaxNavAgentTypes, names unique
	};

	// The invariants every producer guarantees and every reader re-checks on untrusted data (the file comment's layout):
	//   - 1 to MaxNavAgentTypes agents with unique names of 1 to MaxNavAgentNameLength bytes; at most MaxNavMeshAreaCosts
	//     area costs, each finite and in [1, MaxNavAreaCost];
	//   - per agent: Radius, Height, MaxSlope and MaxStep finite and in their ranges; CellSize and CellHeight finite and in
	//     [MinNavCellSize, MaxNavCellSize]; the bounds finite with BoundsMin <= BoundsMax; VerticesPerPolygon in [3, 6];
	//   - array sizes that agree (Vertices a multiple of 3 and at most MaxNavMeshVertices vertices, 2 * VerticesPerPolygon
	//     uint16 per polygon, at most MaxNavMeshPolygons polygons, 4 DetailMeshes values per polygon, DetailVertices a
	//     multiple of 3 and DetailTriangles of 4, within their limits);
	//   - every polygon: its first three vertex indices below the vertex count, then indices below it until the first
	//     NavMeshNullIndex and only NavMeshNullIndex after; each used edge's neighbour NavMeshNullIndex or a polygon's index
	//     (an unused slot's neighbour NavMeshNullIndex); its area NavMeshWalkableArea or in [1, AreaCosts.size()];
	//   - every polygon's detail mesh: at least as many detail vertices as the polygon has vertices and at most
	//     MaxNavMeshDetailVerticesPerPolygon more, its vertex and triangle ranges inside DetailVertices and DetailTriangles,
	//     at most MaxNavMeshDetailTrianglesPerPolygon triangles, each triangle's three indices below its vertex count; the
	//     extra detail vertices of all polygons together at most MaxNavMeshExtraDetailVertices; every detail vertex finite.
	// Pure. Errors: Validation naming the first violation ("agent 'Small': polygon 12's vertex 3 is 70 of 64").
	[[nodiscard]] Status ValidateNavMeshData(const NavMeshData& mesh);

	// The payload (FormatVersion 1), little-endian, floats as their bits:
	//     uint64 InputHash
	//     uint32 AreaCostCount; AreaCostCount x f32
	//     uint32 AgentCount; per agent:
	//         string Name (uint32 byte length, UTF-8); f32 Radius, Height, MaxSlope, MaxStep, CellSize, CellHeight;
	//         f32x3 BoundsMin; f32x3 BoundsMax; uint32 VerticesPerPolygon;
	//         uint32 VertexCount; 3 x VertexCount x uint16
	//         uint32 PolygonCount; 2 x VerticesPerPolygon x PolygonCount x uint16; PolygonCount x uint8 (areas);
	//             4 x PolygonCount x uint32 (detail meshes)
	//         uint32 DetailVertexCount; 3 x DetailVertexCount x f32
	//         uint32 DetailTriangleCount; 4 x DetailTriangleCount x uint8
	// Asserts ValidateNavMeshData. Pure; equal meshes give equal bytes in every configuration (no padding), so two bakes that
	// build equal meshes write byte-identical files.
	[[nodiscard]] Buffer SerializeNavMeshPayload(const NavMeshData& mesh);

	// Reads a payload written by SerializeNavMeshPayload: every count is checked against its limit and the bytes left before
	// anything is allocated, then ValidateNavMeshData. Never asserts on data (fuzzed: the fuzz target NavMeshData,
	// "NavMeshData: 10,000 seeded mutations of a payload never crash"). Errors: Parse for truncation, a count above its limit
	// or trailing bytes; Validation from ReadString's UTF-8 check and from ValidateNavMeshData.
	[[nodiscard]] Result<NavMeshData> DeserializeNavMeshPayload(std::span<const std::byte> payload);

	// The source file of `mesh`: the source header (NavMeshSourceMagic, then uint32 FormatVersion) followed by
	// SerializeNavMeshPayload. What navigation.bake writes. Asserts ValidateNavMeshData.
	[[nodiscard]] Buffer WriteNavMeshSource(const NavMeshData& mesh);

	// Reads a source file (the importer's input, external input): the magic, then a version of exactly FormatVersion, then
	// DeserializeNavMeshPayload of the rest. Errors: Parse for a missing or wrong magic; UnsupportedVersion for another
	// version (naming it); those of DeserializeNavMeshPayload.
	[[nodiscard]] Result<NavMeshData> ReadNavMeshSource(std::span<const std::byte> source);

	// The complete cooked artifact (CookedHeader + payload, §6.8) of `mesh`, produced by a source of version
	// `importerVersion`. Asserts ValidateNavMeshData (through SerializeNavMeshPayload).
	[[nodiscard]] Buffer CookNavMesh(const NavMeshData& mesh, uint32_t importerVersion);

	// The mesh of a cooked artifact (both asset managers' loader for AssetType::NavMesh, RegisterBuiltinLoaders):
	// ReadCookedArtifact with AssetType::NavMesh and FormatVersion, then the payload. Errors: as ReadCookedArtifact and
	// DeserializeNavMeshPayload.
	[[nodiscard]] Result<AssetRef<NavMeshData>> LoadCookedNavMesh(std::span<const std::byte> cooked);

	// The world position of `agent`'s vertex `index` (below GetVertexCount(), asserted): BoundsMin + (x * CellSize,
	// y * CellHeight, z * CellSize). Pure; IEEE-exact operations only.
	[[nodiscard]] glm::vec3 GetNavMeshVertexPosition(const NavMeshAgent& agent, size_t index);

	// The number of vertices of `agent`'s polygon `polygon` (below GetPolygonCount(), asserted): the indices before its first
	// NavMeshNullIndex. Pure.
	[[nodiscard]] uint32_t GetNavMeshPolygonVertexCount(const NavMeshAgent& agent, size_t polygon);

}
