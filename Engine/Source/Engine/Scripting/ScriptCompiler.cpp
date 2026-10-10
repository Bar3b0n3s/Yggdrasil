#include "EnginePCH.h"
#include "Engine/Scripting/ScriptCompiler.h"

namespace Engine {

	Result<ScriptCompilation> ScriptCompiler::Compile(const ScriptCompileRequest& request)
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(request);
		return MakeError(ErrorCode::Unsupported, "ScriptCompiler is an M13 contract stub");
	}

}
