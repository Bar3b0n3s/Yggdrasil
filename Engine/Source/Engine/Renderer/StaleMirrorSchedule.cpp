#include "EnginePCH.h"
#include "Engine/Renderer/StaleMirrorSchedule.h"

namespace Engine {

	void StaleMirrorSchedule::NoteSceneChange()
	{
		m_IsReleasePending = true;
		m_HasRendered = false;
	}

	void StaleMirrorSchedule::NoteRendered()
	{
		if (m_IsReleasePending)
			m_HasRendered = true;
	}

	bool StaleMirrorSchedule::TakeReleaseUnused()
	{
		const bool release = m_IsReleasePending && m_HasRendered;
		if (release)
		{
			m_IsReleasePending = false;
			m_HasRendered = false;
		}
		return release;
	}

	bool StaleMirrorSchedule::IsReleasePending() const
	{
		return m_IsReleasePending;
	}

}
