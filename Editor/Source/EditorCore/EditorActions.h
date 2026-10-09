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
	// Agent pause/deny switches do not deny user actions. Read-only, launcher, revision, dry-run and param rules still hold.
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
		[[nodiscard]] Status Cancel(uint64_t ticket);
	private:
		struct State;
		Scope<State> m_State;
	};

}
