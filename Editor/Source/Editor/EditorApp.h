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
	// It hosts EditorCore (Docs/Decisions/0008-m4-decisions.md decision 14): the factory parses the editor options
	// (GetEditorCommandLineOptions, ParseEditorLaunchOptions); OnInitialize creates the
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
		~EditorApp() override;
	private:
		[[nodiscard]] Status OnInitialize() override;
		void OnShutdown() override;
		void OnSafePoint() override;
	private:
		// The launch options, the EditorContext, the AutomationServer and a batch or upgrade run (EditorApp.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

	// The editor's ApplicationFactory: parses the engine and editor options (GetEditorCommandLineOptions,
	// ParseEditorLaunchOptions), names the application ENGINE_PRODUCT_NAME (§4.4), registers the editor's automation types
	// and turns the headless throttle off for --batch and --upgrade (§4.2). Errors: InvalidArgument for a bad command line,
	// including any positional argument (the editor declares none) and the option combinations ParseEditorLaunchOptions
	// rejects.
	[[nodiscard]] Result<Scope<Application>> CreateEditorApp(std::span<const std::string> arguments);

}
