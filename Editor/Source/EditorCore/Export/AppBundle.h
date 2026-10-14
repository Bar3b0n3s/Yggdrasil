#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"
#include "Engine/Platform/ResourcePatcher.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

// The macOS application bundle of an exported game (Architecture §14.1, §14.2 steps 4 to 6, §16 "Export"; Docs/Decisions/
// 0024-m15-contract.md decision 5). The writers and the two assembly steps below are host-independent (pure functions and
// plain file-system code), so every OS's unit suite builds and tests them, the local Windows gate included; the exporter
// calls the assembly steps on macOS only, and only its codesign call (ExportPhase::Sign) is macOS code, verified by the
// macOS CI job (ADR 0011).
//
// On macOS the export's output directory holds exactly "<Name>.app":
//   <Name>.app/Contents/Info.plist                              WriteInfoPlist
//   <Name>.app/Contents/MacOS/<Name>                            the Runtime, renamed (ExportReport::Executable)
//   <Name>.app/Contents/Resources/Game.json, Data/*.pak         as on the other platforms
//   <Name>.app/Contents/Resources/THIRD_PARTY_NOTICES.txt       the notices
//   <Name>.app/Contents/Resources/AppIcon.icns                  BuildIcns, with an Export.Icon only
//   <Name>.app/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json   RewriteMoltenVkIcd of the SDK's manifest
//   <Name>.app/Contents/Frameworks/libvulkan.1.dylib, libMoltenVK.dylib   the SDK's loader and driver
// The Runtime loads the bundled loader by its explicit path, <executable directory>/../Frameworks/libvulkan.1.dylib, when that
// file exists, and by leaf name otherwise (M15 stream A, Engine/Graphics/VulkanDispatch.cpp: dlopen of a bare leaf name does
// not search LC_RPATH, so the rpath @executable_path/../Frameworks of Runtime/premake5.lua serves the dylibs' own @rpath
// references only); the loader finds the ICD in the bundle's Resources/vulkan/icd.d. The bundle is ad-hoc signed last (codesign --force --sign -,
// the dylibs first, then the bundle), after Game.json, the paks, the notices, Info.plist and the .icns are written,
// because the signature seals Contents/Resources (ExportPhase::Sign).

namespace Engine {

	// Bundle paths, relative to the output directory's "<Name>.app".
	inline constexpr std::string_view AppBundleContentsDirectory = "Contents";
	inline constexpr std::string_view AppBundleExecutableDirectory = "Contents/MacOS";
	inline constexpr std::string_view AppBundleResourcesDirectory = "Contents/Resources";
	inline constexpr std::string_view AppBundleFrameworksDirectory = "Contents/Frameworks";
	inline constexpr std::string_view AppBundleIcdDirectory = "Contents/Resources/vulkan/icd.d";
	inline constexpr std::string_view AppBundleIconName = "AppIcon"; // CFBundleIconFile; the file is AppIcon.icns
	// The ICD manifest's library_path after RewriteMoltenVkIcd: from Contents/Resources/vulkan/icd.d to Contents/Frameworks.
	inline constexpr std::string_view AppBundleMoltenVkLibraryPath = "../../../Frameworks/libMoltenVK.dylib";

	// What Info.plist states.
	struct AppBundleInfo
	{
		std::string Name{};       // the project's Name: CFBundleName, CFBundleDisplayName and CFBundleExecutable
		std::string Identifier{}; // CFBundleIdentifier (MakeBundleIdentifier)
		std::string Version{};    // "major.minor.patch": CFBundleShortVersionString and CFBundleVersion
		std::string Company{};    // NSHumanReadableCopyright when not empty
		bool HasIcon = false;     // CFBundleIconFile AppBundleIconName
	};

	// "com.<company>.<name>", deterministic: each part keeps ASCII letters and digits, lower-cased, turns every other byte
	// into '-', collapses runs of '-' and trims them at both ends; an empty company part becomes "unknown", an empty name
	// part "app-<the first 8 hex digits of XXH64(name)>". Pure.
	[[nodiscard]] std::string MakeBundleIdentifier(std::string_view company, std::string_view name);

	// Info.plist (XML property list 1.0, UTF-8, LF, tab-indented), keys in this order: CFBundleDevelopmentRegion "en",
	// CFBundleDisplayName, CFBundleExecutable, CFBundleIconFile (HasIcon only), CFBundleIdentifier,
	// CFBundleInfoDictionaryVersion "6.0", CFBundleName, CFBundlePackageType "APPL", CFBundleShortVersionString,
	// CFBundleVersion, LSMinimumSystemVersion "14.0" (§16), NSHighResolutionCapable true, NSHumanReadableCopyright (a
	// non-empty Company only); strings XML-escaped. Errors: InvalidArgument for an empty Name, Identifier or Version, or a
	// string that FindExportTextProblem refuses (not valid UTF-8, or holding a control character, U+FFFE or U+FFFF;
	// Project/ProjectSettings.h), naming its key and the problem.
	[[nodiscard]] Result<std::string> WriteInfoPlist(const AppBundleInfo& info);

	// The .icns file: big-endian (an Apple format, outside §16's little-endian rule) 'icns' and the total length, then one
	// entry per image whose size has an entry type, in this order: icp4 (16), icp5 (32), icp6 (64), ic07 (128), ic08 (256),
	// ic11 (32, 16@2x), ic12 (64, 32@2x), ic13 (256, 128@2x), each the type, its length including the 8-byte header and the
	// image's PNG bytes. Images of other sizes (24, 48) are left out. Errors: InvalidArgument when no image has an entry
	// type, or for an image the icon-group rules of ResourcePatcher::BuildIconGroup reject.
	[[nodiscard]] Result<Buffer> BuildIcns(std::span<const ExecutableIconImage> icons);

	// The Vulkan SDK's MoltenVK_icd.json with ICD.library_path set to AppBundleMoltenVkLibraryPath, written canonically
	// (JsonWriter); every other member is kept. Errors: Parse for invalid JSON; Validation located at /ICD/library_path when
	// the member is missing or not a string.
	[[nodiscard]] Result<std::string> RewriteMoltenVkIcd(std::string_view sdkIcdJson);

	// ExportPhase::CopyRuntime's part of the bundle `bundle` (the staging directory's "<Name>.app", created with its Contents,
	// MacOS, Resources and Frameworks directories): `runtime` copied as Contents/MacOS/<name> with the owner's execute
	// permission; libvulkan.1.dylib and libMoltenVK.dylib of <runtimeOutput>/Frameworks copied into Contents/Frameworks; and
	// <runtimeOutput>/Vulkan/icd.d/MoltenVK_icd.json written through RewriteMoltenVkIcd into AppBundleIcdDirectory.
	// `runtimeOutput` is bin/<OutputDir>/Runtime, where Scripts/Build.py stages the SDK's files. Errors: NotFound with the
	// missing input as ErrorLocation::File (Exporter::IsMissingBuildOutput's rule); RewriteMoltenVkIcd's; Io.
	[[nodiscard]] Status CopyAppBundleBinaries(const std::filesystem::path& bundle, std::string_view name, const std::filesystem::path& runtime,
		const std::filesystem::path& runtimeOutput);

	// ExportPhase::PatchResources' part of `bundle`: Contents/Info.plist from WriteInfoPlist(info) and, when `icons` is not
	// empty, Contents/Resources/AppIcon.icns from BuildIcns(icons). Errors: InvalidArgument when info.HasIcon differs from
	// !icons.empty(); the writers'; Io.
	[[nodiscard]] Status WriteAppBundleResources(const std::filesystem::path& bundle, const AppBundleInfo& info,
		std::span<const ExecutableIconImage> icons);

}
