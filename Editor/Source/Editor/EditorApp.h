#pragma once

#include "Engine/App/Application.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <span>
#include <string>

namespace Engine {

	// The editor application (Architecture §12). The editor executable runs the frame loop with a window, or headless with
	// --headless; its context's type registry holds the automation structs (RegisterEditorMethodTypes).
	//
	// M4 (stream B, Docs/Decisions/0008-m4-decisions.md decision 14) completes it as the host of EditorCore: the factory
	// parses the editor options (GetEditorCommandLineOptions, ParseEditorLaunchOptions); OnInitialize creates the
	// EditorContext, opens --project (exit 3 when it is locked or invalid), creates the AutomationServer and handles the
	// one-shot modes (--dump-reference writes the catalogues and exits; --batch and --upgrade run a BatchRunner); OnSafePoint
	// pumps the server, advances a batch and honours a shutdown request; batch runs are unthrottled (ThrottleHeadless off).
	// The server listens (AutomationServerSpecification::Listen) when EditorLaunchOptions::ListensForAutomation(headless):
	// with --automation, or headless unless the run is one-shot, so a one-shot run never opens a port or writes a session
	// file that an MCP editor_launch could attach to.
	class EditorApp final : public Application
	{
	public:
		explicit EditorApp(ApplicationSpecification specification);
	};

	// The editor's ApplicationFactory: parses the engine options (the editor adds its own with their milestones), names the
	// application ENGINE_PRODUCT_NAME (§4.4) and registers the editor's automation types. Errors: InvalidArgument for a bad
	// command line, including any positional argument (the editor declares none).
	[[nodiscard]] Result<Scope<Application>> CreateEditorApp(std::span<const std::string> arguments);

}
