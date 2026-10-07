#include "TestsPCH.h"

#include "Engine/Asset/MeshData.h"

#include "Engine/Core/Random.h"

#include <limits>

namespace Engine {

	namespace {

		// One quad (two triangles) in XY facing +Z, one slot.
		MeshData MakeQuad()
		{
			MeshData mesh;
			const glm::vec3 positions[] = { { -0.5f, -0.5f, 0.0f }, { 0.5f, -0.5f, 0.0f }, { 0.5f, 0.5f, 0.0f }, { -0.5f, 0.5f, 0.0f } };
			const glm::vec2 texCoords[] = { { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f } };
			for (size_t index = 0; index < 4; ++index)
			{
				mesh.Vertices.push_back({ .Position = positions[index], .Normal = { 0.0f, 0.0f, 1.0f }, .Tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .TexCoord = texCoords[index] });
				mesh.Bounds.Extend(positions[index]);
			}
			mesh.Indices = { 0, 1, 2, 0, 2, 3 };
			mesh.Submeshes.push_back({ .IndexOffset = 0, .IndexCount = 6, .MaterialSlot = 0, .Bounds = mesh.Bounds });
			mesh.Slots.push_back({ .Name = "Default", .DefaultMaterial = AssetHandle() });
			return mesh;
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("MeshData: the payload round-trips" * doctest::skip(true))
		{
			const MeshData quad = MakeQuad();
			REQUIRE(ValidateMeshData(quad).has_value());
			const Buffer payload = SerializeMeshPayload(quad);
			// 4 counts (16) + bounds (24) + 1 submesh (36) + 1 slot (4 + 7 + 8) + 4 vertices (192) + 6 indices (24).
			CHECK(payload.size() == 16 + 24 + 36 + 19 + 192 + 24);
			Result<MeshData> read = DeserializeMeshPayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Vertices == quad.Vertices);
			CHECK(read->Indices == quad.Indices);
			CHECK(read->Submeshes == quad.Submeshes);
			CHECK(read->Slots == quad.Slots);
			CHECK(read->Bounds == quad.Bounds);
			CHECK(SerializeMeshPayload(*read) == payload);
			Result<AssetRef<MeshData>> loaded = LoadCookedMesh(CookMesh(quad, 1));
			REQUIRE(loaded.has_value());
			CHECK(SerializeMeshPayload(**loaded) == payload);
		}

		TEST_CASE("MeshData: invalid meshes fail validation" * doctest::skip(true))
		{
			MeshData outOfRange = MakeQuad();
			outOfRange.Indices[2] = 4;
			CHECK_FALSE(ValidateMeshData(outOfRange).has_value());
			MeshData notTriangles = MakeQuad();
			notTriangles.Submeshes[0].IndexCount = 5;
			CHECK_FALSE(ValidateMeshData(notTriangles).has_value());
			MeshData badSlot = MakeQuad();
			badSlot.Submeshes[0].MaterialSlot = 1;
			CHECK_FALSE(ValidateMeshData(badSlot).has_value());
			MeshData notFinite = MakeQuad();
			notFinite.Vertices[0].Position.x = std::numeric_limits<float>::infinity();
			CHECK_FALSE(ValidateMeshData(notFinite).has_value());
			MeshData wrongBounds = MakeQuad();
			wrongBounds.Bounds.Max.x = 3.0f;
			CHECK_FALSE(ValidateMeshData(wrongBounds).has_value());
			MeshData badTangentSign = MakeQuad();
			badTangentSign.Vertices[1].Tangent.w = 0.5f;
			CHECK_FALSE(ValidateMeshData(badTangentSign).has_value());
			CHECK_FALSE(ValidateMeshData(MeshData()).has_value());
		}

		TEST_CASE("MeshData: 10,000 seeded mutations of a payload never crash" * doctest::skip(true))
		{
			const Buffer payload = SerializeMeshPayload(MakeQuad());
			Random random(0x3E54);
			for (int iteration = 0; iteration < 10000; ++iteration)
			{
				Buffer mutated = payload;
				const int64_t flips = random.RangeInt(1, 6);
				for (int64_t flip = 0; flip < flips; ++flip)
				{
					const size_t index = static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(mutated.size()) - 1));
					mutated[index] = static_cast<std::byte>(random.NextU32() & 0xFF);
				}
				if (random.NextBool(0.2))
					mutated.resize(static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(mutated.size()))));
				const Result<MeshData> read = DeserializeMeshPayload(mutated);
				if (read.has_value())
					CHECK(ValidateMeshData(*read).has_value());
			}
		}
	}

}
