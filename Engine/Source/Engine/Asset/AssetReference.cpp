#include "EnginePCH.h"
#include "Engine/Asset/AssetReference.h"

#include "Engine/Core/Assert.h"

#include <format>

namespace Engine {

	namespace Utils {

		static constexpr std::string_view ProjectScheme = "project";
		static constexpr std::string_view EnginePrefix = "engine://";
		static constexpr std::string_view AssetsFolder = "Assets";
		static constexpr std::string_view ReferenceHint = "write a 16-digit hex handle (\"3c9f2e7a11d04b88\"), a project path "
														  "(\"Assets/Materials/Red.material\"), a sub-asset path "
														  "(\"Assets/Models/Track.glb#mesh:0:Straight\") or an engine path "
														  "(\"engine://Meshes/Cube\")";

		static std::unexpected<Error> MakeReferenceError(std::string message)
		{
			return std::unexpected(Error(ErrorCode::InvalidArgument, std::move(message)).WithHint(std::string(ReferenceHint)));
		}

		static bool IsHexDigit(char character)
		{
			return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
		}

		static bool IsHandleText(std::string_view text)
		{
			if (text.size() != UUID::TextLength)
				return false;
			for (const char character : text)
			{
				if (!IsHexDigit(character))
					return false;
			}
			return true;
		}

		// The project path of `text`, which is "Assets/..." or "project://Assets/..." naming something below Assets/.
		static Result<VfsPath> ParseProjectPath(std::string_view text, std::string_view reference)
		{
			Result<VfsPath> path = text.find("://") != std::string_view::npos ? VfsPath::Parse(text) : VfsPath::Create(ProjectScheme, text);
			if (!path.has_value())
				return MakeReferenceError(std::format("'{}' is not a valid asset path: {}", reference, path.error().GetMessageText()));
			if (path->GetScheme() != ProjectScheme)
				return MakeReferenceError(std::format("'{}' is not an asset reference: the scheme '{}' holds no assets", reference, path->GetScheme()));
			const std::string_view relative = path->GetPath();
			if (relative.size() <= AssetsFolder.size() || !relative.starts_with(AssetsFolder) || relative[AssetsFolder.size()] != '/')
				return MakeReferenceError(std::format("'{}' is not below the project's Assets folder", reference));
			return path;
		}

		static Result<VfsPath> ParseEnginePath(std::string_view text, std::string_view reference)
		{
			Result<VfsPath> path = VfsPath::Parse(text);
			if (!path.has_value())
				return MakeReferenceError(std::format("'{}' is not a valid engine path: {}", reference, path.error().GetMessageText()));
			if (path->IsRoot())
				return MakeReferenceError(std::format("'{}' names no engine asset", reference));
			return path;
		}

	}

	Result<AssetReference> ParseAssetReference(std::string_view text)
	{
		if (text.empty())
			return Utils::MakeReferenceError("an empty asset reference names nothing");

		if (Utils::IsHandleText(text))
		{
			const std::optional<UUID> handle = UUID::FromString(text);
			if (!handle.has_value() || !handle->IsValid())
				return Utils::MakeReferenceError(std::format("'{}' is the null handle, which names no asset", text));
			return AssetReference{ .Kind = AssetReferenceKind::Handle, .Handle = *handle, .Path = {}, .SubAssetKey = {} };
		}

		// A sub-asset path splits at the first '#', so an asset file whose name contains '#' is referenced by handle only.
		const size_t hash = text.find('#');
		const std::string_view pathText = text.substr(0, hash);
		std::string key;
		if (hash != std::string_view::npos)
		{
			key = std::string(text.substr(hash + 1));
			if (key.empty())
				return Utils::MakeReferenceError(std::format("'{}' has an empty sub-asset key after '#'", text));
		}

		if (pathText.starts_with(Utils::EnginePrefix))
		{
			ENGINE_TRY_ASSIGN(VfsPath path, Utils::ParseEnginePath(pathText, text));
			return AssetReference{ .Kind = AssetReferenceKind::EnginePath, .Handle = {}, .Path = std::move(path), .SubAssetKey = std::move(key) };
		}

		ENGINE_TRY_ASSIGN(VfsPath path, Utils::ParseProjectPath(pathText, text));
		return AssetReference{ .Kind = AssetReferenceKind::ProjectPath, .Handle = {}, .Path = std::move(path), .SubAssetKey = std::move(key) };
	}

	std::string FormatAssetReference(const AssetReference& reference)
	{
		switch (reference.Kind)
		{
			case AssetReferenceKind::Handle:
				return reference.Handle.ToString();
			case AssetReferenceKind::ProjectPath:
			{
				std::string text(reference.Path.GetPath());
				if (!reference.SubAssetKey.empty())
					text += std::format("#{}", reference.SubAssetKey);
				return text;
			}
			case AssetReferenceKind::EnginePath:
			{
				std::string text = reference.Path.ToString();
				if (!reference.SubAssetKey.empty())
					text += std::format("#{}", reference.SubAssetKey);
				return text;
			}
		}
		ENGINE_CORE_ASSERT(false, "Unknown AssetReferenceKind {}", std::to_underlying(reference.Kind));
		return {};
	}

}
