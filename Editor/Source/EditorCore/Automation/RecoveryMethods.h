#pragma once

#include "EditorCore/Automation/ProjectMethods.h"
#include "Engine/Core/Result.h"

namespace Engine {

	// M10 branch of project.open, called only when recover=true. Main thread, validates a recovery before publishing the
	// opened editor state; on error releases the new lock and returns to launcher. No source Assets file is overwritten.
	// Errors: normal open errors; PermissionDenied read-only; Parse/Validation corrupt recovery; Conflict stale identity.
	// Unsupported when a newer recovery exists but the host has not supplied its long-lived Autosave service.
	// Without a newer recovery, open normally with recovered=false. No dry run, batch, Runtime or new tool.
	// Like project.open without recover, requires the launcher state. UI acceptance after recover=false goes directly
	// through the host-injected already-open service, which holds the original lock and calls Autosave::Recover.
	[[nodiscard]] Result<ProjectOpenResult> OpenProjectWithRecovery(EditorMethodContext& context, const ProjectOpenParams& params);

}
