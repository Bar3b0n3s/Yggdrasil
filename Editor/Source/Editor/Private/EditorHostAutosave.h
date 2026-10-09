#pragma once

#include "Engine/App/ProcessContext.h"

namespace Engine {

	class Autosave;

	// Borrowed fatal hook used by EditorApp. The host keeps saves alive until SetFatalErrorHook({}) has quiesced
	// outstanding invocations, then completes Reset before releasing the project or destroying the service.
	// The callback may run on any failing thread; it only claims previously published CPU bytes and never touches
	// live editor/ECS/VFS/GPU state, waits for another thread, or logs. Disk failure remains best effort.
	[[nodiscard]] FatalErrorHook MakeEditorAutosaveFatalHook(Autosave& saves);

}
