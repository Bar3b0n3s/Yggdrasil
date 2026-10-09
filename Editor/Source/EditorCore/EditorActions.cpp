#include "EditorPCH.h"
#include "EditorCore/EditorActions.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/PendingOperation.h"

#include <map>
#include <utility>

namespace Engine {

	namespace Utils {

		class PanelUserAttribution
		{
		public:
			explicit PanelUserAttribution(EditorContext& editor)
				: m_Editor(editor)
			{
				if (editor.GetCommandOrigin() == CommandOrigin::Agent)
					m_Previous = editor.GetWriteAttribution();
				editor.SetWriteAttribution(std::nullopt);
			}
			~PanelUserAttribution() { m_Editor.SetWriteAttribution(std::move(m_Previous)); }
		private:
			EditorContext& m_Editor; // borrowed for this invocation
			std::optional<WriteAttribution> m_Previous{};
		};

		static Status CheckPanelAdmission(const EditorContext& editor, const MethodDescriptor& method, const RequestOptions& options)
		{
			if (!editor.HasProject() && !method.Specification.AvailableInLauncher)
				return MakeError(ErrorCode::InvalidState, "no project open; call project.create or project.open");
			if (method.Specification.Mutates && !options.DryRun && editor.IsReadOnly())
				return MakeError(ErrorCode::PermissionDenied, "the editor is read-only (--read-only)");
			if (options.IfRevision && *options.IfRevision != editor.GetRevision())
				return MakeError(ErrorCode::Conflict, "the editor revision changed; read the current state and retry");
			return {};
		}

	}

	struct EditorActions::State
	{
		struct Action
		{
			Scope<EditorMethodContext> Context{};
			Scope<PendingOperation> Pending{};
			std::optional<Result<Json>> Completion{};
		};
		EditorContext& Editor;    // owner-provided back-reference; outlives the queue
		AutomationServer& Server; // owner-provided back-reference; outlives every request
		std::map<uint64_t, Action> Actions{};
		uint64_t NextTicket = 1;
	};

	EditorActions::EditorActions(EditorContext& context, AutomationServer& server)
		: m_State(CreateScope<State>(context, server))
	{
	}

	EditorActions::~EditorActions()
	{
		Utils::PanelUserAttribution attribution(m_State->Editor);
		for (auto& [ticket, action] : m_State->Actions)
		{
			static_cast<void>(ticket);
			if (action.Pending)
				action.Pending->Cancel(*action.Context);
		}
	}

	Result<uint64_t> EditorActions::Submit(std::string_view method, const Json& params)
	{
		const MethodRegistry& registry = m_State->Server.GetMethods();
		const MethodDescriptor* descriptor = registry.Find(method);
		if (descriptor == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "unknown editor action '{}'", method);
		if (!m_State->Editor.HasProject() && !descriptor->Specification.AvailableInLauncher)
			return MakeError(ErrorCode::InvalidState, "no project open; call project.create or project.open");
		ENGINE_TRY_ASSIGN(PreparedParams prepared, registry.PrepareParams(*descriptor, params));
		ENGINE_TRY(Utils::CheckPanelAdmission(m_State->Editor, *descriptor, prepared.Options));
		const uint64_t ticket = m_State->NextTicket++;
		MethodRequest request{
			.Info = { .ClientName = "ui", .Id = ticket, .Method = std::string(method) },
			.Options = prepared.Options,
			.Method = descriptor,
			.Params = std::move(prepared.Params),
			.Registry = &registry,
			.PhaseMarker = &m_State->Server.GetWatchdog()
		};
		State::Action action;
		action.Context = CreateScope<EditorMethodContext>(m_State->Editor, m_State->Server, std::move(request));
		m_State->Actions.emplace(ticket, std::move(action));
		return ticket;
	}

	void EditorActions::Pump()
	{
		Utils::PanelUserAttribution attribution(m_State->Editor);
		const uint64_t end = m_State->NextTicket;
		for (auto& [ticket, action] : m_State->Actions)
		{
			if (ticket >= end || action.Completion)
				continue;
			if (action.Pending)
			{
				action.Completion = action.Pending->Poll(*action.Context);
				if (action.Completion)
					action.Pending.reset();
				continue;
			}
			Status admitted = Utils::CheckPanelAdmission(m_State->Editor, action.Context->GetMethod(), action.Context->GetOptions());
			if (!admitted)
			{
				action.Completion.emplace(std::unexpected(std::move(admitted).error()));
				continue;
			}
			Scope<EditorDryRunScope> dryRun;
			if (action.Context->IsDryRun())
			{
				auto scope = EditorDryRunScope::Begin(m_State->Editor);
				if (!scope)
				{
					action.Completion.emplace(std::unexpected(std::move(scope).error()));
					continue;
				}
				dryRun = std::move(*scope);
			}
			MethodResult result = m_State->Server.GetMethods().Invoke(*action.Context);
			if (auto* value = std::get_if<Json>(&result))
				action.Completion.emplace(std::move(*value));
			else if (auto* error = std::get_if<Error>(&result))
				action.Completion.emplace(std::unexpected(std::move(*error)));
			else
				action.Pending = std::move(std::get<Scope<PendingOperation>>(result));
		}
	}

	Result<std::optional<Result<Json>>> EditorActions::TakeResult(uint64_t ticket)
	{
		auto found = m_State->Actions.find(ticket);
		if (found == m_State->Actions.end())
			return MakeError(ErrorCode::NotFound, "unknown or consumed editor action {}", ticket);
		if (!found->second.Completion)
			return std::optional<Result<Json>>{};
		auto completion = std::move(found->second.Completion);
		m_State->Actions.erase(found);
		return completion;
	}

	Status EditorActions::Cancel(uint64_t ticket)
	{
		auto found = m_State->Actions.find(ticket);
		if (found == m_State->Actions.end())
			return MakeError(ErrorCode::NotFound, "unknown editor action {}", ticket);
		auto& action = found->second;
		if (action.Completion)
			return {};
		Utils::PanelUserAttribution attribution(m_State->Editor);
		if (action.Pending)
		{
			action.Pending->Cancel(*action.Context);
			action.Pending.reset();
		}
		action.Completion.emplace(MakeError(ErrorCode::Cancelled, "editor action cancelled"));
		return {};
	}

}
