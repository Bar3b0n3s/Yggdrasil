#include "EnginePCH.h"
#include "Engine/Scripting/LuaHelpers.h"

#include "Engine/Scripting/Private/ScriptCall.h"

namespace Engine {

	Status ScriptCall::CheckWritable() const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script write checks are not implemented");
	}

	Status ScriptCall::PrepareHostMutation() const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script external mutation notification is not implemented");
	}

	namespace Lua {

		template<>
		bool Check<bool>(ScriptCall& /*call*/, int /*index*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		template<>
		int32_t Check<int32_t>(ScriptCall& /*call*/, int /*index*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		template<>
		uint32_t Check<uint32_t>(ScriptCall& /*call*/, int /*index*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		template<>
		float Check<float>(ScriptCall& /*call*/, int /*index*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		template<>
		double Check<double>(ScriptCall& /*call*/, int /*index*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		template<>
		std::string Check<std::string>(ScriptCall& /*call*/, int /*index*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		template<>
		glm::vec2 Check<glm::vec2>(ScriptCall& /*call*/, int /*index*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		template<>
		glm::vec3 Check<glm::vec3>(ScriptCall& /*call*/, int /*index*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		template<>
		glm::vec4 Check<glm::vec4>(ScriptCall& /*call*/, int /*index*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		template<>
		glm::quat Check<glm::quat>(ScriptCall& /*call*/, int /*index*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		template<>
		AssetHandle Check<AssetHandle>(ScriptCall& /*call*/, int /*index*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		template<>
		ScriptEntityIdentity Check<ScriptEntityIdentity>(ScriptCall& /*call*/, int /*index*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		template<>
		ScriptProxyIdentity Check<ScriptProxyIdentity>(ScriptCall& /*call*/, int /*index*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		template<>
		void Push<bool>(ScriptCall& /*call*/, const bool& /*value*/)
		{
			ENGINE_CONTRACT_STUB();
		}

		template<>
		void Push<int32_t>(ScriptCall& /*call*/, const int32_t& /*value*/)
		{
			ENGINE_CONTRACT_STUB();
		}

		template<>
		void Push<uint32_t>(ScriptCall& /*call*/, const uint32_t& /*value*/)
		{
			ENGINE_CONTRACT_STUB();
		}

		template<>
		void Push<float>(ScriptCall& /*call*/, const float& /*value*/)
		{
			ENGINE_CONTRACT_STUB();
		}

		template<>
		void Push<double>(ScriptCall& /*call*/, const double& /*value*/)
		{
			ENGINE_CONTRACT_STUB();
		}

		template<>
		void Push<std::string>(ScriptCall& /*call*/, const std::string& /*value*/)
		{
			ENGINE_CONTRACT_STUB();
		}

		template<>
		void Push<glm::vec2>(ScriptCall& /*call*/, const glm::vec2& /*value*/)
		{
			ENGINE_CONTRACT_STUB();
		}

		template<>
		void Push<glm::vec3>(ScriptCall& /*call*/, const glm::vec3& /*value*/)
		{
			ENGINE_CONTRACT_STUB();
		}

		template<>
		void Push<glm::vec4>(ScriptCall& /*call*/, const glm::vec4& /*value*/)
		{
			ENGINE_CONTRACT_STUB();
		}

		template<>
		void Push<glm::quat>(ScriptCall& /*call*/, const glm::quat& /*value*/)
		{
			ENGINE_CONTRACT_STUB();
		}

		template<>
		void Push<AssetHandle>(ScriptCall& /*call*/, const AssetHandle& /*value*/)
		{
			ENGINE_CONTRACT_STUB();
		}

		template<>
		void Push<ScriptEntityIdentity>(ScriptCall& /*call*/, const ScriptEntityIdentity& /*value*/)
		{
			ENGINE_CONTRACT_STUB();
		}

		template<>
		void Push<ScriptProxyIdentity>(ScriptCall& /*call*/, const ScriptProxyIdentity& /*value*/)
		{
			ENGINE_CONTRACT_STUB();
		}

		int GetArgumentCount(ScriptCall& /*call*/)
		{
			ENGINE_CONTRACT_STUB();
			return 0;
		}

		bool IsNoneOrNil(ScriptCall& /*call*/, int /*index*/)
		{
			ENGINE_CONTRACT_STUB();
			return false;
		}

		void PushNil(ScriptCall& /*call*/)
		{
			ENGINE_CONTRACT_STUB();
		}

		void PushString(ScriptCall& /*call*/, std::string_view /*value*/)
		{
			ENGINE_CONTRACT_STUB();
		}

		int64_t CheckEnum(ScriptCall& /*call*/, int /*index*/, std::string_view /*enumName*/)
		{
			ENGINE_CONTRACT_STUB();
			return 0;
		}

		Value CheckValue(ScriptCall& /*call*/, int /*index*/, const FieldInfo& /*field*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		void PushValue(ScriptCall& /*call*/, const Value& /*value*/, const FieldInfo& /*field*/)
		{
			ENGINE_CONTRACT_STUB();
		}

		int RaiseError(ScriptCall& /*call*/, std::string_view /*message*/)
		{
			ENGINE_CONTRACT_STUB();
			return 0;
		}

		int RaiseError(ScriptCall& /*call*/, const Error& /*error*/)
		{
			ENGINE_CONTRACT_STUB();
			return 0;
		}

		Result<ScriptCallResult> ProtectedCall(ScriptCall& /*call*/, int /*argumentCount*/, int /*resultCount*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Protected script calls are not implemented");
		}

		Result<ScriptCallResult> ProtectedCall(ScriptCall& /*call*/, ScriptNativeFunction /*function*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Protected native script calls are not implemented");
		}

		Result<ScriptCallResult> ProtectedResume(ScriptCall& /*call*/, lua_State* /*from*/, int /*argumentCount*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Protected script resumes are not implemented");
		}

	}

}
