#include "TestsPCH.h"

#include "Engine/AssetPipeline/PakWriter.h"

#include "Engine/Asset/BuiltinMeshes.h"
#include "Engine/Asset/BuiltinTextures.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/PakReader.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Core/Hash.h"

#include <nlohmann/json.hpp>

// The "Pak:" acceptance tests of Roadmap M6 cover the writer and the reader together.

namespace Engine {

	namespace {

		std::vector<PakWriterEntry> MakeEntries()
		{
			const std::string text = "plain file";
			return {
				{ .Handle = AssetHandle(0x185), .Type = "Texture", .Path = "engine://Textures/Missing", .Data = CookTexture(GenerateBuiltinTexture(BuiltinTexture::Missing), 1) },
				{ .Handle = AssetHandle(0x101), .Type = "Mesh", .Path = "engine://Meshes/Cube", .Data = CookMesh(GenerateBuiltinMesh(BuiltinMesh::Cube), 1) },
				{ .Handle = AssetHandle(), .Type = "File", .Path = "Shaders/Readme.txt", .Data = Buffer(AsBytes(text).begin(), AsBytes(text).end()) },
			};
		}

		Buffer BuildPak(std::vector<PakWriterEntry> entries)
		{
			PakWriter writer;
			for (PakWriterEntry& entry : entries)
				REQUIRE(writer.Add(std::move(entry)).has_value());
			Json metadata = Json::object();
			metadata["Name"] = "Test";
			writer.SetMetadata(VariantValue(metadata));
			return writer.Build();
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("Pak: round trip" * doctest::skip(true))
		{
			const std::vector<PakWriterEntry> entries = MakeEntries();
			const Buffer pak = BuildPak(MakeEntries());
			Result<Ref<const PakReader>> reader = PakReader::OpenMemory(pak, "RoundTrip.pak");
			REQUIRE_MESSAGE(reader.has_value(), reader.error().ToString());
			REQUIRE((*reader)->GetEntries().size() == entries.size());
			// Sorted by (Handle, Path): the plain file (null handle) first.
			CHECK((*reader)->GetEntries()[0].Path == "Shaders/Readme.txt");
			CHECK((*reader)->GetEntries()[1].Handle == AssetHandle(0x101));
			for (const PakWriterEntry& entry : entries)
			{
				CAPTURE(entry.Path);
				const PakEntry* found = (*reader)->FindByPath(entry.Path);
				REQUIRE(found != nullptr);
				CHECK(found->Offset % PakEntryAlignment == 0);
				CHECK(found->Hash == XXH64(entry.Data));
				Result<Buffer> bytes = (*reader)->ReadEntry(*found);
				REQUIRE(bytes.has_value());
				CHECK(*bytes == entry.Data);
			}
			Json metadata = (*reader)->GetMetadata().Get();
			CHECK(metadata["Name"] == Json("Test"));
		}

		TEST_CASE("Pak: flipped byte is detected" * doctest::skip(true))
		{
			const Buffer pak = BuildPak(MakeEntries());
			Result<Ref<const PakReader>> intact = PakReader::OpenMemory(pak, "Intact.pak");
			REQUIRE(intact.has_value());
			const PakEntry& cube = *(*intact)->FindByHandle(AssetHandle(0x101));

			// A flipped byte in an entry: the pak opens, the entry fails its hash check on first read.
			Buffer entryFlip = pak;
			entryFlip[cube.Offset + cube.Size / 2] ^= std::byte{ 0x20 };
			Result<Ref<const PakReader>> reader = PakReader::OpenMemory(entryFlip, "EntryFlip.pak");
			REQUIRE(reader.has_value());
			Result<Buffer> read = (*reader)->ReadEntry(*(*reader)->FindByHandle(AssetHandle(0x101)));
			REQUIRE_FALSE(read.has_value());
			CHECK(read.error().GetCode() == ErrorCode::Validation);
			CHECK(read.error().ToString().find("engine://Meshes/Cube") != std::string::npos);

			// A flipped byte in the TOC: the pak does not open (the TOC hash is verified at mount).
			Buffer tocFlip = pak;
			tocFlip[(*intact)->GetHeader().TocOffset + 3] ^= std::byte{ 0x01 };
			Result<Ref<const PakReader>> broken = PakReader::OpenMemory(tocFlip, "TocFlip.pak");
			REQUIRE_FALSE(broken.has_value());
			CHECK(broken.error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("Pak: writer is deterministic" * doctest::skip(true))
		{
			std::vector<PakWriterEntry> reversed = MakeEntries();
			std::ranges::reverse(reversed);
			const Buffer first = BuildPak(MakeEntries());
			const Buffer second = BuildPak(std::move(reversed));
			CHECK(first == second);
			CHECK(XXH64(first) == XXH64(BuildPak(MakeEntries())));
		}

		TEST_CASE("PakWriter: invalid entries are rejected" * doctest::skip(true))
		{
			PakWriter writer;
			const Buffer cube = CookMesh(GenerateBuiltinMesh(BuiltinMesh::Cube), 1);
			REQUIRE(writer.Add({ .Handle = AssetHandle(0x101), .Type = "Mesh", .Path = "engine://Meshes/Cube", .Data = cube }).has_value());
			const Status repeated = writer.Add({ .Handle = AssetHandle(0x101), .Type = "Mesh", .Path = "engine://Meshes/Other", .Data = cube });
			REQUIRE_FALSE(repeated.has_value());
			CHECK(repeated.error().GetCode() == ErrorCode::AlreadyExists);
			CHECK_FALSE(writer.Add({ .Handle = AssetHandle(0x102), .Type = "Texture", .Path = "engine://Meshes/Sphere", .Data = cube }).has_value());
			CHECK_FALSE(writer.Add({ .Handle = AssetHandle(), .Type = "Mesh", .Path = "engine://Meshes/Null", .Data = cube }).has_value());
			CHECK_FALSE(writer.Add({ .Handle = AssetHandle(7), .Type = "File", .Path = "Shaders/A.spv", .Data = {} }).has_value());
			CHECK_FALSE(writer.Add({ .Handle = AssetHandle(), .Type = "File", .Path = "../A.spv", .Data = {} }).has_value());
			CHECK(writer.GetEntryCount() == 1);
		}
	}

}
