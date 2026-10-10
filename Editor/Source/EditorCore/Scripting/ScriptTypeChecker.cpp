#include "EditorPCH.h"
#include "EditorCore/Scripting/ScriptTypeChecker.h"

namespace Engine {

	struct ScriptTypeChecker::State
	{
	};

	ScriptTypeChecker::ScriptTypeChecker(ConstructionKey /*key*/)
	{
		ENGINE_CONTRACT_STUB();
	}
	ScriptTypeChecker::~ScriptTypeChecker() = default;

	Result<ScriptTypeCheckerConfiguration> ScriptTypeChecker::CaptureConfiguration(const VirtualFileSystem& /*vfs*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ScriptTypeChecker configuration capture is an M13 contract stub");
	}

	Result<Scope<ScriptTypeChecker>> ScriptTypeChecker::Create(const ScriptApiRegistry& /*api*/,
		ScriptTypeCheckerConfiguration /*configuration*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ScriptTypeChecker is an M13 contract stub");
	}

	uint64_t ScriptTypeChecker::GetEnvironmentHash() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	std::vector<ScriptDiagnostic> ScriptTypeChecker::CheckScript(const ScriptCheckRequest& request)
	{
		ENGINE_CONTRACT_STUB();
		return { ScriptDiagnostic{
			.Severity = DiagnosticSeverity::Error,
			.Code = "SCRIPT_TYPE_ERROR",
			.File = std::string(request.Path.GetPath()),
			.Line = 1,
			.Column = 1,
			.Message = "Unsupported: ScriptTypeChecker is an M13 contract stub",
			.EndLine = 1,
			.EndColumn = 1,
		} };
	}

}
