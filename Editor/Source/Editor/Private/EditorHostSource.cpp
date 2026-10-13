#include "EditorPCH.h"
#include "Editor/Private/EditorHostSource.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Utf8.h"

#include <array>
#include <optional>
#include <string_view>
#include <utility>

namespace Engine {

	namespace Utils {

		[[nodiscard]] static bool IsSourceWithin(const std::filesystem::path& file, const std::filesystem::path& root)
		{
			const auto relative = file.lexically_relative(root);
			return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
		}

		[[nodiscard]] static bool IsSourceSchemeCharacter(char character, bool first)
		{
			const bool letter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
			return letter || (!first && ((character >= '0' && character <= '9') || character == '+' || character == '.' || character == '-'));
		}

		// The URI scheme `text` starts with: a letter, then letters, digits, '+', '.' or '-', then ':'. Two characters at
		// least, so a drive letter ("C:") stays a native path; empty for any native path.
		[[nodiscard]] static std::string_view FindSourceScheme(std::string_view text)
		{
			size_t end = 0;
			while (end < text.size() && IsSourceSchemeCharacter(text[end], end == 0))
				++end;
			if (end < 2 || end == text.size() || text[end] != ':')
				return {};
			return text.substr(0, end);
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
		if (const std::string_view scheme = Utils::FindSourceScheme(text); !scheme.empty())
		{
			const size_t colon = scheme.size();
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

	EditorSourceLauncher::~EditorSourceLauncher()
	{
		// Shutdown stops supervising: editors still running are released, never killed with the user's unsaved work.
		for (Process& process : m_Running)
		{
			if (process.HasExited())
				continue;
			const Status detached = process.Detach();
			ENGINE_VERIFY(detached.has_value(), "external source editors start without captured output and can always be detached");
		}
	}

	Status EditorSourceLauncher::Queue(const EditorSourceRequest& request)
	{
		if (request.Preferences.ExternalEditorExecutable.empty())
		{
			return MakeError(ErrorCode::Unsupported,
				"No external source editor is configured. Set \"ExternalEditorExecutable\" (and optionally \"ExternalEditorArguments\", "
				"using {{path}} and {{line}}) in {}",
				request.PreferencesFile.empty() ? std::string("user://Editor.json") : FileSystem::PathToUtf8(request.PreferencesFile));
		}
		ENGINE_TRY_ASSIGN(const std::filesystem::path native, ResolveEditorSourcePath(request.Source, request.ProjectRoot, request.RepositoryRoot));
		ENGINE_TRY_ASSIGN(ProcessSpecification specification, BuildEditorSourceProcessSpecification(request.Preferences, native, request.Line));
		std::error_code error;
		const std::filesystem::file_status status = std::filesystem::status(specification.Executable, error);
		if (status.type() == std::filesystem::file_type::not_found || std::filesystem::is_directory(status))
			return MakeError(ErrorCode::NotFound, "The external source editor '{}' does not exist", request.Preferences.ExternalEditorExecutable);
		if (error)
			return MakeError(ErrorCode::Io, "Cannot inspect the external source editor '{}': {}", request.Preferences.ExternalEditorExecutable, error.message());
		specification.WorkingDirectory = request.ProjectRoot.empty() ? request.RepositoryRoot : request.ProjectRoot;
		specification.CaptureOutput = false;
		m_Queued.push_back(std::move(specification));
		return {};
	}

	Status EditorSourceLauncher::Pump()
	{
		std::erase_if(m_Running, [](Process& process)
		{
			return process.HasExited();
		});
		std::optional<Error> failure;
		size_t further = 0;
		for (const ProcessSpecification& specification : std::exchange(m_Queued, {}))
		{
			Result<Process> spawned = Process::Spawn(specification);
			if (spawned.has_value())
				m_Running.push_back(std::move(*spawned));
			else if (!failure.has_value())
				failure = std::move(spawned).error().WithContext("while starting the external source editor");
			else
				++further;
		}
		if (!failure.has_value())
			return {};
		if (further != 0)
			return std::unexpected(std::move(*failure).WithContext(std::format("{} more source editor launches failed", further)));
		return std::unexpected(std::move(*failure));
	}

	void EditorSourceLauncher::Cancel()
	{
		m_Queued.clear();
	}

}
