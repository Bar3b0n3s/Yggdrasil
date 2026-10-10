#include "EnginePCH.h"
#include "Engine/Scripting/Sandbox.h"

#include "Engine/Scripting/Private/SandboxAccess.h"

namespace Engine {

	struct Sandbox::State
	{
	};

	Status InitializeScriptingRuntime()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Scripting runtime is an M13 contract stub");
	}

	void ShutdownScriptingRuntime()
	{
		ENGINE_CONTRACT_STUB();
	}

	Sandbox::Sandbox(ConstructionKey key)
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(key);
	}

	Sandbox::~Sandbox()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<Scope<Sandbox>> Sandbox::Create(const SandboxSpecification& specification)
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(specification);
		return MakeError(ErrorCode::Unsupported, "Sandbox is an M13 contract stub");
	}

	Status Sandbox::RunSource(const VfsPath& path, std::string_view source)
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(path);
		static_cast<void>(source);
		return MakeError(ErrorCode::Unsupported, "Sandbox is an M13 contract stub");
	}

	Status Sandbox::RunModule(const ScriptData& script)
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(script);
		return MakeError(ErrorCode::Unsupported, "Sandbox is an M13 contract stub");
	}

	Result<ScriptEvaluation> Sandbox::Evaluate(std::string_view source, const VfsPath& path, std::optional<UUID> entity)
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(source);
		static_cast<void>(path);
		static_cast<void>(entity);
		return MakeError(ErrorCode::Unsupported, "Sandbox is an M13 contract stub");
	}

	Result<ScriptEvaluation> Sandbox::ExecuteBytecode(const ScriptData& script, std::optional<UUID> entity)
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(script);
		static_cast<void>(entity);
		return MakeError(ErrorCode::Unsupported, "Sandbox is an M13 contract stub");
	}

	std::optional<ScriptError> Sandbox::GetLastError() const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	ScriptMemoryState Sandbox::GetMemoryState() const noexcept
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	bool Sandbox::IsStopped() const noexcept
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	namespace Detail {

		ScriptCall SandboxAccess::MainCall(Sandbox& sandbox)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			return {};
		}

		Result<ScriptCall> SandboxAccess::ThreadCall(Sandbox& sandbox, lua_State* state, std::string_view memberName)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			static_cast<void>(state);
			static_cast<void>(memberName);
			return MakeError(ErrorCode::Unsupported, "Sandbox bridge is an M13 contract stub");
		}

		ScriptWatchdog* SandboxAccess::GetWatchdog(Sandbox& sandbox)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			return nullptr;
		}

		TrackingAllocator* SandboxAccess::GetAllocator(Sandbox& sandbox)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			return nullptr;
		}

		RequireResolver* SandboxAccess::GetResolver(Sandbox& sandbox)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			return nullptr;
		}

		ScriptApiRegistry* SandboxAccess::GetApi(Sandbox& sandbox)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			return nullptr;
		}

		Random* SandboxAccess::GetRandom(Sandbox& sandbox)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			return nullptr;
		}

		uint32_t SandboxAccess::GetBudgetMs(const Sandbox& sandbox)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			return 0;
		}

		double SandboxAccess::SampleClock(Sandbox& sandbox)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			return 0.0;
		}

		SandboxMode SandboxAccess::GetMode(const Sandbox& sandbox)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			return SandboxMode::Runtime;
		}

		bool SandboxAccess::IsReadOnly(const Sandbox& sandbox)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			return false;
		}

		Status SandboxAccess::CheckWritable(const Sandbox& sandbox)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			return MakeError(ErrorCode::Unsupported, "Sandbox bridge is an M13 contract stub");
		}

		Status SandboxAccess::CheckRandomWritable(const Sandbox& sandbox)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			return MakeError(ErrorCode::Unsupported, "Sandbox bridge is an M13 contract stub");
		}

		Status SandboxAccess::EnterProtected(Sandbox& sandbox, ScriptExecutionOrigin origin)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			static_cast<void>(origin);
			return MakeError(ErrorCode::Unsupported, "Sandbox bridge is an M13 contract stub");
		}

		ScriptExecutionContext* SandboxAccess::GetExecution(Sandbox& sandbox)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			return nullptr;
		}

		ScriptExecutionOrigin SandboxAccess::GetExecutionOrigin(const Sandbox& sandbox)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			return ScriptExecutionOrigin::Pure;
		}

		void SandboxAccess::LeaveProtected(Sandbox& sandbox)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
		}

		Status SandboxAccess::PushContext(Sandbox& sandbox, const SandboxExecutionContext& context)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			static_cast<void>(context);
			return MakeError(ErrorCode::Unsupported, "Sandbox bridge is an M13 contract stub");
		}

		void SandboxAccess::PopContext(Sandbox& sandbox)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
		}

		SandboxExecutionContext SandboxAccess::GetContext(const Sandbox& sandbox)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			return {};
		}

		Result<std::string_view> SandboxAccess::RegisterLoadedChunk(Sandbox& sandbox, const ScriptData& script)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			static_cast<void>(script);
			return MakeError(ErrorCode::Unsupported, "Sandbox bridge is an M13 contract stub");
		}

		ErrorLocation SandboxAccess::Locate(const Sandbox& sandbox, std::string_view vmChunkName, uint32_t line, uint32_t column)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			static_cast<void>(vmChunkName);
			static_cast<void>(line);
			static_cast<void>(column);
			return {};
		}

		std::optional<ScriptErrorKind> SandboxAccess::CheckInterrupt(Sandbox& sandbox, int gc)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			static_cast<void>(gc);
			return std::nullopt;
		}

		std::optional<ScriptErrorKind> SandboxAccess::ClassifyFailure(const Sandbox& sandbox, int vmStatus)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			static_cast<void>(vmStatus);
			return std::nullopt;
		}

		void SandboxAccess::RecordFailure(Sandbox& sandbox, const ScriptError& error)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			static_cast<void>(error);
		}

		Status SandboxAccess::RecoverMemory(Sandbox& sandbox)
		{
			ENGINE_CONTRACT_STUB();
			static_cast<void>(sandbox);
			return MakeError(ErrorCode::Unsupported, "Sandbox bridge is an M13 contract stub");
		}

	}

}
