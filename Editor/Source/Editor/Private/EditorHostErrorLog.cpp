#include "EditorPCH.h"
#include "Editor/Private/EditorHostErrorLog.h"

#include "Engine/Core/Log.h"

#include <utility>

namespace Engine {

	bool EditorHostErrorLog::Report(std::string_view what, const Status& status, EditorHostErrorLevel level)
	{
		if (status)
		{
			m_LastError.clear();
			return false;
		}
		std::string error = status.error().ToString();
		if (error == m_LastError)
			return false;
		if (level == EditorHostErrorLevel::Warning)
			ENGINE_WARN("{}: {}", what, error);
		else
			ENGINE_ERROR("{}: {}", what, error);
		m_LastError = std::move(error);
		return true;
	}

}
