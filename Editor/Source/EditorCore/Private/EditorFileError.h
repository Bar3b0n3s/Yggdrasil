#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Result.h"

// The one conversion of operating-system access failures on the editor's file paths (ADR 0008 decision 5), shared by
// EditorContext (project writes, provenance) and the automation methods (scene loads, docs, project.upgrade), so an agent
// sees one wording whichever step failed.

namespace Engine {

	namespace Utils {

		// The file system reports an operating-system access failure (EACCES, EPERM, a Windows sharing violation) as
		// PermissionDenied. The protocol reserves that code for its own refusals (bad tokens, read-only editors), so the
		// editor reports such a failure as Io: "<message> (the operating system denied access)", keeping the location,
		// contexts, hint and issues. Other errors are returned unchanged.
		[[nodiscard]] Error ToEditorFileError(Error error);

		// ToEditorFileError on a failed status; success unchanged.
		[[nodiscard]] Status ToEditorFileStatus(Status status);

	}

}
