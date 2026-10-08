#include "EnginePCH.h"
#include "Engine/Automation/Methods/RuntimeAutomationServer.h"

#include "Engine/Core/Assert.h"

namespace Engine {

	struct RuntimeAutomationServer::State
	{
		Scope<MethodRegistry> Methods; // over the context's registry; filled and frozen by Create
	};

	RuntimeAutomationServer::RuntimeAutomationServer(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	RuntimeAutomationServer::~RuntimeAutomationServer() = default;

	Result<Scope<RuntimeAutomationServer>> RuntimeAutomationServer::Create(const TypeRegistry& /*registry*/, EventLog& /*events*/,
		VirtualFileSystem& /*vfs*/, PlaySession& /*session*/, const RuntimeAutomationServerSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the Runtime's automation server is not implemented yet (M7 stream C)");
	}

	void RuntimeAutomationServer::Pump()
	{
		ENGINE_CONTRACT_STUB();
	}

	uint16_t RuntimeAutomationServer::GetPort() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	const MethodRegistry& RuntimeAutomationServer::GetMethods() const
	{
		ENGINE_CONTRACT_STUB();
		ENGINE_CORE_ASSERT(m_State->Methods != nullptr, "RuntimeAutomationServer::GetMethods needs a server made by Create");
		return *m_State->Methods;
	}

	std::optional<int> RuntimeAutomationServer::GetShutdownRequest() const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	ClientId RuntimeAutomationServer::ConnectInProcess(std::string /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return NoClient;
	}

	void RuntimeAutomationServer::DisconnectInProcess(ClientId /*client*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RuntimeAutomationServer::SubmitInProcess(ClientId /*client*/, RpcRequest /*request*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	std::vector<Json> RuntimeAutomationServer::TakeInProcessResponses(ClientId /*client*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
