#include "TestsPCH.h"

#include "Engine/Asset/PakReader.h"

#include "Engine/Asset/BuiltinMeshes.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/AssetPipeline/PakWriter.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
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

		// `pak` with its header rewritten through `edit` (the TOC hash stays valid, so only the edited field is wrong).
		template<typename Edit>
		Buffer WithHeader(const Buffer& pak, Edit&& edit)
		{
			Result<PakHeader> header = ReadPakHeader(pak);
			REQUIRE(header.has_value());
			edit(*header);
			Buffer edited = pak;
			const std::array<std::byte, PakHeader::Size> bytes = WritePakHeader(*header);
			std::ranges::copy(bytes, edited.begin());
			return edited;
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("PakReader: entries are found by handle and path, from memory and from a file")
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

		TEST_CASE("PakReader: 10,000 seeded mutations never crash and always return Result")
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

		TEST_CASE("PakReader: a truncated pak, trailing bytes and a wrong entry count are rejected")
		{
			const Buffer pak = MakeSmallPak();
			const auto rejects = [](Buffer bytes, std::string_view what)
			{
				CAPTURE(std::string(what));
				Result<Ref<const PakReader>> reader = PakReader::OpenMemory(std::move(bytes), "Broken.pak");
				REQUIRE_FALSE(reader.has_value());
				// The error names the pak.
				CHECK(reader.error().ToString().contains("Broken.pak"));
				return reader.error().GetCode();
			};
			CHECK(rejects(Buffer(pak.begin(), pak.begin() + 40), "shorter than the header") == ErrorCode::Parse);
			CHECK(rejects(Buffer(pak.begin(), pak.end() - 1), "truncated TOC") == ErrorCode::Parse);
			Buffer trailing = pak;
			trailing.push_back(std::byte{ 0 });
			CHECK(rejects(std::move(trailing), "trailing byte") == ErrorCode::Parse);
			CHECK(rejects(WithHeader(pak, [](PakHeader& header)
			{
				header.EntryCount = 3;
			}),
					  "entry count")
				== ErrorCode::Validation);
			CHECK(rejects(WithHeader(pak, [](PakHeader& header)
			{
				header.TocOffset = 8;
			}),
					  "TOC inside the header")
				== ErrorCode::Parse);
			CHECK(rejects(WithHeader(pak, [](PakHeader& header)
			{
				header.TocHash ^= 1;
			}),
					  "TOC hash")
				== ErrorCode::Validation);
		}

		TEST_CASE("PakReader: an entry is read by an equal copy, and a foreign entry is refused")
		{
			Result<Ref<const PakReader>> reader = PakReader::OpenMemory(MakeSmallPak(), "Small.pak");
			REQUIRE(reader.has_value());
			const PakEntry copy = *(*reader)->FindByPath("Shaders/Triangle_VSMain.spv");
			Result<Buffer> bytes = (*reader)->ReadEntry(copy);
			REQUIRE(bytes.has_value());
			CHECK(XXH64(*bytes) == copy.Hash);

			PakEntry foreign = copy;
			foreign.Path = "Shaders/Other.spv";
			Result<Buffer> refused = (*reader)->ReadEntry(foreign);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("PakReader: a file pak verifies its entries like a memory pak")
		{
			Buffer pak = MakeSmallPak();
			Result<Ref<const PakReader>> intact = PakReader::OpenMemory(pak, "Intact.pak");
			REQUIRE(intact.has_value());
			const PakEntry& shader = *(*intact)->FindByPath("Shaders/Triangle_VSMain.spv");
			pak[shader.Offset] ^= std::byte{ 0x01 };

			Test::TempDirectory directory("PakReaderCorrupted");
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Corrupted.pak", pak).has_value());
			Result<Ref<const PakReader>> reader = PakReader::Open(directory / "Corrupted.pak");
			REQUIRE_MESSAGE(reader.has_value(), reader.error().ToString());
			Result<Buffer> read = (*reader)->ReadEntry(*(*reader)->FindByPath("Shaders/Triangle_VSMain.spv"));
			REQUIRE_FALSE(read.has_value());
			CHECK(read.error().GetCode() == ErrorCode::Validation);
			CHECK(read.error().ToString().contains("corrupted"));
			// The intact entry still reads.
			CHECK((*reader)->ReadEntry(*(*reader)->FindByHandle(AssetHandle(0x101))).has_value());
			// A directory is not a pak.
			Result<Ref<const PakReader>> directoryPak = PakReader::Open(directory.GetPath());
			REQUIRE_FALSE(directoryPak.has_value());
			CHECK(directoryPak.error().GetCode() == ErrorCode::Io);
		}
	}

}
