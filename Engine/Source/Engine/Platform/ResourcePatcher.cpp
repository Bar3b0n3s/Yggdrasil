#include "EnginePCH.h"
#include "Engine/Platform/ResourcePatcher.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Utf8.h"

#include <algorithm>
#include <array>
#include <format>
#include <limits>

// The host-independent half of the resource patcher (ResourcePatcher.h): the VS_VERSIONINFO and GRPICONDIR blocks, built
// byte by byte in little-endian order, so they are identical on every OS and for identical input.

namespace Engine {

	namespace Utils {

		// VS_FIXEDFILEINFO's constants (winver.h), spelled here because this file includes no OS header.
		constexpr uint32_t FixedFileInfoSignature = 0xFEEF04BDu;
		constexpr uint32_t FixedFileInfoStructureVersion = 0x00010000u;
		constexpr uint32_t FixedFileInfoFlagsMask = 0x3Fu;           // VS_FFI_FILEFLAGSMASK
		constexpr uint32_t OperatingSystemNtWindows32 = 0x00040004u; // VOS_NT_WINDOWS32
		constexpr uint32_t FileTypeApplication = 1u;                 // VFT_APP
		constexpr uint16_t BinaryValue = 0;                          // wType of a block whose value is binary data
		constexpr uint16_t TextValue = 1;                            // wType of a block whose value is text
		constexpr size_t FixedFileInfoSize = 13 * sizeof(uint32_t);
		// RT_GROUP_ICON's GRPICONDIR and its packed GRPICONDIRENTRY.
		constexpr size_t IconDirectorySize = 6;
		constexpr size_t IconDirectoryEntrySize = 14;
		constexpr size_t MaxIconCount = 16;
		constexpr uint16_t MinIconSize = 16;
		constexpr uint16_t MaxIconSize = 256;
		constexpr std::array<std::byte, 8> PngSignature = { std::byte{ 0x89 }, std::byte{ 'P' }, std::byte{ 'N' }, std::byte{ 'G' }, std::byte{ 0x0D },
			std::byte{ 0x0A }, std::byte{ 0x1A }, std::byte{ 0x0A } };

		// A little-endian byte writer with the 32-bit alignment VS_VERSIONINFO blocks need.
		class ResourceWriter
		{
		public:
			void WriteU8(uint8_t value) { m_Bytes.push_back(static_cast<std::byte>(value)); }

			void WriteU16(uint16_t value)
			{
				WriteU8(static_cast<uint8_t>(value & 0xFFu));
				WriteU8(static_cast<uint8_t>(value >> 8));
			}

			void WriteU32(uint32_t value)
			{
				WriteU16(static_cast<uint16_t>(value & 0xFFFFu));
				WriteU16(static_cast<uint16_t>(value >> 16));
			}

			// UTF-16LE code units followed by a terminating zero unit.
			void WriteText(std::u16string_view text)
			{
				for (const char16_t unit : text)
					WriteU16(static_cast<uint16_t>(unit));
				WriteU16(0);
			}

			void Align()
			{
				while (m_Bytes.size() % 4 != 0)
					WriteU8(0);
			}

			void PatchU16(size_t offset, uint16_t value)
			{
				m_Bytes[offset] = static_cast<std::byte>(value & 0xFFu);
				m_Bytes[offset + 1] = static_cast<std::byte>(value >> 8);
			}

			[[nodiscard]] size_t GetSize() const { return m_Bytes.size(); }
			[[nodiscard]] Buffer Take() { return std::move(m_Bytes); }
		private:
			Buffer m_Bytes{};
		};

		// The UTF-16 form of `text`, which is valid UTF-8 (checked by the caller).
		static std::u16string DecodeUtf8ToUtf16(std::string_view text)
		{
			std::u16string result;
			size_t index = 0;
			while (index < text.size())
			{
				const auto byte = static_cast<uint8_t>(text[index]);
				uint32_t codePoint = 0;
				size_t length = 1;
				if (byte < 0x80)
					codePoint = byte;
				else if ((byte & 0xE0u) == 0xC0u)
				{
					codePoint = byte & 0x1Fu;
					length = 2;
				}
				else if ((byte & 0xF0u) == 0xE0u)
				{
					codePoint = byte & 0x0Fu;
					length = 3;
				}
				else
				{
					codePoint = byte & 0x07u;
					length = 4;
				}
				for (size_t continuation = 1; continuation < length; ++continuation)
					codePoint = (codePoint << 6) | (static_cast<uint8_t>(text[index + continuation]) & 0x3Fu);
				index += length;
				if (codePoint >= 0x10000u)
				{
					codePoint -= 0x10000u;
					result.push_back(static_cast<char16_t>(0xD800u + (codePoint >> 10)));
					result.push_back(static_cast<char16_t>(0xDC00u + (codePoint & 0x3FFu)));
				}
				else
				{
					result.push_back(static_cast<char16_t>(codePoint));
				}
			}
			return result;
		}

		// The UTF-16 form of the version string `field`, refusing what a version resource cannot hold.
		static Result<std::u16string> ToVersionText(std::string_view field, std::string_view value)
		{
			if (!IsValidUtf8(value))
				return MakeError(ErrorCode::InvalidArgument, "the version resource's {} is not valid UTF-8", field);
			if (value.find('\0') != std::string_view::npos)
				return MakeError(ErrorCode::InvalidArgument, "the version resource's {} holds a NUL character", field);
			return DecodeUtf8ToUtf16(value);
		}

		// Writes one block: its header and key, then `writeValue` and `writeChildren` (each child starting 32-bit aligned),
		// and sets wLength to the bytes written. `valueLength` is wValueLength: bytes for binary values, UTF-16 units with
		// the terminator for text values, 0 for none.
		template<typename ValueWriter, typename ChildrenWriter>
		static void WriteBlock(ResourceWriter& writer, std::u16string_view key, uint16_t type, uint16_t valueLength, ValueWriter&& writeValue,
			ChildrenWriter&& writeChildren)
		{
			writer.Align();
			const size_t start = writer.GetSize();
			writer.WriteU16(0); // wLength, patched below
			writer.WriteU16(valueLength);
			writer.WriteU16(type);
			writer.WriteText(key);
			writer.Align();
			writeValue();
			writeChildren();
			const size_t length = writer.GetSize() - start;
			writer.PatchU16(start, static_cast<uint16_t>(std::min<size_t>(length, std::numeric_limits<uint16_t>::max())));
		}

		// One String block of the string table: a text value with its terminator.
		static void WriteStringEntry(ResourceWriter& writer, std::u16string_view key, std::u16string_view value)
		{
			WriteBlock(
				writer, key, TextValue, static_cast<uint16_t>(value.size() + 1), [&writer, value]()
			{
				writer.WriteText(value);
			}, []() {});
		}

		// Reads the big-endian 32-bit value at `offset` of `bytes`.
		static uint32_t ReadBigEndianU32(std::span<const std::byte> bytes, size_t offset)
		{
			return (std::to_integer<uint32_t>(bytes[offset]) << 24) | (std::to_integer<uint32_t>(bytes[offset + 1]) << 16)
				| (std::to_integer<uint32_t>(bytes[offset + 2]) << 8) | std::to_integer<uint32_t>(bytes[offset + 3]);
		}

		// Checks that the PNG of `icon` (image `index` of its group) has an IHDR of Size x Size pixels.
		static Status CheckIconPng(const ExecutableIconImage& icon, size_t index)
		{
			// The signature (8 bytes), then the IHDR chunk: its length (4), "IHDR" (4), width (4) and height (4).
			constexpr size_t HeaderSize = 8 + 4 + 4 + 4 + 4;
			const std::span<const std::byte> bytes(icon.Png);
			if (bytes.size() < HeaderSize || !std::equal(PngSignature.begin(), PngSignature.end(), bytes.begin()))
				return MakeError(ErrorCode::InvalidArgument, "icon image {} ({} px) is not a PNG", index, icon.Size);
			const bool isHeader = bytes[12] == std::byte{ 'I' } && bytes[13] == std::byte{ 'H' } && bytes[14] == std::byte{ 'D' } && bytes[15] == std::byte{ 'R' };
			const uint32_t width = ReadBigEndianU32(bytes, 16);
			const uint32_t height = ReadBigEndianU32(bytes, 20);
			if (!isHeader || width != icon.Size || height != icon.Size)
			{
				return MakeError(ErrorCode::InvalidArgument, "icon image {} is labelled {} px, but its PNG is {} x {} pixels", index, icon.Size, width,
					height);
			}
			return {};
		}

	}

	Result<Buffer> ResourcePatcher::BuildVersionInfo(const ExecutableVersionInfo& info)
	{
		if (info.ProductName.empty())
			return MakeError(ErrorCode::InvalidArgument, "the version resource needs a product name (the project's Name)");
		if (info.OriginalFilename.empty())
			return MakeError(ErrorCode::InvalidArgument, "the version resource needs the executable's original file name");
		ENGINE_TRY_ASSIGN(const std::u16string company, Utils::ToVersionText("CompanyName", info.CompanyName));
		ENGINE_TRY_ASSIGN(const std::u16string product, Utils::ToVersionText("ProductName", info.ProductName));
		ENGINE_TRY_ASSIGN(const std::u16string filename, Utils::ToVersionText("OriginalFilename", info.OriginalFilename));
		const std::string versionText = std::format("{}.{}.{}.0", info.Major, info.Minor, info.Patch);
		const std::u16string version = Utils::DecodeUtf8ToUtf16(versionText);

		Utils::ResourceWriter writer;
		const auto writeFixedInfo = [&writer, &info]()
		{
			const uint32_t versionMs = (static_cast<uint32_t>(info.Major) << 16) | info.Minor;
			const uint32_t versionLs = static_cast<uint32_t>(info.Patch) << 16;
			writer.WriteU32(Utils::FixedFileInfoSignature);
			writer.WriteU32(Utils::FixedFileInfoStructureVersion);
			writer.WriteU32(versionMs); // dwFileVersionMS
			writer.WriteU32(versionLs); // dwFileVersionLS
			writer.WriteU32(versionMs); // dwProductVersionMS
			writer.WriteU32(versionLs); // dwProductVersionLS
			writer.WriteU32(Utils::FixedFileInfoFlagsMask);
			writer.WriteU32(0); // dwFileFlags
			writer.WriteU32(Utils::OperatingSystemNtWindows32);
			writer.WriteU32(Utils::FileTypeApplication);
			writer.WriteU32(0); // dwFileSubtype
			writer.WriteU32(0); // dwFileDateMS: no date, so identical input gives identical bytes
			writer.WriteU32(0); // dwFileDateLS
		};
		const auto writeStrings = [&]()
		{
			Utils::WriteBlock(writer, u"StringFileInfo", Utils::TextValue, 0, []() {}, [&]()
			{
				Utils::WriteBlock(writer, u"040904B0", Utils::TextValue, 0, []() {}, [&]()
				{
					if (!company.empty())
						Utils::WriteStringEntry(writer, u"CompanyName", company);
					Utils::WriteStringEntry(writer, u"FileDescription", product);
					Utils::WriteStringEntry(writer, u"FileVersion", version);
					Utils::WriteStringEntry(writer, u"InternalName", product);
					Utils::WriteStringEntry(writer, u"OriginalFilename", filename);
					Utils::WriteStringEntry(writer, u"ProductName", product);
					Utils::WriteStringEntry(writer, u"ProductVersion", version);
				});
			});
			Utils::WriteBlock(writer, u"VarFileInfo", Utils::TextValue, 0, []() {}, [&writer]()
			{
				Utils::WriteBlock(
					writer, u"Translation", Utils::BinaryValue, 4, [&writer]()
				{
					writer.WriteU16(LanguageId);
					writer.WriteU16(CodePage);
				}, []() {});
			});
		};
		Utils::WriteBlock(writer, u"VS_VERSION_INFO", Utils::BinaryValue, static_cast<uint16_t>(Utils::FixedFileInfoSize), writeFixedInfo, writeStrings);
		if (writer.GetSize() > std::numeric_limits<uint16_t>::max())
		{
			return MakeError(ErrorCode::InvalidArgument, "the version resource would be {} bytes, more than the 65535 a VS_VERSIONINFO can hold",
				writer.GetSize());
		}
		return writer.Take();
	}

	Result<Buffer> ResourcePatcher::BuildIconGroup(std::span<const ExecutableIconImage> icons)
	{
		if (icons.empty())
			return MakeError(ErrorCode::InvalidArgument, "an icon group needs at least one image");
		if (icons.size() > Utils::MaxIconCount)
			return MakeError(ErrorCode::InvalidArgument, "an icon group holds at most {} images, not {}", Utils::MaxIconCount, icons.size());
		for (size_t index = 0; index < icons.size(); ++index)
		{
			const ExecutableIconImage& icon = icons[index];
			if (icon.Size < Utils::MinIconSize || icon.Size > Utils::MaxIconSize)
			{
				return MakeError(ErrorCode::InvalidArgument, "icon image {} is {} px; icon images are {} to {} px", index, icon.Size, Utils::MinIconSize,
					Utils::MaxIconSize);
			}
			if (index > 0 && icon.Size <= icons[index - 1].Size)
			{
				return MakeError(ErrorCode::InvalidArgument, "icon image {} ({} px) does not follow {} px: the sizes must be strictly ascending", index,
					icon.Size, icons[index - 1].Size);
			}
			if (icon.Png.size() > std::numeric_limits<uint32_t>::max())
				return MakeError(ErrorCode::InvalidArgument, "icon image {} ({} px) is larger than 4 GiB", index, icon.Size);
			ENGINE_TRY(Utils::CheckIconPng(icon, index));
		}

		Utils::ResourceWriter writer;
		writer.WriteU16(0); // idReserved
		writer.WriteU16(1); // idType: icon
		writer.WriteU16(static_cast<uint16_t>(icons.size()));
		for (size_t index = 0; index < icons.size(); ++index)
		{
			const ExecutableIconImage& icon = icons[index];
			// A side of 256 is written as 0, the byte's only way to say 256.
			const uint8_t side = icon.Size == Utils::MaxIconSize ? uint8_t{ 0 } : static_cast<uint8_t>(icon.Size);
			writer.WriteU8(side); // bWidth
			writer.WriteU8(side); // bHeight
			writer.WriteU8(0);    // bColorCount: not a palette image
			writer.WriteU8(0);    // bReserved
			writer.WriteU16(1);   // wPlanes
			writer.WriteU16(32);  // wBitCount
			writer.WriteU32(static_cast<uint32_t>(icon.Png.size()));
			writer.WriteU16(static_cast<uint16_t>(FirstIconId + index)); // nId
		}
		ENGINE_CORE_ASSERT(writer.GetSize() == Utils::IconDirectorySize + Utils::IconDirectoryEntrySize * icons.size(), "GRPICONDIR layout");
		return writer.Take();
	}

}
