#include "EnginePCH.h"
#include "Engine/Automation/Protocol/MethodContext.h"

#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/Watchdog.h"
#include "Engine/Core/Assert.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <algorithm>
#include <array>

namespace Engine {

	namespace Utils {

		// The members of an error response's data that the protocol writes itself (JsonRpc.h MakeErrorResponse).
		constexpr std::array<std::string_view, 7> ReservedErrorDataMembers = { "errorCode", "detail", "hint", "contexts", "location",
			"issues", "_meta" };

		[[nodiscard]] static bool IsReservedErrorDataMember(std::string_view name)
		{
			return std::find(ReservedErrorDataMembers.begin(), ReservedErrorDataMembers.end(), name) != ReservedErrorDataMembers.end();
		}

	}

	MethodContext::MethodContext(TypeKey hostKey, MethodRequest request)
		: m_HostKey(hostKey), m_Request(std::move(request))
	{
		ENGINE_CORE_ASSERT(m_Request.Method != nullptr, "A method context needs its method descriptor");
		ENGINE_CORE_ASSERT(m_Request.Registry != nullptr, "A method context needs its method registry");
	}

	MethodContext::~MethodContext() = default;

	bool MethodContext::IsHostType(TypeKey key) const
	{
		return key == m_HostKey || key == TypeKeyOf<MethodContext>();
	}

	bool MethodContext::HasParam(std::string_view name) const
	{
		return m_Request.Params.is_object() && m_Request.Params.contains(name);
	}

	void MethodContext::SetPhase(std::string_view phase) const
	{
		if (m_Request.PhaseMarker != nullptr)
			m_Request.PhaseMarker->SetPhase(phase);
	}

	void MethodContext::SetErrorData(std::string_view name, Json value)
	{
		ENGINE_CORE_ASSERT(!Utils::IsReservedErrorDataMember(name), "SetErrorData: '{}' is a reserved member of an error's data", name);
		if (Utils::IsReservedErrorDataMember(name))
			return;
		if (!m_ErrorData.is_object())
			m_ErrorData = Json::object();
		m_ErrorData[std::string(name)] = std::move(value);
	}

	Result<Json> MethodContext::SerializeResultObject(TypeKey resultType, const void* result) const
	{
		const StructInfo* type = m_Request.Registry->GetTypes().FindStructByKey(resultType);
		ENGINE_CORE_ASSERT(type != nullptr, "The result of '{}' is not a registered struct", m_Request.Info.Method);
		if (type == nullptr)
			return MakeError(ErrorCode::Unknown, "the result of '{}' is not a registered struct", m_Request.Info.Method);
		return type->ToJson(result);
	}

}
