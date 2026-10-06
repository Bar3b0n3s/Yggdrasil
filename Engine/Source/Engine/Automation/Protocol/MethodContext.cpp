#include "EnginePCH.h"
#include "Engine/Automation/Protocol/MethodContext.h"

// M4 contract stub (Roadmap rule 3): stream B (protocol) implements the request context. The constructor already stores
// the request and IsHostType is real, so contexts can be built and their accessors read while the other members are
// stubs.

namespace Engine {

	MethodContext::MethodContext(TypeKey hostKey, MethodRequest request)
		: m_HostKey(hostKey), m_Request(std::move(request))
	{
	}

	MethodContext::~MethodContext() = default;

	bool MethodContext::IsHostType(TypeKey key) const
	{
		return key == m_HostKey || key == TypeKeyOf<MethodContext>();
	}

	bool MethodContext::HasParam(std::string_view /*name*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void MethodContext::SetPhase(std::string_view /*phase*/) const
	{
		ENGINE_CONTRACT_STUB();
	}

	void MethodContext::SetErrorData(std::string_view /*name*/, Json /*value*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<Json> MethodContext::SerializeResultObject(TypeKey /*resultType*/, const void* /*result*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "MethodContext::SerializeResult is an M4 contract stub");
	}

}
