#include "EditorPCH.h"
#include "EditorCore/Export/NativeExport.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/FileSystem.h"

#include <format>

namespace Engine {

	Status CheckNativeExport(const EditorContext& editor)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "exporting '{}', a game with a native module (Docs/Decisions/0030-m19-contract.md decision 33, stream N-A)",
			FileSystem::PathToUtf8(editor.GetProject().GetRoot()));
	}

	Result<Scope<INativeBuildJob>> StartNativeRuntimeBuild(EditorContext& /*editor*/, ExportConfiguration configuration, const std::filesystem::path& /*outputDirectory*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "building the {} Runtime with the native module (Docs/Decisions/0030-m19-contract.md decision 33, stream N-A)",
			ExportConfigurationToString(configuration));
	}

	Error MakeNativeBuildError(const NativeBuildOutcome& /*outcome*/, std::string_view name)
	{
		ENGINE_CONTRACT_STUB();
		return Error(ErrorCode::Unsupported, std::format("the native module build of '{}' (Docs/Decisions/0030-m19-contract.md decision 33, stream N-A)", name));
	}

}
