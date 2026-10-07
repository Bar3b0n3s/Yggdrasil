#include "TestsPCH.h"

#include "Engine/Asset/PakFormat.h"

#include <nlohmann/json.hpp>

namespace Engine {

	TEST_SUITE("Asset")
	{
		TEST_CASE("PakFormat: the 64-byte header round-trips" * doctest::skip(true))
		{
			const PakHeader header{ .Version = PakHeader::CurrentVersion, .EntryCount = 3, .TocOffset = 4096, .TocSize = 512, .TocHash = 0x1234 };
			const std::array<std::byte, PakHeader::Size> bytes = WritePakHeader(header);
			CHECK(std::memcmp(bytes.data(), "ENGPAK01", 8) == 0);
			Result<PakHeader> read = ReadPakHeader(bytes);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(*read == header);

			std::array<std::byte, PakHeader::Size> reserved = bytes;
			reserved[PakHeader::Size - 1] = std::byte{ 1 };
			CHECK_FALSE(ReadPakHeader(reserved).has_value());
			std::array<std::byte, PakHeader::Size> newer = WritePakHeader({ .Version = 2 });
			Result<PakHeader> newerRead = ReadPakHeader(newer);
			REQUIRE_FALSE(newerRead.has_value());
			CHECK(newerRead.error().GetCode() == ErrorCode::UnsupportedVersion);
			CHECK_FALSE(ReadPakHeader(std::span<const std::byte>(bytes).first(63)).has_value());
		}

		TEST_CASE("PakFormat: the TOC round-trips canonically" * doctest::skip(true))
		{
			PakToc toc;
			toc.Entries = {
				{ .Handle = AssetHandle(), .Type = std::string(PakFileEntryType), .Path = "Shaders/Forward_VSMain.spv", .Offset = 64, .Size = 100, .Hash = 1 },
				{ .Handle = AssetHandle(0x101), .Type = "Mesh", .Path = "engine://Meshes/Cube", .Offset = 176, .Size = 32, .Hash = 2 },
			};
			Json metadata = Json::object();
			metadata["ProjectName"] = "Tetris";
			toc.Metadata = VariantValue(metadata);
			const std::string text = SerializePakToc(toc);
			Result<PakToc> read = ParsePakToc(text, 4096);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(*read == toc);
			CHECK(SerializePakToc(*read) == text);
		}

		TEST_CASE("PakFormat: misaligned, overlapping and unsorted entries are rejected" * doctest::skip(true))
		{
			const auto parse = [](std::vector<PakEntry> entries)
			{
				PakToc toc;
				toc.Entries = std::move(entries);
				return ParsePakToc(SerializePakToc(toc), 4096);
			};
			CHECK_FALSE(parse({ { .Handle = AssetHandle(1), .Type = "Mesh", .Path = "a", .Offset = 72, .Size = 8, .Hash = 0 } }).has_value());
			CHECK_FALSE(parse({ { .Handle = AssetHandle(1), .Type = "Mesh", .Path = "a", .Offset = 64, .Size = 32, .Hash = 0 },
								  { .Handle = AssetHandle(2), .Type = "Mesh", .Path = "b", .Offset = 80, .Size = 8, .Hash = 0 } })
					.has_value());
			CHECK_FALSE(parse({ { .Handle = AssetHandle(1), .Type = "Mesh", .Path = "a", .Offset = 64, .Size = 8000, .Hash = 0 } }).has_value());
			CHECK_FALSE(parse({ { .Handle = AssetHandle(1), .Type = "None", .Path = "a", .Offset = 64, .Size = 8, .Hash = 0 } }).has_value());
			CHECK_FALSE(parse({ { .Handle = AssetHandle(1), .Type = "File", .Path = "a", .Offset = 64, .Size = 8, .Hash = 0 } }).has_value());
		}
	}

}
