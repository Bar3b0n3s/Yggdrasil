#include "EnginePCH.h"
#include "Engine/App/DisplaySettingsFile.h"

#include "Engine/App/Application.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Platform/Paths.h"

#include <nlohmann/json.hpp>

#include <format>
#include <span>

namespace Engine {

	namespace Utils {

		[[nodiscard]] static Result<uint32_t> ReadDisplayExtent(const JsonReader& root, std::string_view key, uint32_t minimum, uint32_t maximum)
		{
			ENGINE_TRY_ASSIGN(const JsonReader member, root.GetMember(key));
			ENGINE_TRY_ASSIGN(const uint32_t value, member.ReadUInt32());
			if (value < minimum || value > maximum)
				return std::unexpected(member.MakeLocatedError(ErrorCode::Validation, std::format("{} must be {} to {} (got {})", key, minimum, maximum, value)));
			return value;
		}

	}

	Result<DisplaySettings> ParseDisplaySettingsFile(std::string_view text)
	{
		ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
		const JsonReader root(document);
		ENGINE_TRY(root.ExpectType(JsonType::Object));
		ENGINE_TRY(root.ReadFormatHeader(DisplaySettingsFormatName, 1, DisplaySettingsFormatVersion));
		DisplaySettings settings;
		ENGINE_TRY_ASSIGN(const JsonReader modeMember, root.GetMember("WindowMode"));
		ENGINE_TRY_ASSIGN(const std::string modeName, modeMember.ReadString());
		const std::optional<DisplayWindowMode> mode = DisplayWindowModeFromString(modeName);
		if (!mode.has_value())
			return std::unexpected(modeMember.MakeLocatedError(ErrorCode::Validation, std::format("'{}' is not a window mode: Windowed, Borderless or Exclusive", modeName)));
		settings.WindowMode = *mode;
		ENGINE_TRY_ASSIGN(settings.Width, Utils::ReadDisplayExtent(root, "Width", 1, MaxDisplayExtent));
		ENGINE_TRY_ASSIGN(settings.Height, Utils::ReadDisplayExtent(root, "Height", 1, MaxDisplayExtent));
		ENGINE_TRY_ASSIGN(settings.RefreshRate, Utils::ReadDisplayExtent(root, "RefreshRate", 0, MaxDisplayRefreshRate));
		ENGINE_TRY_ASSIGN(settings.VSync, root.ReadMember<bool>("VSync"));
		return settings;
	}

	Result<std::string> SerializeDisplaySettingsFile(const DisplaySettings& settings)
	{
		ENGINE_TRY(ValidateDisplaySettings(settings));
		Json document = Json::object();
		document["Format"] = DisplaySettingsFormatName;
		document["Version"] = DisplaySettingsFormatVersion;
		document["WindowMode"] = DisplayWindowModeToString(settings.WindowMode);
		document["Width"] = settings.Width;
		document["Height"] = settings.Height;
		document["RefreshRate"] = settings.RefreshRate;
		document["VSync"] = settings.VSync;
		return JsonWriter::Write(document);
	}

	Status WriteDisplaySettingsFile(VirtualFileSystem& vfs, const DisplaySettings& settings)
	{
		if (!vfs.IsMounted("user"))
			return MakeError(ErrorCode::InvalidState, "display settings require a user:// mount");
		ENGINE_TRY_ASSIGN(const std::string text, SerializeDisplaySettingsFile(settings));
		ENGINE_TRY_ASSIGN(const VfsPath path, VfsPath::Parse(DisplaySettingsPath));
		return vfs.WriteFileAtomic(path, std::as_bytes(std::span(text.data(), text.size())));
	}

	std::optional<DisplaySettings> ReadStartupDisplaySettings(std::string_view appName, const std::filesystem::path& userDataRoot)
	{
		const Result<UserDataPaths> paths = Paths::GetUserDataPaths(appName, userDataRoot);
		if (!paths.has_value())
			return std::nullopt;
		Result<Scope<NativeDirectoryMount>> mount = NativeDirectoryMount::Create(paths->Root, MountAccess::ReadOnly);
		VirtualFileSystem vfs;
		if (!mount.has_value() || !vfs.Mount("user", std::move(*mount)).has_value())
			return std::nullopt;
		const Result<VfsPath> path = VfsPath::Parse(DisplaySettingsPath);
		if (!path.has_value() || !vfs.Exists(*path))
			return std::nullopt;
		const Result<std::string> text = vfs.ReadText(*path);
		if (!text.has_value())
		{
			ENGINE_CORE_WARN("The display settings in {} cannot be read, so the project's window settings apply: {}", DisplaySettingsPath, text.error().ToString());
			return std::nullopt;
		}
		Result<DisplaySettings> settings = ParseDisplaySettingsFile(*text);
		if (!settings.has_value())
		{
			ENGINE_CORE_WARN("The display settings in {} are invalid, so the project's window settings apply: {}", DisplaySettingsPath, settings.error().ToString());
			return std::nullopt;
		}
		return *settings;
	}

	void ApplyStartupDisplaySettings(ApplicationSpecification& specification, const DisplaySettings& settings)
	{
		WindowSpecification& window = specification.WindowSettings;
		window.DisplayMode = settings.WindowMode;
		window.RefreshRate = settings.WindowMode == DisplayWindowMode::Windowed ? 0 : settings.RefreshRate;
		window.Fullscreen = false;
		if (settings.WindowMode != DisplayWindowMode::Borderless)
		{
			window.Width = settings.Width;
			window.Height = settings.Height;
		}
		specification.Graphics.VSync = settings.VSync;
	}

}
