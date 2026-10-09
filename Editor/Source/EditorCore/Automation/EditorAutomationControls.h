#pragma once

#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	class AutomationServer;
	class EditorContext;

	struct EditorAutomationPolicy
	{
		bool Paused = false;        // reject new agent work with Busy; already admitted operations finish/cancel normally
		bool DenyMutations = false; // all agent scene/project writes, including non-Mutates methods' optional fix/write paths
	};

	struct EditorRequestActivity
	{
		std::string Client{};
		std::string Method{};
		std::string RequestId{};
		double DurationMilliseconds = 0.0;
		bool Completed = false;
		bool Failed = false;
	};

	// CPU service shared by AutomationPanel and AutomationServer integration. Main thread. context/server outlive it.
	// Bounded latest 256 entries; result order newest first. Paused still admits session.info/hello/shutdown and discovery.
	// DenyMutations must be checked at editor Execute and file-write boundaries for Agent origin as well as RPC admission;
	// UI origin remains allowed. Dry runs remain allowed since they change no live state. Never cancel workers or sessions.
	class EditorAutomationControls
	{
	public:
		EditorAutomationControls(EditorContext& context, AutomationServer& server);
		~EditorAutomationControls();
		[[nodiscard]] EditorAutomationPolicy GetPolicy() const;
		void SetPolicy(const EditorAutomationPolicy& policy);
		[[nodiscard]] std::vector<EditorRequestActivity> GetRecentRequests() const;
		// Listener preference toggle: persist user://Editor.json before changing listener state; roll back on startup error.
		// Existing --automation explicit opt-in and one-shot exclusion take precedence. Errors: Io, InvalidState, bind errors.
		[[nodiscard]] Status SetAllowAiAutomation(bool allowed);
	};

}
