#include "EditorPCH.h"
#include "Editor/EditorApp.h"

#include "Engine/App/CommandLine.h"

namespace Engine {

	EditorApp::EditorApp(ApplicationSpecification specification)
		: Application(std::move(specification))
	{
	}

	Result<Scope<Application>> CreateEditorApp(std::span<const std::string> arguments)
	{
		ENGINE_TRY_ASSIGN(CommandLine commandLine, CommandLine::Parse(arguments, GetEngineCommandLineOptions()));
		if (!commandLine.GetPositional().empty())
		{
			return MakeError(ErrorCode::InvalidArgument, "unexpected argument '{}': the editor takes no positional arguments",
				commandLine.GetPositional().front());
		}
		ApplicationSpecification specification;
		specification.Name = ENGINE_PRODUCT_NAME;
		ENGINE_TRY(ApplyEngineCommandLine(commandLine, specification));
		return CreateScope<EditorApp>(std::move(specification));
	}

}
