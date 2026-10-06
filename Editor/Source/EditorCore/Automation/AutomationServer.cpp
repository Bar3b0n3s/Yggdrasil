#include "EditorPCH.h"
#include "EditorCore/Automation/AutomationServer.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Automation/Protocol/ProtocolServer.h"

// M4 contract stub (Roadmap rule 3): stream B (protocol) implements the editor's automation server. Create fails fast, so
// the stub never opens a socket or writes a session file; the constructor and accessors are real.

namespace Engine {

	struct AutomationServer::State
	{
		std::string ServerTag{}; // MakeOffloadServerTag of the process id and start time
	};

	AutomationServer::AutomationServer(ConstructionKey /*key*/, EditorContext& editor, const AutomationServerSpecification& specification)
		: m_Editor(&editor), m_Specification(specification), m_Watchdog(specification.WatchdogStallThreshold, std::chrono::steady_clock::now()), m_Methods(editor.GetTypeRegistry()), m_State(CreateScope<State>())
	{
	}

	AutomationServer::~AutomationServer() = default;

	Result<Scope<AutomationServer>> AutomationServer::Create(EditorContext& /*editor*/, const AutomationServerSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AutomationServer::Create is an M4 contract stub");
	}

	void AutomationServer::Pump()
	{
		ENGINE_CONTRACT_STUB();
	}

	uint16_t AutomationServer::GetPort() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	std::vector<AutomationClientInfo> AutomationServer::GetClients() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	ClientId AutomationServer::ConnectInProcess(std::string /*name*/, bool /*offloadLargeResults*/)
	{
		ENGINE_CONTRACT_STUB();
		return NoClient;
	}

	void AutomationServer::DisconnectInProcess(ClientId /*client*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AutomationServer::SubmitInProcess(ClientId /*client*/, RpcRequest /*request*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	std::vector<Json> AutomationServer::TakeInProcessResponses(ClientId /*client*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status AutomationServer::CheckAvailability(const MethodDescriptor& /*method*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AutomationServer::CheckAvailability is an M4 contract stub");
	}

	Scope<MethodContext> AutomationServer::CreateContext(MethodRequest request)
	{
		ENGINE_CONTRACT_STUB();
		return CreateScope<EditorMethodContext>(*m_Editor, *this, std::move(request));
	}

	Status AutomationServer::AdmitRequest(MethodContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AutomationServer::AdmitRequest is an M4 contract stub");
	}

	void AutomationServer::EnterInvocation(MethodContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AutomationServer::LeaveInvocation(MethodContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void AutomationServer::FinishRequest(MethodContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	MetaState AutomationServer::GetMetaState() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::string AutomationServer::GetOffloadServerTag() const
	{
		ENGINE_CONTRACT_STUB();
		return m_State->ServerTag;
	}

	Result<std::string> AutomationServer::WriteOffloadedResult(std::string_view /*fileName*/, std::string_view /*text*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AutomationServer::WriteOffloadedResult is an M4 contract stub");
	}

}
