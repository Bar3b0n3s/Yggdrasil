#include "EnginePCH.h"
#include "Engine/Asset/NavMeshData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <set>
#include <string_view>
#include <utility>

// The navigation mesh's formats (NavMeshData.h; Docs/Decisions/0045-m30-navigation.md decision 4): the payload, its
// validation and the `.navmesh` source file that navigation.bake writes and NavMeshImporter reads.

namespace Engine {

	namespace Utils {

		// The source header: the magic and the uint32 version.
		constexpr size_t NavMeshSourceHeaderBytes = NavMeshSourceMagic.size() + sizeof(uint32_t);

		[[nodiscard]] static bool IsFiniteIn(float value, float minimum, float maximum)
		{
			return std::isfinite(value) && value >= minimum && value <= maximum;
		}

		[[nodiscard]] static bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// The agent's own fields and array sizes, before any index is read.
		[[nodiscard]] static Status ValidateAgentShape(const NavMeshAgent& agent)
		{
			if (!IsFiniteIn(agent.Radius, MinNavAgentRadius, MaxNavAgentRadius))
				return MakeError(ErrorCode::Validation, "its Radius is {}; an agent's radius is in [{}, {}] metres", agent.Radius, MinNavAgentRadius, MaxNavAgentRadius);
			if (!IsFiniteIn(agent.Height, MinNavAgentHeight, MaxNavAgentHeight))
				return MakeError(ErrorCode::Validation, "its Height is {}; an agent's height is in [{}, {}] metres", agent.Height, MinNavAgentHeight, MaxNavAgentHeight);
			if (!IsFiniteIn(agent.MaxSlope, 0.0f, MaxNavAgentSlope))
				return MakeError(ErrorCode::Validation, "its MaxSlope is {}; an agent's maximum slope is in [0, {}] degrees", agent.MaxSlope, MaxNavAgentSlope);
			if (!IsFiniteIn(agent.MaxStep, 0.0f, MaxNavAgentStep))
				return MakeError(ErrorCode::Validation, "its MaxStep is {}; an agent's maximum step is in [0, {}] metres", agent.MaxStep, MaxNavAgentStep);
			if (!IsFiniteIn(agent.CellSize, MinNavCellSize, MaxNavCellSize) || !IsFiniteIn(agent.CellHeight, MinNavCellSize, MaxNavCellSize))
			{
				return MakeError(ErrorCode::Validation, "its cells are {} x {} metres; each is in [{}, {}]", agent.CellSize, agent.CellHeight, MinNavCellSize,
					MaxNavCellSize);
			}
			if (!IsFinite(agent.BoundsMin) || !IsFinite(agent.BoundsMax) || glm::any(glm::greaterThan(agent.BoundsMin, agent.BoundsMax)))
				return MakeError(ErrorCode::Validation, "its bounds are not finite, or their minimum exceeds their maximum");
			if (agent.VerticesPerPolygon < NavMeshMinVerticesPerPolygon || agent.VerticesPerPolygon > NavMeshMaxVerticesPerPolygon)
			{
				return MakeError(ErrorCode::Validation, "its polygons have {} vertex slots; a polygon has {} to {}", agent.VerticesPerPolygon,
					NavMeshMinVerticesPerPolygon, NavMeshMaxVerticesPerPolygon);
			}
			if (agent.Vertices.size() % 3 != 0 || agent.GetVertexCount() > MaxNavMeshVertices)
				return MakeError(ErrorCode::Validation, "it holds {} vertex coordinates; a mesh holds 3 per vertex and at most {} vertices", agent.Vertices.size(), MaxNavMeshVertices);
			const size_t polygons = agent.GetPolygonCount();
			if (polygons > MaxNavMeshPolygons)
				return MakeError(ErrorCode::Validation, "it holds {} polygons; a mesh holds at most {}", polygons, MaxNavMeshPolygons);
			if (agent.Polygons.size() != polygons * 2 * agent.VerticesPerPolygon)
			{
				return MakeError(ErrorCode::Validation, "it holds {} polygon values for {} polygons of {} slots; each polygon takes {}", agent.Polygons.size(), polygons,
					agent.VerticesPerPolygon, 2 * agent.VerticesPerPolygon);
			}
			if (agent.DetailMeshes.size() != polygons * 4)
				return MakeError(ErrorCode::Validation, "it holds {} detail-mesh values for {} polygons; each polygon takes 4", agent.DetailMeshes.size(), polygons);
			if (agent.DetailVertices.size() % 3 != 0 || agent.DetailVertices.size() / 3 > MaxNavMeshDetailVertices)
			{
				return MakeError(ErrorCode::Validation, "it holds {} detail vertex coordinates; a mesh holds 3 per detail vertex and at most {} detail vertices",
					agent.DetailVertices.size(), MaxNavMeshDetailVertices);
			}
			if (agent.DetailTriangles.size() % 4 != 0 || agent.DetailTriangles.size() / 4 > MaxNavMeshDetailTriangles)
			{
				return MakeError(ErrorCode::Validation, "it holds {} detail triangle bytes; a mesh holds 4 per detail triangle and at most {} detail triangles",
					agent.DetailTriangles.size(), MaxNavMeshDetailTriangles);
			}
			for (size_t index = 0; index < agent.DetailVertices.size(); ++index)
			{
				if (!std::isfinite(agent.DetailVertices[index]))
					return MakeError(ErrorCode::Validation, "detail vertex {}'s coordinate {} is not finite", index / 3, index % 3);
			}
			return {};
		}

		// Polygon `polygon`'s vertices, neighbours and area (ValidateAgentShape passed).
		[[nodiscard]] static Status ValidatePolygon(const NavMeshAgent& agent, size_t polygon, size_t areaCostCount)
		{
			const size_t slots = agent.VerticesPerPolygon;
			const uint16_t* values = agent.Polygons.data() + polygon * 2 * slots;
			const size_t vertices = agent.GetVertexCount();
			bool ended = false;
			for (size_t slot = 0; slot < slots; ++slot)
			{
				const uint16_t vertex = values[slot];
				const uint16_t neighbour = values[slots + slot];
				if (vertex == NavMeshNullIndex)
				{
					if (slot < NavMeshMinVerticesPerPolygon)
						return MakeError(ErrorCode::Validation, "polygon {} has {} vertices; a polygon has at least {}", polygon, slot, NavMeshMinVerticesPerPolygon);
					ended = true;
				}
				else if (ended)
				{
					return MakeError(ErrorCode::Validation, "polygon {}'s slot {} names vertex {} after an unused slot", polygon, slot, vertex);
				}
				else if (vertex >= vertices)
				{
					return MakeError(ErrorCode::Validation, "polygon {}'s vertex {} is {} of {}", polygon, slot, vertex, vertices);
				}
				if (ended && neighbour != NavMeshNullIndex)
					return MakeError(ErrorCode::Validation, "polygon {}'s unused slot {} has the neighbour {}", polygon, slot, neighbour);
				if (!ended && neighbour != NavMeshNullIndex && neighbour >= agent.GetPolygonCount())
					return MakeError(ErrorCode::Validation, "polygon {}'s edge {} has the neighbour {} of {} polygons", polygon, slot, neighbour, agent.GetPolygonCount());
			}
			const uint8_t area = agent.Areas[polygon];
			if (area != NavMeshWalkableArea && (area == 0 || area > areaCostCount))
			{
				return MakeError(ErrorCode::Validation, "polygon {}'s area is {}; an area is {} or one of the {} area costs (1 to {})", polygon, area,
					NavMeshWalkableArea, areaCostCount, areaCostCount);
			}
			return {};
		}

		// Polygon `polygon`'s detail mesh (ValidatePolygon passed); adds its extra detail vertices to `extraVertices`.
		[[nodiscard]] static Status ValidateDetailMesh(const NavMeshAgent& agent, size_t polygon, size_t& extraVertices)
		{
			const uint32_t* detail = agent.DetailMeshes.data() + polygon * 4;
			const uint64_t vertexBase = detail[0];
			const uint64_t vertexCount = detail[1];
			const uint64_t triangleBase = detail[2];
			const uint64_t triangleCount = detail[3];
			const uint32_t own = GetNavMeshPolygonVertexCount(agent, polygon);
			if (vertexCount < own || vertexCount - own > MaxNavMeshDetailVerticesPerPolygon)
			{
				return MakeError(ErrorCode::Validation, "polygon {}'s detail mesh has {} vertices; it has the polygon's {} and at most {} more", polygon, vertexCount, own,
					MaxNavMeshDetailVerticesPerPolygon);
			}
			if (vertexBase + vertexCount > agent.DetailVertices.size() / 3)
			{
				return MakeError(ErrorCode::Validation, "polygon {}'s detail vertices {} to {} are outside the {} detail vertices", polygon, vertexBase,
					vertexBase + vertexCount, agent.DetailVertices.size() / 3);
			}
			if (triangleCount > MaxNavMeshDetailTrianglesPerPolygon || triangleBase + triangleCount > agent.DetailTriangles.size() / 4)
			{
				return MakeError(ErrorCode::Validation, "polygon {}'s detail triangles {} to {} are outside the {} detail triangles, or more than {}", polygon,
					triangleBase, triangleBase + triangleCount, agent.DetailTriangles.size() / 4, MaxNavMeshDetailTrianglesPerPolygon);
			}
			for (uint64_t triangle = triangleBase; triangle < triangleBase + triangleCount; ++triangle)
			{
				for (size_t corner = 0; corner < 3; ++corner)
				{
					const uint8_t index = agent.DetailTriangles[static_cast<size_t>(triangle) * 4 + corner];
					if (index >= vertexCount)
						return MakeError(ErrorCode::Validation, "polygon {}'s detail triangle {} names vertex {} of {}", polygon, triangle, index, vertexCount);
				}
			}
			extraVertices += static_cast<size_t>(vertexCount - own);
			if (extraVertices > MaxNavMeshExtraDetailVertices)
				return MakeError(ErrorCode::Validation, "the detail meshes have more than {} vertices beyond their polygons'", MaxNavMeshExtraDetailVertices);
			return {};
		}

		[[nodiscard]] static Status ValidateAgent(const NavMeshAgent& agent, size_t areaCostCount)
		{
			ENGINE_TRY(ValidateAgentShape(agent));
			size_t extraVertices = 0;
			for (size_t polygon = 0; polygon < agent.GetPolygonCount(); ++polygon)
			{
				ENGINE_TRY(ValidatePolygon(agent, polygon, areaCostCount));
				ENGINE_TRY(ValidateDetailMesh(agent, polygon, extraVertices));
			}
			return {};
		}

		// The payload of a valid mesh (no assertion: the callers assert).
		[[nodiscard]] static Buffer WritePayload(BinaryWriter& writer, const NavMeshData& mesh)
		{
			writer.WriteU64(mesh.InputHash);
			writer.WriteU32(static_cast<uint32_t>(mesh.AreaCosts.size()));
			for (const float cost : mesh.AreaCosts)
				writer.WriteF32(cost);
			writer.WriteU32(static_cast<uint32_t>(mesh.Agents.size()));
			for (const NavMeshAgent& agent : mesh.Agents)
			{
				writer.WriteString(agent.Name);
				for (const float value : { agent.Radius, agent.Height, agent.MaxSlope, agent.MaxStep, agent.CellSize, agent.CellHeight })
					writer.WriteF32(value);
				for (const glm::vec3& bound : { agent.BoundsMin, agent.BoundsMax })
				{
					for (glm::length_t axis = 0; axis < 3; ++axis)
						writer.WriteF32(bound[axis]);
				}
				writer.WriteU32(agent.VerticesPerPolygon);
				writer.WriteU32(static_cast<uint32_t>(agent.GetVertexCount()));
				for (const uint16_t value : agent.Vertices)
					writer.WriteU16(value);
				writer.WriteU32(static_cast<uint32_t>(agent.GetPolygonCount()));
				for (const uint16_t value : agent.Polygons)
					writer.WriteU16(value);
				for (const uint8_t area : agent.Areas)
					writer.WriteU8(area);
				for (const uint32_t value : agent.DetailMeshes)
					writer.WriteU32(value);
				writer.WriteU32(static_cast<uint32_t>(agent.DetailVertices.size() / 3));
				for (const float value : agent.DetailVertices)
					writer.WriteF32(value);
				writer.WriteU32(static_cast<uint32_t>(agent.DetailTriangles.size() / 4));
				for (const uint8_t value : agent.DetailTriangles)
					writer.WriteU8(value);
			}
			return writer.TakeBuffer();
		}

		// `count` elements of `elementBytes` each must fit in what is left of `reader` (checked before any allocation).
		[[nodiscard]] static Status CheckRemaining(const BinaryReader& reader, uint64_t count, uint64_t elementBytes, std::string_view what)
		{
			const uint64_t needed = count * elementBytes;
			if (needed > reader.GetRemaining())
				return MakeError(ErrorCode::Parse, "a navigation mesh payload's {} need {} bytes, {} are left", what, needed, reader.GetRemaining());
			return {};
		}

		[[nodiscard]] static Status CheckCount(uint64_t count, uint64_t limit, std::string_view what)
		{
			if (count > limit)
				return MakeError(ErrorCode::Parse, "a navigation mesh payload names {} {}; at most {} are allowed", count, what, limit);
			return {};
		}

		[[nodiscard]] static Result<NavMeshAgent> ReadAgent(BinaryReader& reader)
		{
			NavMeshAgent agent;
			ENGINE_TRY_ASSIGN(agent.Name, reader.ReadString());
			for (float* value : { &agent.Radius, &agent.Height, &agent.MaxSlope, &agent.MaxStep, &agent.CellSize, &agent.CellHeight })
			{
				ENGINE_TRY_ASSIGN(*value, reader.ReadF32());
			}
			for (glm::vec3* bound : { &agent.BoundsMin, &agent.BoundsMax })
			{
				for (glm::length_t axis = 0; axis < 3; ++axis)
				{
					ENGINE_TRY_ASSIGN((*bound)[axis], reader.ReadF32());
				}
			}
			ENGINE_TRY_ASSIGN(agent.VerticesPerPolygon, reader.ReadU32());
			ENGINE_TRY(CheckCount(agent.VerticesPerPolygon, NavMeshMaxVerticesPerPolygon, "vertex slots per polygon"));

			ENGINE_TRY_ASSIGN(const uint32_t vertices, reader.ReadU32());
			ENGINE_TRY(CheckCount(vertices, MaxNavMeshVertices, "vertices"));
			ENGINE_TRY(CheckRemaining(reader, vertices, 3 * sizeof(uint16_t), "vertices"));
			agent.Vertices.resize(static_cast<size_t>(vertices) * 3);
			for (uint16_t& value : agent.Vertices)
			{
				ENGINE_TRY_ASSIGN(value, reader.ReadU16());
			}

			ENGINE_TRY_ASSIGN(const uint32_t polygons, reader.ReadU32());
			ENGINE_TRY(CheckCount(polygons, MaxNavMeshPolygons, "polygons"));
			const uint64_t polygonBytes = 2 * uint64_t{ agent.VerticesPerPolygon } * sizeof(uint16_t) + sizeof(uint8_t) + 4 * sizeof(uint32_t);
			ENGINE_TRY(CheckRemaining(reader, polygons, polygonBytes, "polygons"));
			agent.Polygons.resize(static_cast<size_t>(polygons) * 2 * agent.VerticesPerPolygon);
			for (uint16_t& value : agent.Polygons)
			{
				ENGINE_TRY_ASSIGN(value, reader.ReadU16());
			}
			agent.Areas.resize(polygons);
			for (uint8_t& area : agent.Areas)
			{
				ENGINE_TRY_ASSIGN(area, reader.ReadU8());
			}
			agent.DetailMeshes.resize(static_cast<size_t>(polygons) * 4);
			for (uint32_t& value : agent.DetailMeshes)
			{
				ENGINE_TRY_ASSIGN(value, reader.ReadU32());
			}

			ENGINE_TRY_ASSIGN(const uint32_t detailVertices, reader.ReadU32());
			ENGINE_TRY(CheckCount(detailVertices, MaxNavMeshDetailVertices, "detail vertices"));
			ENGINE_TRY(CheckRemaining(reader, detailVertices, 3 * sizeof(float), "detail vertices"));
			agent.DetailVertices.resize(static_cast<size_t>(detailVertices) * 3);
			for (float& value : agent.DetailVertices)
			{
				ENGINE_TRY_ASSIGN(value, reader.ReadF32());
			}

			ENGINE_TRY_ASSIGN(const uint32_t detailTriangles, reader.ReadU32());
			ENGINE_TRY(CheckCount(detailTriangles, MaxNavMeshDetailTriangles, "detail triangles"));
			ENGINE_TRY(CheckRemaining(reader, detailTriangles, 4, "detail triangles"));
			agent.DetailTriangles.resize(static_cast<size_t>(detailTriangles) * 4);
			for (uint8_t& value : agent.DetailTriangles)
			{
				ENGINE_TRY_ASSIGN(value, reader.ReadU8());
			}
			return agent;
		}

	}

	Status ValidateNavMeshData(const NavMeshData& mesh)
	{
		if (mesh.Agents.empty() || mesh.Agents.size() > MaxNavAgentTypes)
			return MakeError(ErrorCode::Validation, "the mesh has {} agent types; a mesh has 1 to {}", mesh.Agents.size(), MaxNavAgentTypes);
		if (mesh.AreaCosts.size() > MaxNavMeshAreaCosts)
			return MakeError(ErrorCode::Validation, "the mesh has {} area costs; a mesh has at most {}", mesh.AreaCosts.size(), MaxNavMeshAreaCosts);
		for (size_t index = 0; index < mesh.AreaCosts.size(); ++index)
		{
			if (!Utils::IsFiniteIn(mesh.AreaCosts[index], 1.0f, MaxNavAreaCost))
				return MakeError(ErrorCode::Validation, "area {}'s cost is {}; a cost is in [1, {}]", index + 1, mesh.AreaCosts[index], MaxNavAreaCost);
		}
		std::set<std::string_view> names;
		for (size_t index = 0; index < mesh.Agents.size(); ++index)
		{
			const NavMeshAgent& agent = mesh.Agents[index];
			if (agent.Name.empty() || agent.Name.size() > MaxNavAgentNameLength)
				return MakeError(ErrorCode::Validation, "agent {}'s name has {} bytes; a name has 1 to {}", index, agent.Name.size(), MaxNavAgentNameLength);
			if (!names.insert(agent.Name).second)
				return MakeError(ErrorCode::Validation, "two agent types are named '{}'", agent.Name);
			ENGINE_TRY(WithContext(Utils::ValidateAgent(agent, mesh.AreaCosts.size()), std::format("agent '{}'", agent.Name)));
		}
		return {};
	}

	Buffer SerializeNavMeshPayload(const NavMeshData& mesh)
	{
		ENGINE_CORE_ASSERT(ValidateNavMeshData(mesh).has_value(), "Serializing an invalid navigation mesh");
		BinaryWriter writer;
		return Utils::WritePayload(writer, mesh);
	}

	Result<NavMeshData> DeserializeNavMeshPayload(std::span<const std::byte> payload)
	{
		BinaryReader reader(payload);
		NavMeshData mesh;
		ENGINE_TRY_ASSIGN(mesh.InputHash, reader.ReadU64());
		ENGINE_TRY_ASSIGN(const uint32_t areaCosts, reader.ReadU32());
		ENGINE_TRY(Utils::CheckCount(areaCosts, MaxNavMeshAreaCosts, "area costs"));
		mesh.AreaCosts.resize(areaCosts);
		for (float& cost : mesh.AreaCosts)
		{
			ENGINE_TRY_ASSIGN(cost, reader.ReadF32());
		}
		ENGINE_TRY_ASSIGN(const uint32_t agents, reader.ReadU32());
		ENGINE_TRY(Utils::CheckCount(agents, MaxNavAgentTypes, "agent types"));
		mesh.Agents.reserve(agents);
		for (uint32_t index = 0; index < agents; ++index)
		{
			ENGINE_TRY_ASSIGN(NavMeshAgent agent, WithContext(Utils::ReadAgent(reader), std::format("reading agent {}", index)));
			mesh.Agents.push_back(std::move(agent));
		}
		if (!reader.IsAtEnd())
			return MakeError(ErrorCode::Parse, "a navigation mesh payload has {} trailing bytes", reader.GetRemaining());
		ENGINE_TRY(ValidateNavMeshData(mesh));
		return mesh;
	}

	Buffer WriteNavMeshSource(const NavMeshData& mesh)
	{
		ENGINE_CORE_ASSERT(ValidateNavMeshData(mesh).has_value(), "Writing the source of an invalid navigation mesh");
		BinaryWriter writer;
		for (const char character : NavMeshSourceMagic)
			writer.WriteU8(static_cast<uint8_t>(character));
		writer.WriteU32(NavMeshData::FormatVersion);
		return Utils::WritePayload(writer, mesh);
	}

	Result<NavMeshData> ReadNavMeshSource(std::span<const std::byte> source)
	{
		BinaryReader reader(source);
		for (const char expected : NavMeshSourceMagic)
		{
			const Result<uint8_t> character = reader.ReadU8();
			if (!character.has_value() || *character != static_cast<uint8_t>(expected))
				return MakeError(ErrorCode::Parse, "not a navigation mesh source: the file does not start with \"ENAV\"");
		}
		ENGINE_TRY_ASSIGN(const uint32_t version, reader.ReadU32());
		if (version != NavMeshData::FormatVersion)
			return MakeError(ErrorCode::UnsupportedVersion, "navigation mesh source version {}; this build reads version {}", version, NavMeshData::FormatVersion);
		return DeserializeNavMeshPayload(source.subspan(Utils::NavMeshSourceHeaderBytes));
	}

	Buffer CookNavMesh(const NavMeshData& mesh, uint32_t importerVersion)
	{
		const Buffer payload = SerializeNavMeshPayload(mesh);
		return WriteCookedArtifact(AssetType::NavMesh, NavMeshData::FormatVersion, importerVersion, payload);
	}

	Result<AssetRef<NavMeshData>> LoadCookedNavMesh(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView artifact, ReadCookedArtifact(cooked, AssetType::NavMesh, NavMeshData::FormatVersion));
		ENGINE_TRY_ASSIGN(NavMeshData mesh, DeserializeNavMeshPayload(artifact.Payload));
		return AssetRef<NavMeshData>(CreateRef<NavMeshData>(std::move(mesh)));
	}

	glm::vec3 GetNavMeshVertexPosition(const NavMeshAgent& agent, size_t index)
	{
		ENGINE_CORE_ASSERT(index < agent.GetVertexCount(), "Vertex {} of a mesh of {}", index, agent.GetVertexCount());
		const uint16_t* vertex = agent.Vertices.data() + index * 3;
		return agent.BoundsMin
			+ glm::vec3(static_cast<float>(vertex[0]) * agent.CellSize, static_cast<float>(vertex[1]) * agent.CellHeight,
				static_cast<float>(vertex[2]) * agent.CellSize);
	}

	uint32_t GetNavMeshPolygonVertexCount(const NavMeshAgent& agent, size_t polygon)
	{
		ENGINE_CORE_ASSERT(polygon < agent.GetPolygonCount(), "Polygon {} of a mesh of {}", polygon, agent.GetPolygonCount());
		const uint16_t* values = agent.Polygons.data() + polygon * 2 * agent.VerticesPerPolygon;
		uint32_t count = 0;
		while (count < agent.VerticesPerPolygon && values[count] != NavMeshNullIndex)
			++count;
		return count;
	}

}
