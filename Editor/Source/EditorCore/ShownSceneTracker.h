#pragma once

#include "Engine/Core/Base.h"

#include <cstdint>

// Notices when the scene the editor shows changes, for the stale-mirror collections (Renderer/StaleMirrorSchedule.h;
// Docs/Decisions/0013-m8-decisions.md decision 7): a scene opened, closed or swapped, and play mode started or stopped. Each
// call compares the edit scene and the play session's scene with those of the previous call; the SceneOpened and
// PlayStateChanged events appended since then count as well, which catches a scene or a session replaced between two calls
// by one at the same address, and so does any event the log dropped before it was read. Pure; main thread only (the
// EventLog's).

namespace Engine {

	class EventLog;
	class Scene;

	class ShownSceneTracker
	{
	public:
		// Whether the shown scenes changed since the previous call: `editScene` is the editor's open scene and `sessionScene`
		// the play session's scene, each null when there is none. The pointers are compared, never dereferenced. The first
		// call compares with no scene and reads `events` from its oldest held event; every call reads up to its end.
		[[nodiscard]] bool Update(const Scene* editScene, const Scene* sessionScene, const EventLog& events);
	private:
		const Scene* m_EditScene = nullptr;    // compared, never dereferenced
		const Scene* m_SessionScene = nullptr; // compared, never dereferenced
		uint64_t m_EventCursor = 0;            // the Seq of the first event not read yet (0: the oldest held)
	};

}
