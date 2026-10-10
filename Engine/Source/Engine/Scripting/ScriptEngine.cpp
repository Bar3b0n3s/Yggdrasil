#include "EnginePCH.h"
#include "Engine/Scripting/ScriptEngine.h"

namespace Engine {

	struct ScriptEngine::State
	{
	};

	ScriptEngine::ScriptEngine(ConstructionKey /*key*/, const ScriptEngineSpecification& specification)
		: m_Specification(specification)
	{
		ENGINE_CONTRACT_STUB();
	}

	ScriptEngine::~ScriptEngine() = default;

	Result<Scope<ScriptEngine>> ScriptEngine::Create(const ScriptEngineSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ScriptEngine is an M13 contract stub");
	}

	Status ScriptEngine::InitializeInstances()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script instances are an M13 contract stub");
	}

	Status ScriptEngine::SynchronizeInstances()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script instances are an M13 contract stub");
	}

	Status ScriptEngine::NotifyCreated(std::span<const UUID> /*entities*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script creation is an M13 contract stub");
	}

	void ScriptEngine::StartPending()
	{
		ENGINE_CONTRACT_STUB();
	}

	void ScriptEngine::FixedUpdate()
	{
		ENGINE_CONTRACT_STUB();
	}

	void ScriptEngine::Update()
	{
		ENGINE_CONTRACT_STUB();
	}

	void ScriptEngine::LateUpdate()
	{
		ENGINE_CONTRACT_STUB();
	}

	void ScriptEngine::ResumeTasks()
	{
		ENGINE_CONTRACT_STUB();
	}

	void ScriptEngine::FinishDestroyFlush()
	{
		ENGINE_CONTRACT_STUB();
	}

	void ScriptEngine::Stop()
	{
		ENGINE_CONTRACT_STUB();
	}

	void ScriptEngine::EndSuite()
	{
		ENGINE_CONTRACT_STUB();
	}

	uint32_t ScriptEngine::PrepareDestroyFlush()
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	void ScriptEngine::OnPhysicsEvent(const PhysicsEvent& /*event*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ScriptEngine::OnPhysicsDiagnostic(const PhysicsDiagnostic& /*diagnostic*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<ScriptEvaluation> ScriptEngine::Evaluate(std::string_view /*source*/, const VfsPath& /*path*/, std::optional<UUID> /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script evaluation is an M13 contract stub");
	}

	Result<ScriptEvaluation> ScriptEngine::ExecuteBytecode(const ScriptData& /*script*/, std::optional<UUID> /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script execution is an M13 contract stub");
	}

	Result<ScriptReloadResult> ScriptEngine::Reload(AssetHandle /*script*/, bool /*fromTest*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script reload is an M13 contract stub");
	}

	Result<ScriptCollectedSuite> ScriptEngine::CollectSuite(AssetHandle /*script*/, IScriptTestHost& /*host*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Test suite collection is an M13 contract stub");
	}

	Result<ScriptReference> ScriptEngine::StartCase(ScriptReference /*caseFunction*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Test case creation is an M13 contract stub");
	}

	Result<ScriptCaseResume> ScriptEngine::ResumeCase(ScriptReference /*caseThread*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Test case resumption is an M13 contract stub");
	}

	void ScriptEngine::CancelCase(ScriptReference /*caseThread*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ScriptEngine::ReleaseReference(ScriptReference /*reference*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	IScriptHost& ScriptEngine::GetHost() const
	{
		ENGINE_CONTRACT_STUB();
		return *m_Specification.Host;
	}

	ScriptApiRegistry& ScriptEngine::GetApi() const
	{
		ENGINE_CONTRACT_STUB();
		return *m_Specification.Api;
	}

	const ScriptingSettings& ScriptEngine::GetSettings() const
	{
		ENGINE_CONTRACT_STUB();
		return m_Specification.Settings;
	}

	RunModes ScriptEngine::GetRunMode() const
	{
		ENGINE_CONTRACT_STUB();
		return m_Specification.Mode;
	}

	bool ScriptEngine::IsTestMode() const
	{
		ENGINE_CONTRACT_STUB();
		return m_Specification.TestMode;
	}

	bool ScriptEngine::IsReadOnly() const
	{
		ENGINE_CONTRACT_STUB();
		return m_Specification.ReadOnly;
	}

	bool ScriptEngine::IsStopped() const
	{
		ENGINE_CONTRACT_STUB();
		return true;
	}

	uint64_t ScriptEngine::GetGeneration() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	ScriptMemoryState ScriptEngine::GetMemoryState() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<ScriptInstanceInfo> ScriptEngine::GetInstances() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	ScriptErrorStream& ScriptEngine::GetErrors()
	{
		ENGINE_CONTRACT_STUB();
		return m_Errors;
	}

	const ScriptErrorStream& ScriptEngine::GetErrors() const
	{
		ENGINE_CONTRACT_STUB();
		return m_Errors;
	}

	TaskScheduler* ScriptEngine::GetTaskScheduler()
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	IScriptTestHost* ScriptEngine::GetTestHost() const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

}
