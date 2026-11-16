#include "TestsPCH.h"

#include "Engine/Asset/NavMeshData.h"

#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Random.h"
#include "Support/DeathTest.h"
#include "Support/Fuzz.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>

// Navigation meshes (M30, Docs/Decisions/0045-m30-navigation.md decision 4): the format version 1, its source file, its
// validation and its fuzz target.

namespace Engine {

	namespace {

		constexpr uint16_t Null = NavMeshNullIndex;

		// A 10 x 10-cell square of two triangles sharing the edge from vertex 0 to vertex 2, the second in the one costly area,
		// each with a one-triangle detail mesh of its own vertices.
		NavMeshAgent MakeAgent(std::string name)
		{
			NavMeshAgent agent;
			agent.Name = std::move(name);
			agent.CellSize = 0.5f;
			agent.CellHeight = 0.25f;
			agent.BoundsMin = glm::vec3(-2.0f, 1.0f, 3.0f);
			agent.BoundsMax = glm::vec3(3.0f, 1.0f, 8.0f);
			agent.VerticesPerPolygon = 6;
			agent.Vertices = { 0, 0, 0, 10, 0, 0, 10, 0, 10, 0, 0, 10 };
			agent.Polygons = {
				0,
				1,
				2,
				Null,
				Null,
				Null,
				Null,
				Null,
				1,
				Null,
				Null,
				Null, // vertices 0, 1, 2; across edge 2 (2 to 0), polygon 1
				0,
				2,
				3,
				Null,
				Null,
				Null,
				0,
				Null,
				Null,
				Null,
				Null,
				Null, // vertices 0, 2, 3; across edge 0 (0 to 2), polygon 0
			};
			agent.Areas = { NavMeshWalkableArea, 1 };
			agent.DetailMeshes = { 0, 3, 0, 1, 3, 3, 1, 1 };
			for (const size_t vertex : { 0, 1, 2, 0, 2, 3 })
			{
				const glm::vec3 position = GetNavMeshVertexPosition(agent, vertex);
				agent.DetailVertices.insert(agent.DetailVertices.end(), { position.x, position.y, position.z });
			}
			agent.DetailTriangles = { 0, 1, 2, 0, 0, 1, 2, 0 };
			return agent;
		}

		NavMeshData MakeMesh()
		{
			NavMeshData mesh;
			mesh.InputHash = 0x0123456789abcdefULL;
			mesh.AreaCosts = { 4.0f };
			mesh.Agents.push_back(MakeAgent("Small"));
			NavMeshAgent large = MakeAgent("Large");
			large.Radius = 1.0f;
			large.Height = 2.5f;
			mesh.Agents.push_back(std::move(large));
			return mesh;
		}

		// Whether `change` applied to MakeMesh() makes ValidateNavMeshData refuse it with Validation.
		bool IsRefused(const std::function<void(NavMeshData&)>& change)
		{
			NavMeshData mesh = MakeMesh();
			change(mesh);
			const Status status = ValidateNavMeshData(mesh);
			return !status.has_value() && status.error().GetCode() == ErrorCode::Validation;
		}

		// A mesh of one agent: `polygons` copies of the triangle of vertices 0, 1 and 2, without neighbours, each with a
		// detail mesh of the same `detailVertices` vertices (the triangle's, then extra ones at its first vertex) and
		// `detailTriangles` triangles.
		NavMeshData MakeRepeatedMesh(size_t polygons, uint32_t detailVertices, uint32_t detailTriangles)
		{
			NavMeshData mesh;
			NavMeshAgent& agent = mesh.Agents.emplace_back(MakeAgent("Small"));
			agent.VerticesPerPolygon = 3;
			agent.Vertices = { 0, 0, 0, 10, 0, 0, 10, 0, 10 };
			agent.Polygons.clear();
			agent.Areas.assign(polygons, NavMeshWalkableArea);
			agent.DetailMeshes.clear();
			for (size_t polygon = 0; polygon < polygons; ++polygon)
			{
				agent.Polygons.insert(agent.Polygons.end(), { 0, 1, 2, Null, Null, Null });
				agent.DetailMeshes.insert(agent.DetailMeshes.end(), { 0, detailVertices, 0, detailTriangles });
			}
			agent.DetailVertices.clear();
			for (uint32_t vertex = 0; vertex < detailVertices; ++vertex)
			{
				const glm::vec3 position = GetNavMeshVertexPosition(agent, vertex < 3 ? vertex : 0);
				agent.DetailVertices.insert(agent.DetailVertices.end(), { position.x, position.y, position.z });
			}
			agent.DetailTriangles.clear();
			for (uint32_t triangle = 0; triangle < detailTriangles; ++triangle)
				agent.DetailTriangles.insert(agent.DetailTriangles.end(), { 0, 1, 2, 0 });
			return mesh;
		}

		// Whether ValidateNavMeshData accepts `mesh`.
		bool IsValid(const NavMeshData& mesh)
		{
			const Status status = ValidateNavMeshData(mesh);
			if (!status.has_value())
				MESSAGE(status.error().ToString());
			return status.has_value();
		}

	}

	ENGINE_DEATH_TEST("Asset/NavMeshVertexPastItsCount")
	{
		const NavMeshAgent agent = MakeAgent("Small");
		static_cast<void>(GetNavMeshVertexPosition(agent, 4));
	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("NavMeshData: the artifact names its type and format version, and its loader is registered")
		{
			CHECK(NavMeshData::StaticType == AssetType::NavMesh);
			CHECK(NavMeshData::FormatVersion == 1);
			CHECK(GetCookedFormatVersion(AssetType::NavMesh) == NavMeshData::FormatVersion);
			AssetLoaderRegistry loaders;
			RegisterBuiltinLoaders(loaders);
			REQUIRE(loaders.Find(AssetType::NavMesh) != nullptr);
			const std::byte payload[1] = {};
			const Result<AssetRef<NavMeshData>> newer =
				LoadCookedNavMesh(WriteCookedArtifact(AssetType::NavMesh, NavMeshData::FormatVersion + 1, 1, payload));
			REQUIRE_FALSE(newer.has_value());
			CHECK(newer.error().GetCode() == ErrorCode::UnsupportedVersion);
			const Result<AssetRef<NavMeshData>> cooked = LoadCookedNavMesh(CookNavMesh(MakeMesh(), 1));
			REQUIRE_MESSAGE(cooked.has_value(), cooked.error().ToString());
			CHECK((*cooked)->Agents == MakeMesh().Agents);
		}

		TEST_CASE("NavMeshData: vertices sit at their cells and a polygon's vertices end at its first unused slot")
		{
			const NavMeshAgent agent = MakeAgent("Small");
			CHECK(GetNavMeshVertexPosition(agent, 0) == glm::vec3(-2.0f, 1.0f, 3.0f));
			CHECK(GetNavMeshVertexPosition(agent, 2) == glm::vec3(3.0f, 1.0f, 8.0f));
			CHECK(agent.GetVertexCount() == 4);
			CHECK(agent.GetPolygonCount() == 2);
			CHECK(GetNavMeshPolygonVertexCount(agent, 0) == 3);
			NavMeshAgent full = agent;
			full.VerticesPerPolygon = 3;
			full.Polygons = { 0, 1, 2, Null, Null, 1, 0, 2, 3, 0, Null, Null };
			CHECK(GetNavMeshPolygonVertexCount(full, 1) == 3);
			NavMeshData mesh = MakeMesh();
			mesh.Agents[0] = full;
			CHECK(ValidateNavMeshData(mesh).has_value());
		}

		TEST_CASE("NavMeshData: a vertex index past its count asserts")
		{
			ENGINE_CHECK_DEATH("Asset/NavMeshVertexPastItsCount", "Vertex 4");
		}

		TEST_CASE("NavMeshData: a payload and a source file round-trip the mesh byte for byte")
		{
			const NavMeshData mesh = MakeMesh();
			REQUIRE(ValidateNavMeshData(mesh).has_value());
			const Buffer payload = SerializeNavMeshPayload(mesh);
			const Result<NavMeshData> read = DeserializeNavMeshPayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->InputHash == mesh.InputHash);
			CHECK(read->AreaCosts == mesh.AreaCosts);
			CHECK(read->Agents == mesh.Agents);
			CHECK(SerializeNavMeshPayload(*read) == payload);

			const Buffer source = WriteNavMeshSource(mesh);
			REQUIRE(source.size() == 8 + payload.size());
			CHECK(static_cast<char>(source[0]) == 'E');
			CHECK(static_cast<char>(source[1]) == 'N');
			CHECK(static_cast<char>(source[3]) == 'V');
			const Result<NavMeshData> fromSource = ReadNavMeshSource(source);
			REQUIRE_MESSAGE(fromSource.has_value(), fromSource.error().ToString());
			CHECK(fromSource->Agents == mesh.Agents);
			// An agent type whose bake found nothing to walk on holds no polygon, and is valid.
			NavMeshData empty = MakeMesh();
			NavMeshAgent& nothing = empty.Agents[1];
			nothing.Vertices.clear();
			nothing.Polygons.clear();
			nothing.Areas.clear();
			nothing.DetailMeshes.clear();
			nothing.DetailVertices.clear();
			nothing.DetailTriangles.clear();
			REQUIRE(ValidateNavMeshData(empty).has_value());
			const Result<NavMeshData> emptyRead = DeserializeNavMeshPayload(SerializeNavMeshPayload(empty));
			REQUIRE_MESSAGE(emptyRead.has_value(), emptyRead.error().ToString());
			CHECK(emptyRead->Agents[1].GetPolygonCount() == 0);
		}

		TEST_CASE("NavMeshData: agents, area costs and the agents' settings outside the format's rules are refused")
		{
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents.clear();
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				for (const char* name : { "C", "D", "E" })
					mesh.Agents.push_back(MakeAgent(name));
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[1].Name = "Small";
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].Name.clear();
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].Name = std::string(MaxNavAgentNameLength + 1, 'a');
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.AreaCosts[0] = 0.5f;
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.AreaCosts[0] = std::numeric_limits<float>::infinity();
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.AreaCosts.assign(MaxNavMeshAreaCosts + 1, 2.0f);
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].Radius = MaxNavAgentRadius * 2.0f;
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].Height = std::numeric_limits<float>::quiet_NaN();
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].MaxSlope = 90.0f;
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].MaxStep = -0.1f;
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].CellSize = 0.0f;
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].CellHeight = MaxNavCellSize * 2.0f;
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].BoundsMin.x = 4.0f;
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].BoundsMax.y = std::numeric_limits<float>::infinity();
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].VerticesPerPolygon = 7;
			}));
		}

		TEST_CASE("NavMeshData: array sizes, indices, neighbours, areas and detail meshes outside the format's rules are refused")
		{
			// Array sizes that disagree with the counts.
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].Vertices.pop_back();
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].Polygons.pop_back();
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].DetailMeshes.pop_back();
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].DetailVertices.pop_back();
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].DetailTriangles.pop_back();
			}));
			// Polygon vertices: fewer than three, past the vertex count, a used slot after an unused one.
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].Polygons[2] = Null;
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].Polygons[1] = 4;
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].Polygons[4] = 3;
			}));
			// Neighbours: past the polygon count, or on an unused slot.
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].Polygons[6] = 2;
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].Polygons[6] = 0x8000;
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].Polygons[10] = 1;
			}));
			// Areas: the null area, or a cost the mesh does not hold.
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].Areas[0] = 0;
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].Areas[1] = 2;
			}));
			// Detail meshes: fewer vertices than the polygon, ranges outside the arrays, a triangle naming a vertex past them.
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].DetailMeshes[1] = 2;
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].DetailMeshes[4] = 4;
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].DetailMeshes[7] = 2;
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].DetailMeshes[1] = 3 + MaxNavMeshDetailVerticesPerPolygon + 1;
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].DetailTriangles[2] = 3;
			}));
			CHECK(IsRefused([](NavMeshData& mesh)
			{
				mesh.Agents[0].DetailVertices[4] = std::numeric_limits<float>::quiet_NaN();
			}));
		}

		// Detour stores a neighbour as its index + 1 and reads DT_EXT_LINK (0x8000) as a link to another tile, so a polygon
		// index of 0x7fff or more would silently lose its links: a mesh holds at most 0x7fff polygons, every neighbour index
		// below 0x7fff.
		TEST_CASE("NavMeshData: a mesh holds at most 0x7fff polygons, so no neighbour index reaches Detour's external-link bit")
		{
			CHECK(MaxNavMeshPolygons == 0x7fff);
			NavMeshData most = MakeRepeatedMesh(MaxNavMeshPolygons, 3, 1);
			CHECK(IsValid(most));
			// The last polygon is a valid neighbour; one past it is not.
			most.Agents[0].Polygons[3] = static_cast<uint16_t>(MaxNavMeshPolygons - 1);
			CHECK(IsValid(most));
			most.Agents[0].Polygons[3] = static_cast<uint16_t>(MaxNavMeshPolygons);
			CHECK_FALSE(IsValid(most));
			CHECK_FALSE(IsValid(MakeRepeatedMesh(MaxNavMeshPolygons + 1, 3, 1)));
		}

		// The caps that keep Detour's narrowing safe, each reached with arrays large enough that no range check refuses first:
		// a polygon's extra detail vertices and its detail triangles (a byte each in dtPolyDetail), and every polygon's extra
		// detail vertices together (a uint16 range in dtCreateNavMeshData).
		TEST_CASE("NavMeshData: the detail mesh's per-polygon and total caps refuse what the range checks pass")
		{
			CHECK(IsValid(MakeRepeatedMesh(1, 3 + MaxNavMeshDetailVerticesPerPolygon, 1)));
			CHECK_FALSE(IsValid(MakeRepeatedMesh(1, 3 + MaxNavMeshDetailVerticesPerPolygon + 1, 1)));
			CHECK(IsValid(MakeRepeatedMesh(1, 3, MaxNavMeshDetailTrianglesPerPolygon)));
			CHECK_FALSE(IsValid(MakeRepeatedMesh(1, 3, MaxNavMeshDetailTrianglesPerPolygon + 1)));
			// 257 polygons of 255 extra detail vertices each make 65,535, the most; 258 make more.
			constexpr size_t Polygons = MaxNavMeshExtraDetailVertices / MaxNavMeshDetailVerticesPerPolygon;
			static_assert(Polygons * MaxNavMeshDetailVerticesPerPolygon == MaxNavMeshExtraDetailVertices);
			CHECK(IsValid(MakeRepeatedMesh(Polygons, 3 + MaxNavMeshDetailVerticesPerPolygon, 1)));
			CHECK_FALSE(IsValid(MakeRepeatedMesh(Polygons + 1, 3 + MaxNavMeshDetailVerticesPerPolygon, 1)));
		}

		// The readers' own refusals: a count above its limit, refused before anything is allocated however large; a payload
		// shorter or longer than its arrays; and a source whose magic is not "ENAV", of another version or shorter than its
		// header.
		TEST_CASE("NavMeshData: a count past its limit, a truncated payload, trailing bytes, another magic and another version are refused")
		{
			const Buffer payload = SerializeNavMeshPayload(MakeMesh());
			// The payload: InputHash (uint64), the area cost count (uint32) and its one cost, then the agent count (uint32).
			constexpr size_t AgentCountOffset = 8 + 4 + 4;
			const auto withU32 = [&payload](size_t offset, uint32_t value)
			{
				Buffer changed = payload;
				for (size_t index = 0; index < 4; ++index)
					changed[offset + index] = static_cast<std::byte>((value >> (8 * index)) & 0xFFU);
				return changed;
			};
			for (const uint32_t count : { MaxNavAgentTypes + 1, 0xFFFFFFFFU })
			{
				CAPTURE(count);
				CHECK(DeserializeNavMeshPayload(withU32(AgentCountOffset, count)).error().GetCode() == ErrorCode::Parse);
				CHECK(DeserializeNavMeshPayload(withU32(8, count)).error().GetCode() == ErrorCode::Parse);
			}
			Buffer truncated = payload;
			truncated.pop_back();
			CHECK(DeserializeNavMeshPayload(truncated).error().GetCode() == ErrorCode::Parse);
			Buffer trailing = payload;
			trailing.push_back(std::byte{ 0 });
			CHECK(DeserializeNavMeshPayload(trailing).error().GetCode() == ErrorCode::Parse);
			CHECK(DeserializeNavMeshPayload(std::span(payload).first(7)).error().GetCode() == ErrorCode::Parse);

			Buffer magic = WriteNavMeshSource(MakeMesh());
			magic[1] = std::byte{ 'X' };
			CHECK(ReadNavMeshSource(magic).error().GetCode() == ErrorCode::Parse);
			const Buffer source = WriteNavMeshSource(MakeMesh());
			CHECK(ReadNavMeshSource(std::span(source).first(3)).error().GetCode() == ErrorCode::Parse);
			Buffer version = source;
			version[4] = std::byte{ 2 };
			CHECK(ReadNavMeshSource(version).error().GetCode() == ErrorCode::UnsupportedVersion);
		}

		TEST_CASE("NavMeshData: 10,000 seeded mutations of a payload never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			const Buffer payload = SerializeNavMeshPayload(MakeMesh());
			REQUIRE(DeserializeNavMeshPayload(payload).has_value());
			const Test::FuzzRun run = Test::RunFuzzCase(0x4E4D, 10000, [&payload](Random& random, uint64_t /*index*/)
			{
				Buffer mutated = payload;
				Test::MutateBytes(mutated, random);
				// A payload the reader accepts is a valid mesh, which serializes to the bytes it was read from.
				const Result<NavMeshData> read = DeserializeNavMeshPayload(mutated);
				if (!read.has_value())
					return false;
				CHECK(ValidateNavMeshData(*read).has_value());
				CHECK(SerializeNavMeshPayload(*read) == mutated);
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
				CHECK(run.Counts.Rejected > 0);
		}
	}

}
