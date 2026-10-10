#include "EnginePCH.h"
#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/Private/BindingRegistration.h"
#include "Engine/Scripting/Private/SandboxAccess.h"
#include "Engine/Scripting/Private/ScriptEngineState.h"

#include <algorithm>

namespace Engine {

	Result<ScriptCollectedSuite> ScriptEngine::CollectSuite(AssetHandle scriptHandle, IScriptTestHost& host)
	{
		m_State->AssertOwner();
		if (!IsTestMode() || IsStopped() || IsReadOnly() || m_State->SuiteActive
			|| (m_Specification.TestHost != nullptr && m_Specification.TestHost != &host))
			return MakeError(ErrorCode::InvalidState, "Suite collection needs a running test VM without another active suite");
		AssetManager* assets = GetHost().GetAssets();
		if (assets == nullptr)
			return MakeError(ErrorCode::NotFound, "The suite needs an asset manager");
		ENGINE_TRY_ASSIGN(auto asset, assets->Load(scriptHandle));
		const auto script = AssetCast<ScriptData>(asset);
		if (script == nullptr || script->Kind != ScriptKind::TestSuite)
			return MakeError(ErrorCode::Validation, "The script is not a TestSuite");
		m_State->SuiteActive = true;
		m_State->Collecting = true;
		m_State->TestHost = &host;
		m_State->Suite = {};
		m_State->SuiteTimeout = 0;
		std::optional<ScriptError> bodyFailure;
		const auto operation = [this, script, &bodyFailure](ScriptCall& call) -> int
		{
			const Status loaded = Detail::SandboxAccess::PushModule(call, *script);
			if (!loaded)
				return Lua::RaiseError(call, loaded.error());
			const Status body = Detail::PushSuiteBody(call, -1, m_State->Suite, m_State->SuiteTimeout);
			if (!body)
				return Lua::RaiseError(call, body.error());
			const auto result = Lua::ProtectedCall(call, 0, 0);
			if (!result)
				return Lua::RaiseError(call, result.error());
			bodyFailure = result->Failure;
			return 0;
		};
		const auto result = m_State->RunNative(operation, ScriptExecutionOrigin::TestDriver, {}, "Test.Suite");
		m_State->Collecting = false;
		if (result && result->Failure)
			bodyFailure = result->Failure;
		if (!result || bodyFailure)
		{
			if (bodyFailure)
				m_State->Publish(*bodyFailure);
			EndSuite();
			if (!result)
				return std::unexpected(result.error());
			return std::unexpected(Error(bodyFailure->Kind == ScriptErrorKind::Timeout ? ErrorCode::Timeout : ErrorCode::Script, bodyFailure->Message)
					.WithLocation({ bodyFailure->Script, bodyFailure->Line, bodyFailure->Column, std::nullopt, {} }));
		}
		return m_State->Suite;
	}

	Result<ScriptReference> ScriptEngine::StartCase(ScriptReference caseFunction)
	{
		m_State->AssertOwner();
		if (!IsTestMode() || !m_State->SuiteActive || !m_State->ActiveCase.IsNull() || IsStopped())
			return MakeError(ErrorCode::InvalidState, "A case requires an active suite and no other active case");
		const auto test = std::find_if(m_State->Suite.Cases.begin(), m_State->Suite.Cases.end(), [caseFunction](const ScriptTestCase& candidate)
		{
			return candidate.Function == caseFunction;
		});
		if (test == m_State->Suite.Cases.end())
			return MakeError(ErrorCode::InvalidArgument, "The function does not belong to this collected suite");
		ENGINE_TRY_ASSIGN(auto thread, Detail::ScriptEngineAccess::CreateThread(*this, caseFunction, {}, Detail::ScriptThreadKind::Case));
		m_State->References.find(thread.Index)->second.Owner.Case = thread;
		m_State->ActiveCase = thread;
		m_State->ObservedErrors.clear();
		return thread;
	}

	Result<ScriptCaseResume> ScriptEngine::ResumeCase(ScriptReference caseThread)
	{
		m_State->AssertOwner();
		if (caseThread != m_State->ActiveCase)
			return MakeError(ErrorCode::InvalidArgument, "The reference is not the active case");
		ENGINE_TRY(m_State->FindReference(caseThread, State::ReferenceKind::Case));
		return m_State->Resume(caseThread);
	}

	void ScriptEngine::CancelCase(ScriptReference caseThread)
	{
		m_State->AssertOwner();
		if (caseThread.IsNull() || caseThread.VMGeneration != GetGeneration())
			return;
		const auto found = m_State->References.find(caseThread.Index);
		if (found == m_State->References.end() || found->second.Kind != State::ReferenceKind::Case)
			return;
		m_State->Scheduler->CancelCase(caseThread);
		m_State->Release(caseThread);
		if (m_State->ActiveCase == caseThread)
		{
			m_State->ActiveCase = {};
			m_State->ObservedErrors.clear();
		}
	}

	void ScriptEngine::ReleaseReference(ScriptReference reference)
	{
		m_State->AssertOwner();
		if (reference == m_State->ActiveCase && !reference.IsNull())
			CancelCase(reference);
		else
		{
			const auto found = m_State->References.find(reference.Index);
			if (reference.VMGeneration == GetGeneration() && found != m_State->References.end() && found->second.Kind == State::ReferenceKind::Task)
			{
				const Status cancelled = m_State->Scheduler->Cancel(reference);
				if (!cancelled)
					m_State->Publish(cancelled.error());
			}
			else
				m_State->Release(reference);
		}
	}

	void ScriptEngine::EndSuite()
	{
		m_State->AssertOwner();
		if (!m_State->SuiteActive)
			return;
		CancelCase(m_State->ActiveCase);
		std::vector<ScriptReference> tasks;
		for (const auto& [index, reference] : m_State->References)
		{
			if (reference.Kind == State::ReferenceKind::Task && reference.Origin == ScriptExecutionOrigin::TestDriver)
				tasks.push_back({ GetGeneration(), index });
		}
		for (const auto task : tasks)
		{
			const Status cancelled = m_State->Scheduler->Cancel(task);
			if (!cancelled)
				m_State->Publish(cancelled.error());
		}
		for (const ScriptTestCase& test : m_State->Suite.Cases)
			m_State->Release(test.Function);
		m_State->Suite = {};
		m_State->SuiteActive = false;
		m_State->Collecting = false;
		m_State->ObservedErrors.clear();
		m_State->TestHost = m_Specification.TestHost;
	}

}
