#include "EditorPCH.h"
#include "EditorCore/Export/ExecutableResources.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/Image.h"

#include <array>
#include <format>
#include <optional>

namespace Engine {

	namespace Utils {

		// The smallest icon side an exported executable accepts (§6.1 "Export": Icon is a PNG of at least 256 x 256).
		constexpr uint32_t MinimumIconSize = 256;

		[[nodiscard]] static Error MakeExportSettingError(std::string_view pointer, std::string message, std::string hint)
		{
			ErrorLocation location;
			location.JsonPointer = std::string(pointer);
			return Error(ErrorCode::Validation, std::move(message)).WithLocation(std::move(location)).WithHint(std::move(hint));
		}

		// One decimal part of a version: digits only, no sign, no leading zero (except "0" itself), at most 65535.
		[[nodiscard]] static std::optional<uint16_t> ParseVersionPart(std::string_view text)
		{
			if (text.empty() || text.size() > 5 || (text.size() > 1 && text.front() == '0'))
				return std::nullopt;
			uint32_t value = 0;
			for (const char character : text)
			{
				if (character < '0' || character > '9')
					return std::nullopt;
				value = value * 10 + static_cast<uint32_t>(character - '0');
			}
			if (value > 65535)
				return std::nullopt;
			return static_cast<uint16_t>(value);
		}

		// "major.minor.patch" as its three parts; nullopt for anything else.
		[[nodiscard]] static std::optional<std::array<uint16_t, 3>> ParseVersion(std::string_view version)
		{
			std::array<uint16_t, 3> parts{};
			size_t start = 0;
			for (size_t index = 0; index < parts.size(); ++index)
			{
				const size_t dot = version.find('.', start);
				const bool last = index + 1 == parts.size();
				if (last != (dot == std::string_view::npos))
					return std::nullopt;
				const std::optional<uint16_t> part = ParseVersionPart(version.substr(start, last ? std::string_view::npos : dot - start));
				if (!part.has_value())
					return std::nullopt;
				parts[index] = *part;
				start = dot + 1;
			}
			return parts;
		}

		// Checks that the project file `icon` decodes as a PNG of at least MinimumIconSize pixels on each side.
		[[nodiscard]] static Status CheckIcon(const EditorContext& editor, const std::string& icon)
		{
			constexpr std::string_view Pointer = "/Export/Icon";
			const Result<VfsPath> path = VfsPath::Create("project", icon);
			if (!path)
			{
				return std::unexpected(MakeExportSettingError(Pointer, std::format("Export.Icon '{}' is not a project-relative path: {}", icon, path.error().GetMessageText()), "name a PNG inside the project, such as \"Assets/Icon.png\""));
			}
			const Result<Buffer> bytes = editor.GetVfs().ReadFile(*path);
			if (!bytes)
			{
				return std::unexpected(MakeExportSettingError(Pointer, std::format("Export.Icon '{}' cannot be read: {}", icon, bytes.error().GetMessageText()),
					"add the icon file, or clear Export.Icon to use the engine's default icon"));
			}
			const Result<Image> image = DecodePng(std::span<const std::byte>(*bytes));
			if (!image)
			{
				return std::unexpected(MakeExportSettingError(Pointer, std::format("Export.Icon '{}' is not a PNG: {}", icon, image.error().GetMessageText()),
					"save the icon as a PNG of at least 256 x 256 pixels"));
			}
			if (image->Width < MinimumIconSize || image->Height < MinimumIconSize)
			{
				return std::unexpected(MakeExportSettingError(Pointer,
					std::format("Export.Icon '{}' is {} x {} pixels; an executable icon needs at least {} x {}", icon, image->Width, image->Height,
						MinimumIconSize, MinimumIconSize),
					"save a larger PNG; 256 x 256 pixels or more"));
			}
			return {};
		}

	}

	Result<ExecutableResources> ResolveExecutableResources(const EditorContext& editor, const ExportSettings& settings)
	{
		if (!editor.HasProject())
			return MakeError(ErrorCode::InvalidState, "no project open; the executable's resources come from the open project's Export settings");

		const std::optional<std::array<uint16_t, 3>> version = Utils::ParseVersion(settings.Version);
		if (!version.has_value())
		{
			return std::unexpected(Utils::MakeExportSettingError("/Export/Version",
				std::format("Export.Version '{}' is not \"major.minor.patch\" with three decimal numbers of at most 65535, without signs or leading zeros",
					settings.Version),
				"write a version such as \"1.0.0\" or \"2.3.14\""));
		}
		if (!settings.Icon.empty())
			ENGINE_TRY(Utils::CheckIcon(editor, settings.Icon));

		ExecutableResources resources;
		resources.Icon = settings.Icon;
		resources.VersionMajor = (*version)[0];
		resources.VersionMinor = (*version)[1];
		resources.VersionPatch = (*version)[2];
		resources.Company = settings.Company;
		return resources;
	}

}
