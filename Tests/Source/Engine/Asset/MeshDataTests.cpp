#include "TestsPCH.h"

#include "Engine/Asset/MeshData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Random.h"
#include "Support/Fuzz.h"

#include <algorithm>
#include <array>
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

		// The quad with MeshData version 2's attribute stream: a second UV set and a vertex colour per vertex.
		MeshData MakeColoredQuad()
		{
			MeshData quad = MakeQuad();
			for (size_t index = 0; index < quad.Vertices.size(); ++index)
			{
				const float shade = static_cast<float>(index) * 0.25f;
				quad.Attributes.push_back({ .TexCoord1 = { shade, 1.0f - shade }, .Color = { shade, 0.5f, 1.0f, 1.0f - shade } });
			}
			return quad;
		}

		// M23 (C23.3): the coloured quad with the skin section (three joints, two influences a vertex) and the morph section
		// (one target "Smile" of default weight 0.5 that lifts each vertex by a tenth of its index).
		MeshData MakeSkinnedQuad()
		{
			MeshData quad = MakeColoredQuad();
			quad.SkinJointCount = 3;
			for (uint16_t index = 0; index < 4; ++index)
				quad.Skin.push_back({ .Joints = { static_cast<uint16_t>(index % 3), 2, 0, 0 }, .Weights = { 0.75f, 0.25f, 0.0f, 0.0f } });
			MeshMorphTarget smile{ .Name = "Smile", .DefaultWeight = 0.5f };
			for (size_t index = 0; index < 4; ++index)
				smile.Deltas.push_back({ .Position = { 0.0f, 0.1f * static_cast<float>(index), 0.0f } });
			quad.MorphTargets.push_back(smile);
			return quad;
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("MeshData: the payload round-trips")
		{
			const MeshData quad = MakeQuad();
			REQUIRE(ValidateMeshData(quad).has_value());
			const Buffer payload = SerializeMeshPayload(quad);
			// 4 counts (16) + Streams and Reserved (8) + bounds (24) + 1 submesh (36) + 1 slot (4 + 7 + 8) + 4 vertices (192) + 6
			// indices (24).
			CHECK(payload.size() == 16 + 8 + 24 + 36 + 19 + 192 + 24);
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

		// M23 (C23.1, C-MESH2; Docs/Decisions/0034-m23-contract.md decision 9, stream M): MeshData version 2 carries the
		// attribute stream (TexCoord1 and the vertex colour) after the indices, flagged by MeshStreams::Attributes, and keeps
		// the version 1 layout's other members; a payload that sets the Skin or Morph bit (C23.3's sections) without its
		// section is truncated, and one that sets an undefined bit or holds attributes for another vertex count is refused.
		TEST_CASE("MeshData: a version 2 payload round-trips the attribute stream and refuses the reserved bits")
		{
			const MeshData quad = MakeColoredQuad();
			CHECK(MeshData::FormatVersion == 2);
			REQUIRE(ValidateMeshData(quad).has_value());
			CHECK(GetMeshStreams(quad) == MeshStreams::Attributes);
			const Buffer payload = SerializeMeshPayload(quad);
			// Version 1's 311 bytes, the Streams and Reserved words (8) and 4 attribute vertices (96).
			CHECK(payload.size() == 311 + 8 + 96);
			const Result<MeshData> read = DeserializeMeshPayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Attributes == quad.Attributes);
			CHECK(SerializeMeshPayload(*read) == payload);

			// A mesh without the stream serializes none and reads back without it.
			const MeshData plain = MakeQuad();
			const Result<MeshData> plainRead = DeserializeMeshPayload(SerializeMeshPayload(plain));
			REQUIRE(plainRead.has_value());
			CHECK(plainRead->Attributes.empty());

			// Attributes for another vertex count fail validation.
			MeshData mismatched = quad;
			mismatched.Attributes.pop_back();
			CHECK_FALSE(ValidateMeshData(mismatched).has_value());

			// The Streams word follows the four counts; setting the Skin bit (C23.3's) without its section is refused.
			Buffer skinned = payload;
			skinned[16] = static_cast<std::byte>(std::to_integer<uint32_t>(skinned[16]) | 2U);
			CHECK_FALSE(DeserializeMeshPayload(skinned).has_value());
			// So are the Morph bit without its section, a bit no version defines and a non-zero Reserved word, each a Parse
			// error.
			Buffer morphed = payload;
			morphed[16] = static_cast<std::byte>(std::to_integer<uint32_t>(morphed[16]) | 4U);
			Buffer unknown = payload;
			unknown[19] = std::byte{ 0x80 };
			Buffer reserved = payload;
			reserved[20] = std::byte{ 1 };
			for (const Buffer* refused : { &morphed, &unknown, &reserved })
			{
				const Result<MeshData> rejected = DeserializeMeshPayload(*refused);
				REQUIRE_FALSE(rejected.has_value());
				CHECK(rejected.error().GetCode() == ErrorCode::Parse);
			}
			// The Attributes bit promises the stream: without its bytes the payload is truncated, and the stream's bytes
			// without the bit are trailing bytes.
			Buffer withoutBit = payload;
			withoutBit[16] = std::byte{ 0 };
			CHECK_FALSE(DeserializeMeshPayload(withoutBit).has_value());
			Buffer withoutStream = SerializeMeshPayload(plain);
			withoutStream[16] = std::byte{ 1 };
			CHECK_FALSE(DeserializeMeshPayload(withoutStream).has_value());
		}

		// M23 (C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md decision 25, stream A): the skin section (the joint
		// count, then a joint quadruple and its weights per vertex) and the morph section (the target count, then per target
		// its name, default weight and one delta per vertex) follow the attribute stream in bit order, and their invariants are
		// validated.
		TEST_CASE("MeshData: a version 2 payload round-trips the skin and morph sections")
		{
			const MeshData quad = MakeSkinnedQuad();
			const MeshMorphTarget& smile = quad.MorphTargets.front();
			REQUIRE(ValidateMeshData(quad).has_value());
			CHECK(GetMeshStreams(quad) == (MeshStreams::Attributes | MeshStreams::Skin | MeshStreams::Morph));
			const Buffer payload = SerializeMeshPayload(quad);
			// The attribute stream's 415 bytes, the skin section (4 + 4 x 24) and the morph section (4 + 4 + 5 + 4 + 4 x 36).
			CHECK(payload.size() == 415 + 100 + 161);
			const Result<MeshData> read = DeserializeMeshPayload(payload);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->SkinJointCount == 3);
			CHECK(read->Skin == quad.Skin);
			CHECK(read->MorphTargets == quad.MorphTargets);
			CHECK(SerializeMeshPayload(*read) == payload);

			// A joint index at the skin's joint count, weights that do not sum to 1, a skin for another vertex count, a joint
			// count of 0 with a skin, a duplicate target name and deltas for another vertex count each fail validation.
			MeshData outOfRange = quad;
			outOfRange.Skin[1].Joints[0] = 3;
			MeshData heavy = quad;
			heavy.Skin[0].Weights = glm::vec4(0.75f, 0.75f, 0.0f, 0.0f);
			MeshData shortSkin = quad;
			shortSkin.Skin.pop_back();
			MeshData noJoints = quad;
			noJoints.SkinJointCount = 0;
			MeshData twice = quad;
			twice.MorphTargets.push_back(smile);
			MeshData shortDeltas = quad;
			shortDeltas.MorphTargets[0].Deltas.pop_back();
			for (const MeshData* invalid : { &outOfRange, &heavy, &shortSkin, &noJoints, &twice, &shortDeltas })
				CHECK_FALSE(ValidateMeshData(*invalid).has_value());
		}

		TEST_CASE("MeshData: attribute colours outside [0, 1] and non-finite attributes fail validation")
		{
			MeshData bright = MakeColoredQuad();
			bright.Attributes[2].Color.g = 1.5f;
			CHECK_FALSE(ValidateMeshData(bright).has_value());
			MeshData negative = MakeColoredQuad();
			negative.Attributes[0].Color.a = -0.25f;
			CHECK_FALSE(ValidateMeshData(negative).has_value());
			MeshData notFinite = MakeColoredQuad();
			notFinite.Attributes[1].TexCoord1.x = std::numeric_limits<float>::quiet_NaN();
			CHECK_FALSE(ValidateMeshData(notFinite).has_value());
			MeshData bounds = MakeColoredQuad();
			bounds.Attributes[3].Color = glm::vec4(0.0f);
			bounds.Attributes[1].Color = glm::vec4(1.0f);
			CHECK(ValidateMeshData(bounds).has_value());
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

		// M23 (C-MESH2): a third of the mutations start from a version 2 payload with the attribute stream, so its section and
		// the Streams word are mutated too (Docs/Decisions/0034-m23-contract.md decision 19); since C23.3 another third from
		// the skinned and morphed quad, so they reach the skin and morph sections' counts, joint indices, names and deltas
		// (decision 24).
		TEST_CASE("MeshData: 10,000 seeded mutations of a payload never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			const std::array<Buffer, 3> payloads = { SerializeMeshPayload(MakeQuad()), SerializeMeshPayload(MakeColoredQuad()),
				SerializeMeshPayload(MakeSkinnedQuad()) };
			const Test::FuzzRun run = Test::RunFuzzCase(0x3E54, 10000, [&payloads](Random& random, uint64_t index)
			{
				Buffer mutated = payloads[index % payloads.size()];
				Test::MutateBytes(mutated, random);
				// A payload the reader accepts is a valid mesh.
				const Result<MeshData> read = DeserializeMeshPayload(mutated);
				if (!read.has_value())
					return false;
				CHECK(ValidateMeshData(*read).has_value());
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
				CHECK(run.Counts.Rejected > 0);
		}
	}

}
