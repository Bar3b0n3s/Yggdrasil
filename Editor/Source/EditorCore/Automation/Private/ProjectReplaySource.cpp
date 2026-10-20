#include "EditorPCH.h"
#include "EditorCore/Automation/Private/ProjectReplaySource.h"

#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <format>

namespace Engine {

	namespace Utils {

		namespace {

			// The replay-path rule of EditorMethodContext::LoadReplay: a project path below Assets/ with the .replay extension,
			// project-relative or project://. Errors located at /path.
			Result<VfsPath> ResolveReplayPath(std::string_view path)
			{
				constexpr std::string_view Hint = "give a project-relative .replay path below Assets/, such as \"Assets/Tests/Replays/Opening.replay\"";
				if (path.empty())
					return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, "/path", "a path must not be empty", std::string(Hint)));
				constexpr std::string_view SchemeSeparator = "://";
				Result<VfsPath> parsed = path.find(SchemeSeparator) != std::string_view::npos ? VfsPath::Parse(path) : VfsPath::Create("project", path);
				if (!parsed)
				{
					return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, "/path",
						std::format("'{}' is not a valid project path: {}", path, parsed.error().GetMessageText()), std::string(Hint)));
				}
				if (parsed->GetScheme() != "project")
				{
					return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, "/path",
						std::format("'{}' is outside the project: only project:// paths are accepted", path), std::string(Hint)));
				}
				ENGINE_TRY_ASSIGN(const VfsPath assets, VfsPath::Create("project", "Assets"));
				if (!parsed->IsUnder(assets) || *parsed == assets)
					return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, "/path", std::format("'{}' is not below the Assets folder", path), std::string(Hint)));
				if (parsed->GetExtension() != ".replay")
					return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, "/path", std::format("'{}' does not end with '.replay'", path), std::string(Hint)));
				return std::move(*parsed);
			}

		}

		Result<ReplayDocument> ReadProjectReplay(const EditorContext& editor, std::string_view path, ReplayLoadReport& report, std::string& canonicalPath)
		{
			if (!editor.HasProject())
				return MakeError(ErrorCode::InvalidState, "no project open; call project.create or project.open");
			ENGINE_TRY_ASSIGN(const VfsPath resolved, ResolveReplayPath(path));
			const std::string relative = ToProjectRelative(resolved);
			const VirtualFileSystem& vfs = editor.GetVfs();
			if (!vfs.Exists(resolved))
				return std::unexpected(MakeParamError(ErrorCode::NotFound, "/path", std::format("no replay '{}' exists", relative), "list the replays with asset.list {type: \"Replay\"}"));
			Result<std::string> text = vfs.ReadText(resolved);
			if (!text)
				return std::unexpected(std::move(text).error().WithLocation({ .File = relative, .JsonPointer = std::nullopt, .Entity = {} }));
			Result<ReplayDocument> document = ReplayFromText(*text, report, true);
			if (!document)
				return std::unexpected(std::move(document).error().WithLocation({ .File = relative, .JsonPointer = std::nullopt, .Entity = {} }));
			canonicalPath = relative;
			return document;
		}

	}

}
