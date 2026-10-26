#pragma once

#include "Engine/Asset/ReplayData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <string>
#include <string_view>

// The editor's side of input.readReplay (Docs/Decisions/0030-m19-contract.md decision 14, A-RREAD): reading a project's
// source .replay, which EditorMethodContext::ReadReplayDocument forwards to. Its own file, stream B's, so the editor's
// method context (stream C's in W19.1) holds only the forwarding override.

namespace Engine {

	class EditorContext;

	namespace Utils {

		// The source replay at `path` of `editor`'s open project (project-relative or project://, the replay-path rule of
		// EditorMethodContext::LoadReplay), read strictly (ReplayFromText with strictUnknowns) and migrated to the current
		// version, with the file's own version in `report` and its canonical project-relative path in `canonicalPath`.
		// Errors, located at /path unless the reader located them in the file: InvalidState without an open project;
		// InvalidArgument for a path outside the project or without the .replay extension; NotFound for a missing file;
		// the reader's Parse, Validation and UnsupportedVersion with the file as their location's file.
		[[nodiscard]] Result<ReplayDocument> ReadProjectReplay(const EditorContext& editor, std::string_view path, ReplayLoadReport& report,
			std::string& canonicalPath);

	}

}
