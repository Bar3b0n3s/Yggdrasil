#include "EnginePCH.h"
#include "Engine/Platform/CrashHandler.h"

// M2 contract stub (Roadmap rule 3): stream B (process, crash handler, paths, project lock) implements the
// host-independent parts: the breadcrumb and log-line buffers and the report text. Installing, the OS handlers and
// writing reports live in Platform/Windows/CrashHandlerWindows.cpp and Platform/Posix/CrashHandlerPosix.cpp.

namespace Engine {

	bool CrashHandler::IsInstalled()
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void CrashHandler::SetBreadcrumb(CrashBreadcrumb /*breadcrumb*/, std::string_view /*value*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	std::string_view CrashBreadcrumbToString(CrashBreadcrumb /*breadcrumb*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
