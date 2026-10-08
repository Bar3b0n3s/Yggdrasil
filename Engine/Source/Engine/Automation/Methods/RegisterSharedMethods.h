#pragma once

#include "Engine/Core/Base.h"

#include <cstdint>

// The registration of the automation methods the Editor and the Runtime share (Architecture §3 "Automation/Methods",
// §13.5 "Runtime subset"; Docs/Decisions/0008-m4-decisions.md decision 26, Docs/Decisions/0012-m7-decisions.md decisions
// 12 and 13). The editor calls both functions from EditorCore's RegisterEditorMethodTypes and RegisterEditorMethods; the
// Runtime's automation server (RuntimeAutomationServer.h) calls them for its subset. Adding a shared method: its params,
// result and handler in its domain's header under Engine/Automation/Methods, typed on AutomationMethodContext, flagged
// AvailableInRuntime when §13.5 lists it, and its registration called from here (skill add-automation-method).

namespace Engine {

	class MethodRegistry;
	class TypeRegistry;

	// Which host registers: the editor registers every shared method; the Runtime only those of its subset (play.start and
	// play.stop are editor only, and so are the later milestones' editor-side members of shared domains).
	enum class AutomationHost : uint8_t
	{
		Editor,
		Runtime
	};

	// The types of every shared domain, in dependency order: input (RegisterInputMethodTypes), play
	// (RegisterPlayMethodTypes), viewport.screenshot (RegisterViewportScreenshotMethodTypes); stream C adds the domains it
	// moves from EditorCore (session, rpc, scene reads, entity reads, observe). RegisterAutomationSharedTypes must have run
	// on the registry first (the editor's RegisterAutomationCommonTypes runs it; the Runtime calls it itself).
	void RegisterSharedMethodTypes(TypeRegistry& registry);

	// The shared methods, in the order above, for `host` (RegisterPlayMethods with includeEditorMethods for the editor).
	void RegisterSharedMethods(MethodRegistry& methods, AutomationHost host);

}
