#pragma once

#include "Editor/FileDialog.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// A scripted native dialog backend for the FileDialog API's tests and the panels' (Docs/Decisions/0029-m18-contract.md
// decision 19: "an injectable backend for tests"). It never shows anything: its sessions answer as the script says.

namespace Engine::Test {

	// What a scripted backend saw, and how its sessions answer. Shared with the backend, so a test reads it after a panel
	// used the backend.
	struct ScriptedFileDialogScript
	{
		std::vector<FileDialogRequest> Requests{}; // every request the backend was offered, in order
		std::optional<Error> StartError{};         // set: the backend refuses to start a session (Unsupported, Io)
		uint32_t PendingPolls = 1;                 // the polls of each session that report Pending before its outcome
		Result<FileDialogOutcome> Outcome = FileDialogOutcome{ .State = FileDialogState::Cancelled, .Path = {} };
		uint32_t Cancels = 0;      // Cancel calls on its sessions
		uint32_t LiveSessions = 0; // sessions not yet destroyed
	};

	// One scripted dialog: Pending for PendingPolls polls, then the script's outcome; Cancelled once cancelled.
	class ScriptedFileDialogSession final : public IFileDialogSession
	{
	public:
		explicit ScriptedFileDialogSession(Ref<ScriptedFileDialogScript> script)
			: m_Script(std::move(script))
		{
			++m_Script->LiveSessions;
		}

		~ScriptedFileDialogSession() override { --m_Script->LiveSessions; }

		ScriptedFileDialogSession(const ScriptedFileDialogSession&) = delete;
		ScriptedFileDialogSession& operator=(const ScriptedFileDialogSession&) = delete;

		[[nodiscard]] Result<FileDialogOutcome> Poll() override
		{
			if (m_Cancelled)
				return FileDialogOutcome{ .State = FileDialogState::Cancelled, .Path = {} };
			if (m_Polls < m_Script->PendingPolls)
			{
				++m_Polls;
				return FileDialogOutcome{};
			}
			return m_Script->Outcome;
		}

		void Cancel() override
		{
			++m_Script->Cancels;
			m_Cancelled = true;
		}
	private:
		Ref<ScriptedFileDialogScript> m_Script;
		uint32_t m_Polls = 0;
		bool m_Cancelled = false;
	};

	// A FileDialogBackend that records each request in `script` and starts a ScriptedFileDialogSession, or refuses with the
	// script's StartError.
	[[nodiscard]] inline FileDialogBackend MakeScriptedFileDialogBackend(Ref<ScriptedFileDialogScript> script)
	{
		return [script = std::move(script)](const FileDialogRequest& request) -> Result<Scope<IFileDialogSession>>
		{
			script->Requests.push_back(request);
			if (script->StartError.has_value())
				return std::unexpected(*script->StartError);
			return Scope<IFileDialogSession>(CreateScope<ScriptedFileDialogSession>(script));
		};
	}

}
