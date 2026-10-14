#include "EditorPCH.h"
#include "EditorCore/Export/AppBundle.h"

#include "Engine/Core/Error.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Project/ProjectSettings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

		// LSMinimumSystemVersion: the oldest macOS the engine supports (§16).
		constexpr std::string_view MinimumMacOsVersion = "14.0";
		// What CopyAppBundleBinaries copies from the build output's Runtime directory, where Scripts/Build.py stages it.
		constexpr std::string_view FrameworksSourceDirectory = "Frameworks";
		constexpr std::array<std::string_view, 2> BundleFrameworks = { "libvulkan.1.dylib", "libMoltenVK.dylib" };
		constexpr std::string_view IcdManifestName = "MoltenVK_icd.json";
		constexpr std::string_view IcdSourceDirectory = "Vulkan/icd.d";

		// The .icns entry types BuildIcns writes, in order, with the pixel size each holds.
		struct IcnsEntryType
		{
			std::string_view Type{};
			uint16_t Size = 0;
		};
		constexpr std::array<IcnsEntryType, 8> IcnsEntryTypes = { {
			{ "icp4", 16 },
			{ "icp5", 32 },
			{ "icp6", 64 },
			{ "ic07", 128 },
			{ "ic08", 256 },
			{ "ic11", 32 },
			{ "ic12", 64 },
			{ "ic13", 256 },
		} };

		// One part of a bundle identifier: ASCII letters and digits lower-cased, every run of other bytes one '-', no '-' at
		// either end.
		[[nodiscard]] static std::string SanitizeIdentifierPart(std::string_view text)
		{
			std::string part;
			bool pendingDash = false;
			for (const char character : text)
			{
				const bool letter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
				const bool digit = character >= '0' && character <= '9';
				if (!letter && !digit)
				{
					pendingDash = !part.empty();
					continue;
				}
				if (pendingDash)
					part.push_back('-');
				pendingDash = false;
				part.push_back(character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character);
			}
			return part;
		}

		// Checks a string Info.plist holds against the one rule of an exported executable's text (FindExportTextProblem: valid
		// UTF-8, no control character, neither U+FFFE nor U+FFFF, which XML 1.0 cannot carry).
		[[nodiscard]] static Status CheckPlistText(std::string_view key, std::string_view value)
		{
			if (const std::optional<std::string> problem = FindExportTextProblem(value))
				return MakeError(ErrorCode::InvalidArgument, "the Info.plist value of {} {}", key, *problem);
			return {};
		}

		// `text` with the five XML special characters escaped.
		[[nodiscard]] static std::string EscapeXml(std::string_view text)
		{
			std::string escaped;
			escaped.reserve(text.size());
			for (const char character : text)
			{
				switch (character)
				{
					case '&':  escaped += "&amp;"; break;
					case '<':  escaped += "&lt;"; break;
					case '>':  escaped += "&gt;"; break;
					case '"':  escaped += "&quot;"; break;
					case '\'': escaped += "&apos;"; break;
					default:   escaped.push_back(character); break;
				}
			}
			return escaped;
		}

		static void AppendBigEndianU32(Buffer& bytes, uint32_t value)
		{
			for (const int shift : { 24, 16, 8, 0 })
				bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
		}

		// The NotFound of a missing build output (Exporter::IsMissingBuildOutput's rule): the path as the error's file and
		// the build script in the message.
		[[nodiscard]] static Error MissingBundleInput(const std::filesystem::path& file)
		{
			ErrorLocation location;
			location.File = FileSystem::PathToUtf8(file);
			return Error(ErrorCode::NotFound,
				std::format("the bundle's input '{}' is missing: build it with python Scripts/Build.py --project Runtime, which stages the Vulkan "
							"SDK's loader and MoltenVK on macOS",
					location.File))
				.WithLocation(std::move(location))
				.WithHint("exports package what Scripts/Build.py builds; the editor never builds");
		}

		// Copies `source` to `target` byte for byte, with `source`'s permissions plus `extra`.
		[[nodiscard]] static Status CopyBundleFile(const std::filesystem::path& source, const std::filesystem::path& target,
			std::filesystem::perms extra = std::filesystem::perms::none)
		{
			std::error_code error;
			if (!std::filesystem::is_regular_file(source, error))
				return std::unexpected(MissingBundleInput(source));
			ENGINE_TRY_ASSIGN(const Buffer bytes, FileSystem::ReadFile(source));
			ENGINE_TRY(FileSystem::CreateDirectories(target.parent_path()));
			ENGINE_TRY(FileSystem::WriteFileAtomic(target, bytes, { .KeepBackup = false }));
			const std::filesystem::file_status status = std::filesystem::status(source, error);
			if (!error)
				std::filesystem::permissions(target, status.permissions() | extra, std::filesystem::perm_options::replace, error);
			if (error)
			{
				return MakeError(ErrorCode::Io, "cannot give '{}' the permissions of '{}': {}", FileSystem::PathToUtf8(target), FileSystem::PathToUtf8(source),
					error.message());
			}
			return {};
		}

	}

	std::string MakeBundleIdentifier(std::string_view company, std::string_view name)
	{
		std::string companyPart = Utils::SanitizeIdentifierPart(company);
		if (companyPart.empty())
			companyPart = "unknown";
		std::string namePart = Utils::SanitizeIdentifierPart(name);
		if (namePart.empty())
			namePart = std::format("app-{:08x}", static_cast<uint32_t>(XXH64(name) >> 32));
		return std::format("com.{}.{}", companyPart, namePart);
	}

	Result<std::string> WriteInfoPlist(const AppBundleInfo& info)
	{
		if (info.Name.empty())
			return MakeError(ErrorCode::InvalidArgument, "Info.plist needs the game's name (CFBundleName)");
		if (info.Identifier.empty())
			return MakeError(ErrorCode::InvalidArgument, "Info.plist needs a bundle identifier (CFBundleIdentifier)");
		if (info.Version.empty())
			return MakeError(ErrorCode::InvalidArgument, "Info.plist needs a version (CFBundleShortVersionString)");
		ENGINE_TRY(Utils::CheckPlistText("CFBundleName", info.Name));
		ENGINE_TRY(Utils::CheckPlistText("CFBundleIdentifier", info.Identifier));
		ENGINE_TRY(Utils::CheckPlistText("CFBundleShortVersionString", info.Version));
		ENGINE_TRY(Utils::CheckPlistText("NSHumanReadableCopyright", info.Company));

		std::string text = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
						   "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
						   "<plist version=\"1.0\">\n"
						   "<dict>\n";
		const auto writeString = [&text](std::string_view key, std::string_view value)
		{
			text += std::format("\t<key>{}</key>\n\t<string>{}</string>\n", key, Utils::EscapeXml(value));
		};
		writeString("CFBundleDevelopmentRegion", "en");
		writeString("CFBundleDisplayName", info.Name);
		writeString("CFBundleExecutable", info.Name);
		if (info.HasIcon)
			writeString("CFBundleIconFile", AppBundleIconName);
		writeString("CFBundleIdentifier", info.Identifier);
		writeString("CFBundleInfoDictionaryVersion", "6.0");
		writeString("CFBundleName", info.Name);
		writeString("CFBundlePackageType", "APPL");
		writeString("CFBundleShortVersionString", info.Version);
		writeString("CFBundleVersion", info.Version);
		writeString("LSMinimumSystemVersion", Utils::MinimumMacOsVersion);
		text += "\t<key>NSHighResolutionCapable</key>\n\t<true/>\n";
		if (!info.Company.empty())
			writeString("NSHumanReadableCopyright", info.Company);
		text += "</dict>\n</plist>\n";
		return text;
	}

	Result<Buffer> BuildIcns(std::span<const ExecutableIconImage> icons)
	{
		// The same image rules as the Windows icon group: ascending unique sizes of 16 to 256 px, PNGs of their size.
		if (!icons.empty())
			ENGINE_TRY(WithContext(ResourcePatcher::BuildIconGroup(icons), "while checking the .icns images"));
		Buffer body;
		for (const Utils::IcnsEntryType& entry : Utils::IcnsEntryTypes)
		{
			const auto image = std::ranges::find(icons, entry.Size, &ExecutableIconImage::Size);
			if (image == icons.end())
				continue;
			if (image->Png.size() > std::numeric_limits<uint32_t>::max() - 8)
				return MakeError(ErrorCode::InvalidArgument, "the {} px icon image is too large for an .icns entry", entry.Size);
			for (const char character : entry.Type)
				body.push_back(static_cast<std::byte>(character));
			Utils::AppendBigEndianU32(body, static_cast<uint32_t>(image->Png.size() + 8));
			body.insert(body.end(), image->Png.begin(), image->Png.end());
		}
		if (body.empty())
		{
			return MakeError(ErrorCode::InvalidArgument, "no icon image has an .icns entry type (16, 32, 64, 128 or 256 px; {} given)", icons.size());
		}
		if (body.size() > std::numeric_limits<uint32_t>::max() - 8)
			return MakeError(ErrorCode::InvalidArgument, "the .icns file would be larger than 4 GiB");
		Buffer icns;
		icns.reserve(body.size() + 8);
		for (const char character : std::string_view("icns"))
			icns.push_back(static_cast<std::byte>(character));
		Utils::AppendBigEndianU32(icns, static_cast<uint32_t>(body.size() + 8));
		icns.insert(icns.end(), body.begin(), body.end());
		return icns;
	}

	Result<std::string> RewriteMoltenVkIcd(std::string_view sdkIcdJson)
	{
		ENGINE_TRY_ASSIGN(Json document, JsonReader::Parse(sdkIcdJson));
		const auto missing = [](std::string message)
		{
			ErrorLocation location;
			location.JsonPointer = "/ICD/library_path";
			return std::unexpected(Error(ErrorCode::Validation, std::move(message)).WithLocation(std::move(location)));
		};
		if (!document.is_object() || !document.contains("ICD") || !document["ICD"].is_object())
			return missing("the MoltenVK ICD manifest has no \"ICD\" object");
		Json& icd = document["ICD"];
		if (!icd.contains("library_path") || !icd["library_path"].is_string())
			return missing("the MoltenVK ICD manifest's ICD.library_path is missing or not a string");
		icd["library_path"] = std::string(AppBundleMoltenVkLibraryPath);
		return JsonWriter::Write(document);
	}

	Status CopyAppBundleBinaries(const std::filesystem::path& bundle, std::string_view name, const std::filesystem::path& runtime,
		const std::filesystem::path& runtimeOutput)
	{
		// Every input first, so a missing one leaves no half-assembled bundle behind it.
		std::error_code error;
		const std::filesystem::path frameworks = runtimeOutput / FileSystem::PathFromUtf8(Utils::FrameworksSourceDirectory);
		std::vector<std::filesystem::path> inputs = { runtime };
		for (const std::string_view framework : Utils::BundleFrameworks)
			inputs.push_back(frameworks / FileSystem::PathFromUtf8(framework));
		const std::filesystem::path icdSource = runtimeOutput / FileSystem::PathFromUtf8(Utils::IcdSourceDirectory) / FileSystem::PathFromUtf8(Utils::IcdManifestName);
		inputs.push_back(icdSource);
		for (const std::filesystem::path& input : inputs)
		{
			if (!std::filesystem::is_regular_file(input, error))
				return std::unexpected(Utils::MissingBundleInput(input));
		}

		for (const std::string_view directory : { AppBundleExecutableDirectory, AppBundleResourcesDirectory, AppBundleFrameworksDirectory })
			ENGINE_TRY(FileSystem::CreateDirectories(bundle / FileSystem::PathFromUtf8(directory)));
		const std::filesystem::path executable = bundle / FileSystem::PathFromUtf8(AppBundleExecutableDirectory) / FileSystem::PathFromUtf8(name);
		ENGINE_TRY(WithContext(Utils::CopyBundleFile(runtime, executable, std::filesystem::perms::owner_exec), "while copying the Runtime"));
		for (const std::string_view framework : Utils::BundleFrameworks)
		{
			ENGINE_TRY(WithContext(Utils::CopyBundleFile(frameworks / FileSystem::PathFromUtf8(framework),
									   bundle / FileSystem::PathFromUtf8(AppBundleFrameworksDirectory) / FileSystem::PathFromUtf8(framework)),
				std::format("while copying '{}'", framework)));
		}
		ENGINE_TRY_ASSIGN(const std::string icdText, FileSystem::ReadText(icdSource));
		ENGINE_TRY_ASSIGN(const std::string rewritten,
			WithContext(RewriteMoltenVkIcd(icdText), std::format("while rewriting '{}'", FileSystem::PathToUtf8(icdSource))));
		const std::filesystem::path icdTarget = bundle / FileSystem::PathFromUtf8(AppBundleIcdDirectory) / FileSystem::PathFromUtf8(Utils::IcdManifestName);
		ENGINE_TRY(FileSystem::CreateDirectories(icdTarget.parent_path()));
		ENGINE_TRY(FileSystem::WriteFileAtomic(icdTarget, AsBytes(rewritten), { .KeepBackup = false }));
		return {};
	}

	Status WriteAppBundleResources(const std::filesystem::path& bundle, const AppBundleInfo& info, std::span<const ExecutableIconImage> icons)
	{
		if (info.HasIcon && icons.empty())
			return MakeError(ErrorCode::InvalidArgument, "Info.plist names an icon (HasIcon), but no icon image was given");
		if (!info.HasIcon && !icons.empty())
			return MakeError(ErrorCode::InvalidArgument, "{} icon images were given, but Info.plist names no icon (HasIcon is false)", icons.size());
		ENGINE_TRY_ASSIGN(const std::string plist, WriteInfoPlist(info));
		const std::filesystem::path contents = bundle / FileSystem::PathFromUtf8(AppBundleContentsDirectory);
		ENGINE_TRY(FileSystem::CreateDirectories(contents));
		ENGINE_TRY(FileSystem::WriteFileAtomic(contents / "Info.plist", AsBytes(plist), { .KeepBackup = false }));
		if (!icons.empty())
		{
			ENGINE_TRY_ASSIGN(const Buffer icns, BuildIcns(icons));
			const std::filesystem::path resources = bundle / FileSystem::PathFromUtf8(AppBundleResourcesDirectory);
			ENGINE_TRY(FileSystem::CreateDirectories(resources));
			ENGINE_TRY(FileSystem::WriteFileAtomic(resources / FileSystem::PathFromUtf8(std::format("{}.icns", AppBundleIconName)), icns, { .KeepBackup = false }));
		}
		return {};
	}

}
