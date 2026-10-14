#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

// The executable resources of an exported game (Architecture §14.2 step 5, §16 "Export"; Docs/Decisions/
// 0024-m15-contract.md decision 4): the icon group and the version information the exporter patches into the copied
// Windows Runtime. The resource data is built host-independently (ResourcePatcher.cpp), so it is unit-tested on every OS
// and byte-identical for identical input; only Patch touches the OS (Platform/Windows/ResourcePatcherWindows.cpp,
// Platform/Posix/ResourcePatcherPosix.cpp). macOS carries the same data in Info.plist and an .icns
// (EditorCore/Export/AppBundle.h); Linux executables carry none.

namespace Engine {

	// One image of an executable's icon: square, PNG-encoded (RT_ICON entries may hold PNG data on every supported Windows).
	struct ExecutableIconImage
	{
		uint16_t Size = 0; // width = height in pixels, 16 to 256
		Buffer Png{};      // the PNG file's bytes; its IHDR width and height equal Size
	};

	// The content of the VS_VERSIONINFO resource (§14.2 step 5: Version, Company, product and file description = Name).
	struct ExecutableVersionInfo
	{
		uint16_t Major = 0;
		uint16_t Minor = 0;
		uint16_t Patch = 0;
		std::string CompanyName{};      // Export.Company; the CompanyName string is left out when empty
		std::string ProductName{};      // the project's Name: ProductName, FileDescription and InternalName
		std::string OriginalFilename{}; // "<Name>.exe"
	};

	// Everything Patch writes.
	struct ExecutableResourceSet
	{
		std::vector<ExecutableIconImage> Icons{}; // ascending, unique sizes; empty: no icon group (no Export.Icon)
		ExecutableVersionInfo Version{};
	};

	// Static functions only; thread-safe (no shared state).
	class ResourcePatcher
	{
	public:
		ResourcePatcher() = delete;

		// The resource ids and language Patch writes: RT_GROUP_ICON IconGroupId, RT_ICON FirstIconId + i for Icons[i],
		// RT_VERSION VersionId, all in LanguageId (en-US); the version strings use code page 1200 (UTF-16LE), table
		// "040904B0".
		static constexpr uint16_t IconGroupId = 1;
		static constexpr uint16_t FirstIconId = 1;
		static constexpr uint16_t VersionId = 1;
		static constexpr uint16_t LanguageId = 0x0409;
		static constexpr uint16_t CodePage = 1200;

		// The VS_VERSIONINFO block, little-endian, every structure 32-bit aligned (wLength, wValueLength, wType, the
		// UTF-16LE key with its terminator, padding, value, padding, children):
		//   - VS_FIXEDFILEINFO: signature 0xFEEF04BD, structure version 0x00010000, FileVersion = ProductVersion =
		//     Major.Minor.Patch.0, flags mask 0x3F, flags 0, VOS_NT_WINDOWS32, VFT_APP, subtype 0, date 0 (deterministic);
		//   - StringFileInfo / "040904B0": CompanyName (when not empty), FileDescription, FileVersion ("M.m.p.0"),
		//     InternalName, OriginalFilename, ProductName, ProductVersion ("M.m.p.0"), in this order, text values (wType 1)
		//     whose wValueLength counts UTF-16 code units including the terminator;
		//   - VarFileInfo / Translation: 0x0409, 1200.
		// Errors: InvalidArgument for an empty ProductName or OriginalFilename, a string that is not valid UTF-8 or holds a
		// NUL, or a block that would exceed 65535 bytes.
		[[nodiscard]] static Result<Buffer> BuildVersionInfo(const ExecutableVersionInfo& info);

		// The RT_GROUP_ICON block: GRPICONDIR {0, 1, count} and one packed 14-byte GRPICONDIRENTRY per image {width and
		// height (0 for 256), 0 colours, 0, 1 plane, 32 bits per pixel, the PNG's byte count, FirstIconId + index}.
		// Errors: InvalidArgument for no image, more than 16, a size outside 16 to 256, sizes not strictly ascending, or a
		// PNG whose signature or IHDR size does not match its Size.
		[[nodiscard]] static Result<Buffer> BuildIconGroup(std::span<const ExecutableIconImage> icons);

		// Writes `resources` into the executable file `executable` in one update (BeginUpdateResourceW without deleting
		// existing resources, one UpdateResourceW per resource, EndUpdateResourceW), so the RT_MANIFEST the build embedded
		// stays and earlier icon and version resources of the same ids are replaced. A failed update is discarded
		// (EndUpdateResourceW with fDiscard) and leaves the file as it was. A sharing violation (an antivirus scanner holding
		// the freshly copied file) is retried within a bounded wait of 10 s, never a spin count. Identical input gives an
		// identical file. Errors: InvalidArgument from the builders; Io naming the failing call and the system error.
		// Linux and macOS: Unsupported (§16: only Windows executables carry resources).
		[[nodiscard]] static Status Patch(const std::filesystem::path& executable, const ExecutableResourceSet& resources);
	};

}
