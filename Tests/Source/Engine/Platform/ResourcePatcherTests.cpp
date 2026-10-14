#include "TestsPCH.h"

#include "Engine/Platform/ResourcePatcher.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Graphics/Image.h"
#include "Support/PlatformProbes.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The resource patcher (Architecture §14.2 step 5; Docs/Decisions/0024-m15-contract.md decision 4, stream A). The builders
// are checked against a test-local walk of the documented layout on every OS; Patch is checked on Windows through the OS's
// own resource readers (Support/PlatformProbes.h).

namespace Engine {

	namespace {

		// One VS_VERSIONINFO-style block: wLength, wValueLength, wType, a UTF-16LE key with its terminator, padding to 32
		// bits, the value, padding, then child blocks up to wLength.
		struct VersionBlock
		{
			std::u16string Key{};
			uint16_t Type = 0;
			std::vector<std::byte> Value{};
			std::vector<VersionBlock> Children{};

			[[nodiscard]] const VersionBlock* Find(std::u16string_view key) const
			{
				for (const VersionBlock& child : Children)
				{
					if (child.Key == key)
						return &child;
				}
				return nullptr;
			}
		};

		uint16_t ReadU16(std::span<const std::byte> bytes, size_t offset)
		{
			return static_cast<uint16_t>(std::to_integer<uint16_t>(bytes[offset]) | (std::to_integer<uint16_t>(bytes[offset + 1]) << 8));
		}

		uint32_t ReadU32(std::span<const std::byte> bytes, size_t offset)
		{
			return static_cast<uint32_t>(ReadU16(bytes, offset)) | (static_cast<uint32_t>(ReadU16(bytes, offset + 2)) << 16);
		}

		size_t Align4(size_t offset)
		{
			return (offset + 3) & ~size_t{ 3 };
		}

		// Parses the block at `offset`; nullopt when it does not fit or is malformed.
		std::optional<VersionBlock> ParseBlock(std::span<const std::byte> bytes, size_t offset)
		{
			if (offset + 6 > bytes.size() || offset % 4 != 0)
				return std::nullopt;
			const size_t length = ReadU16(bytes, offset);
			const size_t valueLength = ReadU16(bytes, offset + 2);
			VersionBlock block;
			block.Type = ReadU16(bytes, offset + 4);
			const size_t end = offset + length;
			if (length < 6 || end > bytes.size())
				return std::nullopt;
			size_t cursor = offset + 6;
			for (;;)
			{
				if (cursor + 2 > end)
					return std::nullopt;
				const char16_t character = static_cast<char16_t>(ReadU16(bytes, cursor));
				cursor += 2;
				if (character == u'\0')
					break;
				block.Key.push_back(character);
			}
			cursor = Align4(cursor);
			const size_t valueBytes = block.Type == 1 ? valueLength * 2 : valueLength;
			if (cursor + valueBytes > end)
				return std::nullopt;
			block.Value.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor), bytes.begin() + static_cast<std::ptrdiff_t>(cursor + valueBytes));
			cursor = Align4(cursor + valueBytes);
			while (cursor < end)
			{
				std::optional<VersionBlock> child = ParseBlock(bytes, cursor);
				if (!child.has_value())
					return std::nullopt;
				cursor = Align4(cursor + ReadU16(bytes, cursor));
				block.Children.push_back(std::move(*child));
			}
			return block;
		}

		// A text value without its terminator.
		std::u16string TextOf(const VersionBlock& block)
		{
			std::u16string text;
			for (size_t offset = 0; offset + 1 < block.Value.size(); offset += 2)
			{
				const char16_t character = static_cast<char16_t>(ReadU16(block.Value, offset));
				if (character == u'\0')
					break;
				text.push_back(character);
			}
			return text;
		}

		// A square PNG of `size` pixels, its colour depending on the size.
		ExecutableIconImage MakeIcon(uint16_t size)
		{
			Result<Image> image = CreateImage(size, size, nvrhi::Format::RGBA8_UNORM);
			REQUIRE(image.has_value());
			for (size_t index = 0; index < image->Pixels.size(); ++index)
				image->Pixels[index] = static_cast<std::byte>((index % 4 == 3) ? 255 : (size + index % 4) & 0xFF);
			Result<Buffer> png = EncodePng(*image);
			REQUIRE(png.has_value());
			return ExecutableIconImage{ .Size = size, .Png = std::move(*png) };
		}

		// The error code of a failed result; nullopt when it succeeded.
		template<typename T>
		std::optional<ErrorCode> FailureCode(const Result<T>& result)
		{
			if (result.has_value())
				return std::nullopt;
			return result.error().GetCode();
		}

		ExecutableVersionInfo MakeVersionInfo()
		{
			return ExecutableVersionInfo{
				.Major = 2,
				.Minor = 3,
				.Patch = 4,
				.CompanyName = "Fixture Studio",
				.ProductName = "Tetromin\xC3\xB6",
				.OriginalFilename = "Tetromin\xC3\xB6.exe",
			};
		}

	}

	TEST_SUITE("Platform")
	{
		TEST_CASE("ResourcePatcher: VS_VERSIONINFO holds the version, company and names")
		{
			const Result<Buffer> built = ResourcePatcher::BuildVersionInfo(MakeVersionInfo());
			REQUIRE_MESSAGE(built.has_value(), built.error().ToString());
			const std::optional<VersionBlock> root = ParseBlock(*built, 0);
			REQUIRE(root.has_value());
			CHECK(ReadU16(*built, 0) == built->size());
			CHECK(root->Key == u"VS_VERSION_INFO");
			REQUIRE(root->Value.size() == 52);
			CHECK(ReadU32(root->Value, 0) == 0xFEEF04BDu);
			CHECK(ReadU32(root->Value, 4) == 0x00010000u);
			CHECK(ReadU32(root->Value, 8) == ((2u << 16) | 3u));  // dwFileVersionMS
			CHECK(ReadU32(root->Value, 12) == (4u << 16));        // dwFileVersionLS
			CHECK(ReadU32(root->Value, 16) == ((2u << 16) | 3u)); // dwProductVersionMS
			CHECK(ReadU32(root->Value, 20) == (4u << 16));        // dwProductVersionLS
			CHECK(ReadU32(root->Value, 32) == 0x00040004u);       // VOS_NT_WINDOWS32
			CHECK(ReadU32(root->Value, 36) == 1u);                // VFT_APP
			CHECK(ReadU32(root->Value, 44) == 0u);                // no date
			CHECK(ReadU32(root->Value, 48) == 0u);

			const VersionBlock* strings = root->Find(u"StringFileInfo");
			REQUIRE(strings != nullptr);
			const VersionBlock* table = strings->Find(u"040904B0");
			REQUIRE(table != nullptr);
			std::vector<std::u16string> keys;
			for (const VersionBlock& entry : table->Children)
			{
				keys.push_back(entry.Key);
				CHECK(entry.Type == 1);
			}
			CHECK(keys == std::vector<std::u16string>{ u"CompanyName", u"FileDescription", u"FileVersion", u"InternalName", u"OriginalFilename", u"ProductName", u"ProductVersion" });
			CHECK(TextOf(*table->Find(u"CompanyName")) == u"Fixture Studio");
			CHECK(TextOf(*table->Find(u"FileDescription")) == u"Tetrominö");
			CHECK(TextOf(*table->Find(u"InternalName")) == u"Tetrominö");
			CHECK(TextOf(*table->Find(u"ProductName")) == u"Tetrominö");
			CHECK(TextOf(*table->Find(u"OriginalFilename")) == u"Tetrominö.exe");
			CHECK(TextOf(*table->Find(u"FileVersion")) == u"2.3.4.0");
			CHECK(TextOf(*table->Find(u"ProductVersion")) == u"2.3.4.0");

			const VersionBlock* variables = root->Find(u"VarFileInfo");
			REQUIRE(variables != nullptr);
			const VersionBlock* translation = variables->Find(u"Translation");
			REQUIRE(translation != nullptr);
			REQUIRE(translation->Value.size() == 4);
			CHECK(ReadU16(translation->Value, 0) == ResourcePatcher::LanguageId);
			CHECK(ReadU16(translation->Value, 2) == ResourcePatcher::CodePage);

			// Identical input, identical bytes; no company, no CompanyName.
			const Result<Buffer> again = ResourcePatcher::BuildVersionInfo(MakeVersionInfo());
			REQUIRE(again.has_value());
			CHECK(*again == *built);
			ExecutableVersionInfo anonymous = MakeVersionInfo();
			anonymous.CompanyName.clear();
			const Result<Buffer> withoutCompany = ResourcePatcher::BuildVersionInfo(anonymous);
			REQUIRE(withoutCompany.has_value());
			const std::optional<VersionBlock> parsed = ParseBlock(*withoutCompany, 0);
			REQUIRE(parsed.has_value());
			const VersionBlock* parsedStrings = parsed->Find(u"StringFileInfo");
			REQUIRE(parsedStrings != nullptr);
			const VersionBlock* parsedTable = parsedStrings->Find(u"040904B0");
			REQUIRE(parsedTable != nullptr);
			CHECK(parsedTable->Find(u"CompanyName") == nullptr);
			CHECK(parsedTable->Find(u"ProductName") != nullptr);

			ExecutableVersionInfo invalid = MakeVersionInfo();
			invalid.ProductName.clear();
			CHECK(FailureCode(ResourcePatcher::BuildVersionInfo(invalid)) == ErrorCode::InvalidArgument);
			invalid = MakeVersionInfo();
			invalid.CompanyName = "\xFF\xFE";
			CHECK(FailureCode(ResourcePatcher::BuildVersionInfo(invalid)) == ErrorCode::InvalidArgument);
		}

		TEST_CASE("ResourcePatcher: the icon group lists every size with its id")
		{
			std::vector<ExecutableIconImage> icons;
			for (const uint16_t size : std::array<uint16_t, 4>{ 16, 32, 48, 256 })
				icons.push_back(MakeIcon(size));
			const Result<Buffer> group = ResourcePatcher::BuildIconGroup(icons);
			REQUIRE_MESSAGE(group.has_value(), group.error().ToString());
			REQUIRE(group->size() == 6 + 14 * icons.size());
			CHECK(ReadU16(*group, 0) == 0);
			CHECK(ReadU16(*group, 2) == 1);
			CHECK(ReadU16(*group, 4) == icons.size());
			for (size_t index = 0; index < icons.size(); ++index)
			{
				CAPTURE(index);
				const size_t entry = 6 + 14 * index;
				const uint8_t expectedSide = icons[index].Size == 256 ? uint8_t{ 0 } : static_cast<uint8_t>(icons[index].Size);
				CHECK(std::to_integer<uint8_t>((*group)[entry]) == expectedSide);
				CHECK(std::to_integer<uint8_t>((*group)[entry + 1]) == expectedSide);
				CHECK(std::to_integer<uint8_t>((*group)[entry + 2]) == 0);
				CHECK(ReadU16(*group, entry + 4) == 1);
				CHECK(ReadU16(*group, entry + 6) == 32);
				CHECK(ReadU32(*group, entry + 8) == icons[index].Png.size());
				CHECK(ReadU16(*group, entry + 12) == ResourcePatcher::FirstIconId + index);
			}

			CHECK(FailureCode(ResourcePatcher::BuildIconGroup({})) == ErrorCode::InvalidArgument);
			const std::vector<ExecutableIconImage> descending = { MakeIcon(32), MakeIcon(16) };
			CHECK(FailureCode(ResourcePatcher::BuildIconGroup(descending)) == ErrorCode::InvalidArgument);
			std::vector<ExecutableIconImage> mislabelled = { MakeIcon(32) };
			mislabelled.front().Size = 48;
			CHECK(FailureCode(ResourcePatcher::BuildIconGroup(mislabelled)) == ErrorCode::InvalidArgument);
			const std::vector<ExecutableIconImage> duplicated = { MakeIcon(32), MakeIcon(32) };
			CHECK(FailureCode(ResourcePatcher::BuildIconGroup(duplicated)) == ErrorCode::InvalidArgument);
			std::vector<ExecutableIconImage> notPng = { MakeIcon(16) };
			notPng.front().Png = Buffer(64, std::byte{ 0x42 });
			CHECK(FailureCode(ResourcePatcher::BuildIconGroup(notPng)) == ErrorCode::InvalidArgument);
			std::vector<ExecutableIconImage> tooSmall = { MakeIcon(16) };
			tooSmall.front().Size = 8;
			CHECK(FailureCode(ResourcePatcher::BuildIconGroup(tooSmall)) == ErrorCode::InvalidArgument);
		}

#if defined(ENGINE_PLATFORM_WINDOWS)
		TEST_CASE("ResourcePatcher: patching keeps the manifest and replaces the icon group and version")
		{
			const Result<std::filesystem::path> runtime = Test::GetBuiltExecutablePath("Runtime");
			REQUIRE_MESSAGE(runtime.has_value(), runtime.error().ToString());
			Test::TempDirectory directory("ResourcePatcher");
			const std::filesystem::path executable = directory / "Tetromino.exe";
			std::error_code error;
			REQUIRE(std::filesystem::copy_file(*runtime, executable, error));

			ExecutableResourceSet resources{ .Icons = { MakeIcon(16), MakeIcon(256) }, .Version = MakeVersionInfo() };
			const Status patched = ResourcePatcher::Patch(executable, resources);
			REQUIRE_MESSAGE(patched.has_value(), patched.error().ToString());
			Result<Test::ExecutableResourceProbe> probe = Test::ProbeExecutableResources(executable);
			REQUIRE_MESSAGE(probe.has_value(), probe.error().ToString());
			CHECK(probe->HasManifest);
			CHECK(probe->FileVersion == std::array<uint16_t, 4>{ 2, 3, 4, 0 });
			CHECK(probe->ProductVersion == std::array<uint16_t, 4>{ 2, 3, 4, 0 });
			CHECK(probe->Strings["CompanyName"] == "Fixture Studio");
			CHECK(probe->Strings["ProductName"] == "Tetromin\xC3\xB6");
			CHECK(probe->Strings["FileDescription"] == "Tetromin\xC3\xB6");
			REQUIRE(probe->Icons.size() == 2);
			CHECK(probe->Icons[0].first == 16);
			CHECK(probe->Icons[0].second == resources.Icons[0].Png);
			CHECK(probe->Icons[1].first == 256);
			CHECK(probe->Icons[1].second == resources.Icons[1].Png);

			// Identical input on identical files gives identical bytes ("two exports identical file for file").
			const std::filesystem::path twin = directory / "Twin.exe";
			REQUIRE(std::filesystem::copy_file(*runtime, twin, error));
			REQUIRE(ResourcePatcher::Patch(twin, resources).has_value());
			const Result<Buffer> first = FileSystem::ReadFile(executable);
			const Result<Buffer> second = FileSystem::ReadFile(twin);
			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			CHECK(*first == *second);

			// A second patch replaces the version and the icon group (the exporter only ever patches a fresh copy).
			resources.Version.CompanyName = "Other Studio";
			resources.Icons = { MakeIcon(32) };
			REQUIRE(ResourcePatcher::Patch(executable, resources).has_value());
			probe = Test::ProbeExecutableResources(executable);
			REQUIRE(probe.has_value());
			CHECK(probe->HasManifest);
			CHECK(probe->Strings["CompanyName"] == "Other Studio");
			REQUIRE(probe->Icons.size() == 1);
			CHECK(probe->Icons[0].first == 32);
		}

		TEST_CASE("ResourcePatcher: a file that is not an executable is an Io error and stays as it was")
		{
			Test::TempDirectory directory("ResourcePatcherInvalid");
			const std::filesystem::path file = directory / "NotAnExecutable.exe";
			constexpr std::string_view Text = "not a portable executable";
			REQUIRE(FileSystem::WriteFileAtomic(file, AsBytes(Text), { .KeepBackup = false }).has_value());
			const ExecutableResourceSet resources{ .Icons = { MakeIcon(16) }, .Version = MakeVersionInfo() };
			const Status patched = ResourcePatcher::Patch(file, resources);
			REQUIRE_FALSE(patched.has_value());
			CHECK(patched.error().GetCode() == ErrorCode::Io);
			CHECK(patched.error().GetMessageText().contains("NotAnExecutable.exe"));
			CHECK(FileSystem::ReadText(file).value_or("") == Text);

			// Invalid resources are refused before the file is opened.
			ExecutableResourceSet invalid = resources;
			invalid.Version.ProductName.clear();
			const Status refused = ResourcePatcher::Patch(file, invalid);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
		}
#else
		TEST_CASE("ResourcePatcher: Patch is Unsupported where executables carry no resources")
		{
			Test::TempDirectory directory("ResourcePatcher");
			const ExecutableResourceSet resources{ .Icons = {}, .Version = MakeVersionInfo() };
			const Status patched = ResourcePatcher::Patch(directory / "Game", resources);
			REQUIRE_FALSE(patched.has_value());
			CHECK(patched.error().GetCode() == ErrorCode::Unsupported);
			CHECK(patched.error().ToString().find("Info.plist") != std::string::npos);
		}
#endif
	}

}
