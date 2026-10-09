#include "EditorPCH.h"
#include "Editor/Private/EditorHostSource.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Utf8.h"

#include <array>
#include <string_view>

namespace Engine {

	namespace Utils {

		[[nodiscard]] static bool IsSourceWithin(const std::filesystem::path& file, const std::filesystem::path& root)
		{
			const auto relative = file.lexically_relative(root);
			return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
		}

	}

	Result<std::filesystem::path> ResolveEditorSourcePath(const std::filesystem::path& source,
		const std::filesystem::path& projectRoot, const std::filesystem::path& repositoryRoot)
	{
		std::string text = FileSystem::PathToUtf8(source);
		if (text.empty() || text.find('\0') != std::string::npos || !IsValidUtf8(text))
			return MakeError(ErrorCode::InvalidArgument, "Source location must be a nonempty UTF-8 path");
		// filesystem::path may collapse the two URI slashes; both spellings refer to the same VFS source.
		std::array roots{ projectRoot, repositoryRoot };
		std::filesystem::path requested = source;
		const size_t colon = text.find(':');
		if (colon != std::string::npos && colon != 1)
		{
			const std::string_view scheme(text.data(), colon);
			if (scheme != "project" && scheme != "engine")
				return MakeError(ErrorCode::InvalidArgument, "Unsupported source URI scheme '{}'", scheme);
			if (colon + 1 == text.size() || (text[colon + 1] != '/' && text[colon + 1] != '\\'))
				return MakeError(ErrorCode::InvalidArgument, "Source URI needs an absolute mount path");
			if (scheme == "project" && projectRoot.empty())
				return MakeError(ErrorCode::InvalidState, "Open a project before opening project source");
			const auto start = text.find_first_not_of("/\\", colon + 1);
			if (start == std::string::npos)
				return MakeError(ErrorCode::InvalidArgument, "Source URI names a directory");
			requested = FileSystem::PathFromUtf8(std::string_view(text).substr(start));
			if (requested.is_absolute() || requested.has_root_name())
				return MakeError(ErrorCode::PermissionDenied, "Source URI escapes its mount");
			roots = { scheme == "project" ? projectRoot : repositoryRoot / "Resources", std::filesystem::path{} };
		}
		bool outside = false;
		for (const auto& root : roots)
		{
			if (root.empty())
				continue;
			std::error_code error;
			const auto canonicalRoot = std::filesystem::canonical(root, error);
			if (error)
				return MakeError(ErrorCode::Io, "Cannot resolve source root: {}", error.message());
			const auto candidate = std::filesystem::canonical(requested.is_absolute() ? requested : canonicalRoot / requested, error);
			if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory)
				continue;
			if (error)
				return MakeError(ErrorCode::Io, "Cannot resolve source: {}", error.message());
			if (!Utils::IsSourceWithin(candidate, canonicalRoot))
			{
				outside = true;
				continue;
			}
			const bool regular = std::filesystem::is_regular_file(candidate, error);
			if (error)
				return MakeError(ErrorCode::Io, "Cannot inspect source: {}", error.message());
			if (!regular)
				return MakeError(ErrorCode::InvalidArgument, "Source location names a directory");
			return candidate;
		}
		if (outside)
			return MakeError(ErrorCode::PermissionDenied, "Source is outside its project or engine root");
		return MakeError(ErrorCode::NotFound, "Source file does not exist");
	}

}
