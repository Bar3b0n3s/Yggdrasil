#include "TestsPCH.h"

#include "Engine/Asset/PakReader.h"

#include "Engine/Asset/BuiltinMeshes.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/AssetPipeline/PakWriter.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Random.h"
#include "Support/TempDirectory.h"

namespace Engine {

	namespace {

		Buffer MakeSmallPak()
		{
			PakWriter writer;
			REQUIRE(writer.Add({ .Handle = AssetHandle(0x101), .Type = "Mesh", .Path = "engine://Meshes/Cube", .Data = CookMesh(GenerateBuiltinMesh(BuiltinMesh::Cube), 1) })
					.has_value());
			const std::string spirv = "spirv bytes";
			REQUIRE(writer.Add({ .Handle = AssetHandle(), .Type = "File", .Path = "Shaders/Triangle_VSMain.spv", .Data = Buffer(AsBytes(spirv).begin(), AsBytes(spirv).end()) })
					.has_value());
			return writer.Build();
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("PakReader: entries are found by handle and path, from memory and from a file" * doctest::skip(true))
		{
			const Buffer pak = MakeSmallPak();
			Result<Ref<const PakReader>> reader = PakReader::OpenMemory(pak, "Small.pak");
			REQUIRE_MESSAGE(reader.has_value(), reader.error().ToString());
			CHECK((*reader)->GetName() == "Small.pak");
			CHECK((*reader)->GetHeader().EntryCount == 2);
			const PakEntry* cube = (*reader)->FindByHandle(AssetHandle(0x101));
			REQUIRE(cube != nullptr);
			CHECK((*reader)->FindByPath("engine://Meshes/Cube") == cube);
			CHECK((*reader)->FindByHandle(AssetHandle()) == nullptr);
			Result<Buffer> bytes = (*reader)->ReadEntry(*cube);
			REQUIRE(bytes.has_value());
			CHECK(LoadCookedMesh(*bytes).has_value());

			Test::TempDirectory directory("PakReaderFile");
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Small.pak", pak).has_value());
			Result<Ref<const PakReader>> fromFile = PakReader::Open(directory / "Small.pak");
			REQUIRE_MESSAGE(fromFile.has_value(), fromFile.error().ToString());
			const PakEntry* shader = (*fromFile)->FindByPath("Shaders/Triangle_VSMain.spv");
			REQUIRE(shader != nullptr);
			Result<Buffer> shaderBytes = (*fromFile)->ReadEntry(*shader);
			REQUIRE(shaderBytes.has_value());
			CHECK(AsStringView(*shaderBytes) == "spirv bytes");
			CHECK_FALSE(PakReader::Open(directory / "Missing.pak").has_value());
		}

		TEST_CASE("PakReader: 10,000 seeded mutations never crash and always return Result" * doctest::skip(true))
		{
			const Buffer pak = MakeSmallPak();
			Random random(0x9A4);
			for (int iteration = 0; iteration < 10000; ++iteration)
			{
				Buffer mutated = pak;
				const int64_t flips = random.RangeInt(1, 4);
				for (int64_t flip = 0; flip < flips; ++flip)
				{
					const size_t index = static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(mutated.size()) - 1));
					mutated[index] = static_cast<std::byte>(random.NextU32() & 0xFF);
				}
				if (random.NextBool(0.2))
					mutated.resize(static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(mutated.size()))));
				Result<Ref<const PakReader>> reader = PakReader::OpenMemory(std::move(mutated), "Mutated.pak");
				if (!reader.has_value())
					continue;
				// An opened pak never crashes on reads either: entries fail their hash check instead.
				for (const PakEntry& entry : (*reader)->GetEntries())
					static_cast<void>((*reader)->ReadEntry(entry));
			}
		}
	}

}
