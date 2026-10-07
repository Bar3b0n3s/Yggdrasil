#include "TestsPCH.h"

#include "Engine/Asset/MeshData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Random.h"

#include <algorithm>
#include <limits>
#include <string>

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
		TEST_CASE("MeshData: the payload round-trips")
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

		TEST_CASE("MeshData: invalid meshes fail validation")
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

		TEST_CASE("MeshData: validation names the first violation")
		{
			MeshData outOfRange = MakeQuad();
			outOfRange.Indices[2] = 400;
			const Status indexError = ValidateMeshData(outOfRange);
			REQUIRE_FALSE(indexError.has_value());
			CHECK(indexError.error().GetCode() == ErrorCode::Validation);
			CHECK(indexError.error().GetMessageText() == "index 2 is 400, beyond the 4 vertices");

			MeshData shortNormal = MakeQuad();
			shortNormal.Vertices[3].Normal = { 0.0f, 0.0f, 0.99f };
			CHECK_FALSE(ValidateMeshData(shortNormal).has_value());
			MeshData nearlyUnit = MakeQuad();
			nearlyUnit.Vertices[3].Normal = { 0.0f, 0.0f, 0.9995f };
			nearlyUnit.Vertices[3].Tangent = { 1.0005f, 0.0f, 0.0f, -1.0f };
			CHECK(ValidateMeshData(nearlyUnit).has_value());

			MeshData wrongSubmeshBounds = MakeQuad();
			wrongSubmeshBounds.Submeshes[0].Bounds.Min.y = -0.25f;
			CHECK_FALSE(ValidateMeshData(wrongSubmeshBounds).has_value());
			MeshData pastTheEnd = MakeQuad();
			pastTheEnd.Submeshes[0].IndexOffset = 3;
			CHECK_FALSE(ValidateMeshData(pastTheEnd).has_value());
			MeshData noSlots = MakeQuad();
			noSlots.Slots.clear();
			CHECK_FALSE(ValidateMeshData(noSlots).has_value());
			MeshData badName = MakeQuad();
			badName.Slots[0].Name = std::string("\xff\xfe", 2);
			CHECK_FALSE(ValidateMeshData(badName).has_value());
		}

		TEST_CASE("MeshData: truncated payloads, trailing bytes and oversized counts are Parse errors")
		{
			const Buffer payload = SerializeMeshPayload(MakeQuad());
			for (const size_t size : { size_t{ 0 }, size_t{ 15 }, size_t{ 40 }, payload.size() - 1 })
			{
				CAPTURE(size);
				const Result<MeshData> truncated = DeserializeMeshPayload(std::span(payload).first(size));
				REQUIRE_FALSE(truncated.has_value());
				CHECK(truncated.error().GetCode() == ErrorCode::Parse);
			}
			Buffer trailing = payload;
			trailing.push_back(std::byte{ 0 });
			const Result<MeshData> extra = DeserializeMeshPayload(trailing);
			REQUIRE_FALSE(extra.has_value());
			CHECK(extra.error().GetCode() == ErrorCode::Parse);

			// A submesh count of 0xffffffff is refused before anything is sized from it.
			Buffer huge = payload;
			for (size_t index = 8; index < 12; ++index)
				huge[index] = std::byte{ 0xff };
			const Result<MeshData> oversized = DeserializeMeshPayload(huge);
			REQUIRE_FALSE(oversized.has_value());
			CHECK(oversized.error().GetCode() == ErrorCode::Parse);

			// Well-formed bytes that break an invariant are Validation errors.
			Buffer badIndex = payload;
			badIndex[badIndex.size() - 4] = std::byte{ 9 };
			const Result<MeshData> invalid = DeserializeMeshPayload(badIndex);
			REQUIRE_FALSE(invalid.has_value());
			CHECK(invalid.error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("MeshData: a cooked artifact of another type or format version is rejected")
		{
			const MeshData quad = MakeQuad();
			const Buffer payload = SerializeMeshPayload(quad);
			const Buffer cooked = CookMesh(quad, 7);
			Result<CookedArtifactView> view = ReadCookedArtifact(cooked);
			REQUIRE_MESSAGE(view.has_value(), view.error().ToString());
			CHECK(view->Header.Type == AssetType::Mesh);
			CHECK(view->Header.FormatVersion == MeshData::FormatVersion);
			CHECK(view->Header.ImporterVersion == 7);
			CHECK(std::ranges::equal(view->Payload, payload));

			const Result<AssetRef<MeshData>> asTexture = LoadCookedMesh(WriteCookedArtifact(AssetType::Texture, MeshData::FormatVersion, 1, payload));
			REQUIRE_FALSE(asTexture.has_value());
			CHECK(asTexture.error().GetCode() == ErrorCode::Validation);
			const Result<AssetRef<MeshData>> newer = LoadCookedMesh(WriteCookedArtifact(AssetType::Mesh, static_cast<uint16_t>(MeshData::FormatVersion + 1), 1, payload));
			REQUIRE_FALSE(newer.has_value());
			CHECK(newer.error().GetCode() == ErrorCode::UnsupportedVersion);
		}

		TEST_CASE("MeshData: 10,000 seeded mutations of a payload never crash")
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
