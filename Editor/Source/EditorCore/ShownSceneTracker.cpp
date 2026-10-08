#include "EditorPCH.h"
#include "EditorCore/ShownSceneTracker.h"

#include "Engine/Core/EventLog.h"

#include <array>

namespace Engine {

	bool ShownSceneTracker::Update(const Scene* editScene, const Scene* sessionScene, const EventLog& events)
	{
		// One matching event is enough to know of a change; the cursor then skips to the log's end.
		constexpr std::array<EngineEventType, 2> SceneEvents = { EngineEventType::SceneOpened, EngineEventType::PlayStateChanged };
		const EventReadResult read = events.Read(m_EventCursor, SceneEvents, 1);
		m_EventCursor = events.GetNextSeq();
		const bool changed = editScene != m_EditScene || sessionScene != m_SessionScene || !read.Events.empty() || read.DroppedCount > 0;
		m_EditScene = editScene;
		m_SessionScene = sessionScene;
		return changed;
	}

}
