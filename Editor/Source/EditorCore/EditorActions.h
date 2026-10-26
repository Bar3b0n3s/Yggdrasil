#pragma once

#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace Engine {

	class AutomationServer;
	class EditorContext;

	// UI requests use the same reflected handlers/commands as automation, with CommandOrigin::User and provenance "ui".
	// Main-thread queue, owned by EditorLayer; context and server are back-references that outlive it. Never TCP.
	// A request is validated before enqueue; effects happen at the next safe point, outside ImGui traversal.
	// Pending methods retain their own MethodContext and are polled once per Pump; Cancel invokes their cancellation.
	// Agent pause/deny switches do not deny user actions. Read-only, launcher, revision, dry-run and param rules still hold,
	// and so does a test run's lease (EditorContext::CheckTestRunAdmission), at Submit and again when the action runs.
	// A request's scene target ("scene", M20: Docs/Decisions/0031-m20-contract.md decision 16; the scene tabs submit
	// scene.activate and scene.close with one) is resolved as the automation server resolves it
	// (EditorMethodContext::ResolveSceneTarget), after the project and test-run checks, and an EditorSceneTargetScope on it
	// encloses the read-only and revision checks, the dry-run sandbox and the handler, so they address that open scene: at
	// Submit, and again when the action runs, which refuses a target closed in between with NotFound.
	class EditorActions
	{
	public:
		EditorActions(EditorContext& context, AutomationServer& server);
		~EditorActions();
		EditorActions(const EditorActions&) = delete;
		EditorActions& operator=(const EditorActions&) = delete;
		// Returns a process-local monotonic ticket. InvalidArgument unknown method/params; normal host availability errors.
		[[nodiscard]] Result<uint64_t> Submit(std::string_view method, const Json& params);
		void Pump();
		// Moves out one completion; nullopt pending. NotFound unknown/already-consumed ticket.
		[[nodiscard]] Result<std::optional<Result<Json>>> TakeResult(uint64_t ticket);
		// NotFound unknown ticket; queued cancellation has no side effects, pending cancellation follows the handler.
		// The Cancelled completion stays until TakeResult consumes it.
		[[nodiscard]] Status Cancel(uint64_t ticket);
		// Cancel for a caller that abandons the ticket (a panel whose project changed): the entry, its method context and
		// any completion are released at once, and the ticket becomes unknown. NotFound unknown ticket.
		[[nodiscard]] Status Discard(uint64_t ticket);
	private:
		struct State;
		Scope<State> m_State;
	};

}
