#include "TestsPCH.h"

#include "Engine/Asset/PakFormat.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>

namespace Engine {

	namespace {

		// A version 2 TOC text with `entries` (a JSON array) and an empty Metadata, spelled by hand so the reader's own checks
		// are tested, not the writer's.
		std::string MakeTocText(std::string_view entries, std::string_view extraMember = {})
		{
			return std::format(R"({{"Format": "PakToc", "Version": 2, "Entries": {}, "Metadata": {{}}{}}})", entries, extraMember);
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("PakFormat: the 64-byte header round-trips")
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
			std::array<std::byte, PakHeader::Size> newer = WritePakHeader({ .Version = PakHeader::CurrentVersion + 1 });
			Result<PakHeader> newerRead = ReadPakHeader(newer);
			REQUIRE_FALSE(newerRead.has_value());
			CHECK(newerRead.error().GetCode() == ErrorCode::UnsupportedVersion);
			CHECK_FALSE(ReadPakHeader(std::span<const std::byte>(bytes).first(63)).has_value());
		}

		TEST_CASE("PakFormat: the header is little-endian with the documented field offsets")
		{
			const std::array<std::byte, PakHeader::Size> bytes = WritePakHeader(
				{ .Version = 1, .EntryCount = 0x0a0b0c0d, .TocOffset = 0x1122334455667788ull, .TocSize = 0x99, .TocHash = 0x0102030405060708ull });
			CHECK(bytes[8] == std::byte{ 0x01 });
			CHECK(bytes[12] == std::byte{ 0x0d });
			CHECK(bytes[15] == std::byte{ 0x0a });
			CHECK(bytes[16] == std::byte{ 0x88 });
			CHECK(bytes[23] == std::byte{ 0x11 });
			CHECK(bytes[24] == std::byte{ 0x99 });
			CHECK(bytes[32] == std::byte{ 0x08 });
			CHECK(bytes[39] == std::byte{ 0x01 });
			CHECK(std::ranges::all_of(std::span(bytes).subspan(40), [](std::byte value)
			{
				return value == std::byte{ 0 };
			}));

			std::array<std::byte, PakHeader::Size> wrongMagic = bytes;
			wrongMagic[0] = std::byte{ 'X' };
			Result<PakHeader> read = ReadPakHeader(wrongMagic);
			REQUIRE_FALSE(read.has_value());
			CHECK(read.error().GetCode() == ErrorCode::Parse);
			Result<PakHeader> older = ReadPakHeader(WritePakHeader({ .Version = 0 }));
			REQUIRE_FALSE(older.has_value());
			CHECK(older.error().GetCode() == ErrorCode::UnsupportedVersion);
		}

		TEST_CASE("PakFormat: the TOC round-trips canonically")
		{
			PakToc toc;
			toc.Entries = {
				{ .Handle = AssetHandle(), .Type = std::string(PakFileEntryType), .Path = "Shaders/Forward_VSMain.spv", .Offset = 64, .Size = 100, .Hash = 1, .StoredSize = 100 },
				{ .Handle = AssetHandle(0x101), .Type = "Mesh", .Path = "engine://Meshes/Cube", .Offset = 176, .Size = 32, .Hash = 2, .StoredSize = 32 },
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

		TEST_CASE("PakFormat: the TOC text has the documented members and spellings")
		{
			PakToc toc;
			toc.Entries = {
				{ .Handle = AssetHandle(), .Type = std::string(PakFileEntryType), .Path = "Shaders/A.spv", .Offset = 64, .Size = 0, .Hash = 0xef46db3751d8e999ull },
				{ .Handle = AssetHandle(0x77e1a0c4d2b95f01ull), .Type = "Mesh", .Path = "Assets/Models/Track.glb#mesh:0:Straight", .Offset = 64, .Size = 8, .Hash = 0xab, .StoredSize = 8 },
			};
			const std::string text = SerializePakToc(toc);
			// Pretty canonical JSON: tab indentation, the null handle as null, handles and hashes as 16 hex digits, the
			// compression by its name after the hash, then the stored size, and a null Metadata written as {}.
			CHECK(text.starts_with("{\n\t\"Format\": \"PakToc\",\n\t\"Version\": 2,\n\t\"Entries\": [\n"));
			CHECK(text.contains("\"Handle\": null"));
			CHECK(text.contains("\"Handle\": \"77e1a0c4d2b95f01\""));
			CHECK(text.contains("\"XXH64\": \"00000000000000ab\",\n\t\t\t\"Compression\": \"None\",\n\t\t\t\"StoredSize\": 8\n"));
			CHECK(text.ends_with("\t\"Metadata\": {}\n}\n"));
			Result<PakToc> read = ParsePakToc(text, 128);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Entries == toc.Entries);
			// The reader keeps the Metadata verbatim: an empty object.
			CHECK(read->Metadata.Get() == Json::object());
		}

		TEST_CASE("PakFormat: misaligned, overlapping and unsorted entries are rejected")
		{
			const auto parse = [](std::vector<PakEntry> entries)
			{
				PakToc toc;
				toc.Entries = std::move(entries);
				return ParsePakToc(SerializePakToc(toc), 4096);
			};
			REQUIRE(parse({ { .Handle = AssetHandle(1), .Type = "Mesh", .Path = "a", .Offset = 64, .Size = 8, .Hash = 0, .StoredSize = 8 } }).has_value());
			CHECK_FALSE(parse({ { .Handle = AssetHandle(1), .Type = "Mesh", .Path = "a", .Offset = 72, .Size = 8, .Hash = 0, .StoredSize = 8 } }).has_value());
			CHECK_FALSE(parse({ { .Handle = AssetHandle(1), .Type = "Mesh", .Path = "a", .Offset = 64, .Size = 32, .Hash = 0, .StoredSize = 32 },
								  { .Handle = AssetHandle(2), .Type = "Mesh", .Path = "b", .Offset = 80, .Size = 8, .Hash = 0, .StoredSize = 8 } })
					.has_value());
			CHECK_FALSE(parse({ { .Handle = AssetHandle(1), .Type = "Mesh", .Path = "a", .Offset = 64, .Size = 8000, .Hash = 0, .StoredSize = 8000 } }).has_value());
			CHECK_FALSE(parse({ { .Handle = AssetHandle(1), .Type = "None", .Path = "a", .Offset = 64, .Size = 8, .Hash = 0, .StoredSize = 8 } }).has_value());
			CHECK_FALSE(parse({ { .Handle = AssetHandle(1), .Type = "File", .Path = "a", .Offset = 64, .Size = 8, .Hash = 0, .StoredSize = 8 } }).has_value());
		}

		TEST_CASE("PakFormat: the reader rejects what the writer never produces")
		{
			const auto rejects = [](const std::string& text, ErrorCode code)
			{
				CAPTURE(text);
				Result<PakToc> read = ParsePakToc(text, 4096);
				REQUIRE_FALSE(read.has_value());
				CHECK(read.error().GetCode() == code);
			};
			const std::string file = R"({"Handle": null, "Type": "File", "Path": "a.spv", "Offset": 64, "Size": 4, "XXH64": "0000000000000001", "Compression": "None", "StoredSize": 4})";
			const std::string mesh = R"({"Handle": "0000000000000101", "Type": "Mesh", "Path": "engine://Meshes/Cube", "Offset": 80, "Size": 4, "XXH64": "0000000000000002", "Compression": "None", "StoredSize": 4})";
			REQUIRE(ParsePakToc(MakeTocText("[" + file + ", " + mesh + "]"), 4096).has_value());

			rejects("not json", ErrorCode::Parse);
			rejects(R"({"Format": "Other", "Version": 2, "Entries": [], "Metadata": {}})", ErrorCode::Validation);
			rejects(R"({"Format": "PakToc", "Version": 3, "Entries": [], "Metadata": {}})", ErrorCode::UnsupportedVersion);
			rejects(MakeTocText("[]", R"(, "Extra": 1)"), ErrorCode::Validation);
			rejects(R"({"Format": "PakToc", "Version": 2, "Entries": [], "Metadata": null})", ErrorCode::Validation);
			// Version 1, which readers no longer read, and a version 1 entry, without its Compression and StoredSize.
			rejects(R"({"Format": "PakToc", "Version": 1, "Entries": [], "Metadata": {}})", ErrorCode::Validation);
			rejects(MakeTocText(R"([{"Handle": null, "Type": "File", "Path": "a.spv", "Offset": 64, "Size": 4, "XXH64": "0000000000000001"}])"),
				ErrorCode::Validation);
			// Unsorted (the cooked asset before the plain file), a repeated path, a repeated handle.
			rejects(MakeTocText("[" + mesh + ", " + file + "]"), ErrorCode::Validation);
			rejects(MakeTocText("[" + file + R"(, {"Handle": "0000000000000101", "Type": "Mesh", "Path": "a.spv", "Offset": 80, "Size": 4, "XXH64": "0000000000000002", "Compression": "None", "StoredSize": 4}])"),
				ErrorCode::Validation);
			rejects(MakeTocText("[" + mesh + R"(, {"Handle": "0000000000000101", "Type": "Mesh", "Path": "engine://Meshes/Other", "Offset": 96, "Size": 4, "XXH64": "0000000000000003", "Compression": "None", "StoredSize": 4}])"),
				ErrorCode::Validation);
			// The null handle spelled as sixteen zeros, a plain file below a parent escape, an unknown entry member, a bad hash,
			// an entry inside the header, an unknown type.
			rejects(MakeTocText(R"([{"Handle": "0000000000000000", "Type": "File", "Path": "a.spv", "Offset": 64, "Size": 4, "XXH64": "0000000000000001", "Compression": "None", "StoredSize": 4}])"),
				ErrorCode::Validation);
			rejects(MakeTocText(R"([{"Handle": null, "Type": "File", "Path": "../a.spv", "Offset": 64, "Size": 4, "XXH64": "0000000000000001", "Compression": "None", "StoredSize": 4}])"),
				ErrorCode::Validation);
			rejects(MakeTocText(R"([{"Handle": null, "Type": "File", "Path": "a.spv", "Offset": 64, "Size": 4, "XXH64": "0000000000000001", "Compression": "None", "StoredSize": 4, "Extra": true}])"),
				ErrorCode::Validation);
			rejects(MakeTocText(R"([{"Handle": null, "Type": "File", "Path": "a.spv", "Offset": 64, "Size": 4, "XXH64": "xyz", "Compression": "None", "StoredSize": 4}])"), ErrorCode::Validation);
			rejects(MakeTocText(R"([{"Handle": null, "Type": "File", "Path": "a.spv", "Offset": 32, "Size": 4, "XXH64": "0000000000000001", "Compression": "None", "StoredSize": 4}])"),
				ErrorCode::Validation);
			rejects(MakeTocText(R"([{"Handle": "0000000000000101", "Type": "Gizmo", "Path": "x", "Offset": 64, "Size": 4, "XXH64": "0000000000000001", "Compression": "None", "StoredSize": 4}])"),
				ErrorCode::Validation);
		}

		TEST_CASE("PakFormat: empty entries may sit at the end of the entry data")
		{
			// An empty file is laid out at the next aligned offset, which can be where the TOC starts.
			PakToc toc;
			toc.Entries = {
				{ .Handle = AssetHandle(), .Type = std::string(PakFileEntryType), .Path = "Empty.txt", .Offset = 80, .Size = 0, .Hash = 0 },
				{ .Handle = AssetHandle(), .Type = std::string(PakFileEntryType), .Path = "Full.txt", .Offset = 64, .Size = 16, .Hash = 0, .StoredSize = 16 },
			};
			Result<PakToc> read = ParsePakToc(SerializePakToc(toc), 80);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Entries.size() == 2);
		}

		// Docs/Decisions/0030-m19-contract.md decision 17 (B-PAKZ): pak version 2 gives every entry its Compression and
		// StoredSize after its XXH64; the byte ranges are those of the stored bytes, a None entry stores exactly its Size,
		// and a Deflate entry saves at least an eighth of it. Readers read the current version only.
		TEST_CASE("PakFormat: version 2 entries carry their compression and stored size, and the ranges cover the stored bytes")
		{
			CHECK(PakHeader::CurrentVersion == PakCompressionFormatVersion);
			CHECK(PakToc::CurrentVersion == PakCompressionFormatVersion);
			Result<PakHeader> older = ReadPakHeader(WritePakHeader({ .Version = 1 }));
			REQUIRE_FALSE(older.has_value());
			CHECK(older.error().GetCode() == ErrorCode::UnsupportedVersion);

			PakToc toc;
			toc.Entries = {
				{ .Handle = AssetHandle(), .Type = std::string(PakFileEntryType), .Path = "Shaders/A.spv", .Offset = 64, .Size = 100, .Hash = 1, .Compression = PakCompression::None, .StoredSize = 100 },
				{ .Handle = AssetHandle(0x101), .Type = "Mesh", .Path = "engine://Meshes/Cube", .Offset = 176, .Size = 4096, .Hash = 2, .Compression = PakCompression::Deflate, .StoredSize = 512 },
			};
			// An object Metadata, so the TOC reads back equal (a null one is written, and read back, as {}).
			toc.Metadata = VariantValue(Json::object());
			const std::string text = SerializePakToc(toc);
			CHECK(text.starts_with("{\n\t\"Format\": \"PakToc\",\n\t\"Version\": 2,\n"));
			CHECK(text.contains("\"Compression\": \"None\""));
			CHECK(text.contains("\"Compression\": \"Deflate\""));
			CHECK(text.contains("\"StoredSize\": 512"));
			CHECK(text.find("\"XXH64\"") < text.find("\"Compression\""));
			CHECK(text.find("\"Compression\"") < text.find("\"StoredSize\""));
			// The deflated entry's 4096 bytes would leave a data area that ends at 1024; its 512 stored bytes fit.
			Result<PakToc> read = ParsePakToc(text, 1024);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(*read == toc);
			CHECK(SerializePakToc(*read) == text);

			// Entries overlap only by their stored bytes: a file may start inside the deflated entry's Size but after its
			// StoredSize, never inside the stored bytes [64, 576).
			PakToc packed;
			packed.Entries = {
				{ .Handle = AssetHandle(), .Type = std::string(PakFileEntryType), .Path = "Shaders/B.spv", .Offset = 576, .Size = 16, .Hash = 3, .Compression = PakCompression::None, .StoredSize = 16 },
				{ .Handle = AssetHandle(0x101), .Type = "Mesh", .Path = "engine://Meshes/Cube", .Offset = 64, .Size = 4096, .Hash = 2, .Compression = PakCompression::Deflate, .StoredSize = 512 },
			};
			const Result<PakToc> packedRead = ParsePakToc(SerializePakToc(packed), 1024);
			REQUIRE_MESSAGE(packedRead.has_value(), packedRead.error().ToString());
			packed.Entries[0].Offset = 560;
			const Result<PakToc> overlapping = ParsePakToc(SerializePakToc(packed), 1024);
			REQUIRE_FALSE(overlapping.has_value());
			CHECK(overlapping.error().GetCode() == ErrorCode::Validation);

			const auto rejects = [&toc](size_t index, PakCompression compression, uint64_t storedSize)
			{
				PakToc edited = toc;
				edited.Entries[index].Compression = compression;
				edited.Entries[index].StoredSize = storedSize;
				CAPTURE(index);
				CAPTURE(storedSize);
				Result<PakToc> parsed = ParsePakToc(SerializePakToc(edited), 8192);
				REQUIRE_FALSE(parsed.has_value());
				CHECK(parsed.error().GetCode() == ErrorCode::Validation);
			};
			// A stored entry whose StoredSize is not its Size; a deflated one that saves nothing, less than an eighth, or is
			// empty.
			rejects(0, PakCompression::None, 99);
			rejects(1, PakCompression::Deflate, 4096);
			rejects(1, PakCompression::Deflate, 3600);
			rejects(1, PakCompression::Deflate, 0);

			std::string unknown = text;
			unknown.replace(unknown.find("\"Deflate\""), std::string_view("\"Deflate\"").size(), "\"Zstd\"");
			Result<PakToc> unknownRead = ParsePakToc(unknown, 1024);
			REQUIRE_FALSE(unknownRead.has_value());
			CHECK(unknownRead.error().GetCode() == ErrorCode::Validation);
		}

		// Decision 17's rule for a deflated entry, shared by the writer and the reader: a non-empty stream that saves at
		// least an eighth (8 × (Size − StoredSize) >= Size), decided without overflow for any size.
		TEST_CASE("PakFormat: a deflated size is valid exactly when the stream is not empty and saves at least an eighth")
		{
			CHECK(IsValidPakDeflateSize(8, 7));
			CHECK_FALSE(IsValidPakDeflateSize(9, 8));
			CHECK(IsValidPakDeflateSize(9, 7));
			CHECK(IsValidPakDeflateSize(4096, 3584));
			CHECK_FALSE(IsValidPakDeflateSize(4096, 3585));
			CHECK(IsValidPakDeflateSize(4096, 1));
			CHECK_FALSE(IsValidPakDeflateSize(4096, 0));
			CHECK_FALSE(IsValidPakDeflateSize(4096, 4096));
			CHECK_FALSE(IsValidPakDeflateSize(4096, 5000));
			CHECK_FALSE(IsValidPakDeflateSize(0, 0));
			CHECK_FALSE(IsValidPakDeflateSize(1, 0));
			CHECK(IsValidPakDeflateSize(7, 6));
			// The largest size: the saving must be at least 2^61 bytes, ceil((2^64 - 1) / 8).
			constexpr uint64_t Largest = std::numeric_limits<uint64_t>::max();
			CHECK(IsValidPakDeflateSize(Largest, Largest - (uint64_t{ 1 } << 61)));
			CHECK_FALSE(IsValidPakDeflateSize(Largest, Largest - (uint64_t{ 1 } << 61) + 1));
		}
	}

}
