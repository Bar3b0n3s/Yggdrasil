#include "Runtime/RuntimeApp.h"

#include "Engine/App/CommandLine.h"

#include <utility>

namespace Engine {

	RuntimeApp::RuntimeApp(ApplicationSpecification specification)
		: Application(std::move(specification))
	{
	}

	Result<Scope<Application>> CreateRuntimeApp(std::span<const std::string> arguments)
	{
		ENGINE_TRY_ASSIGN(CommandLine commandLine, CommandLine::Parse(arguments, GetEngineCommandLineOptions()));
		if (!commandLine.GetPositional().empty())
		{
			return MakeError(ErrorCode::InvalidArgument, "unexpected argument '{}': the runtime takes no positional arguments",
				commandLine.GetPositional().front());
		}
		ApplicationSpecification specification;
		specification.Name = ENGINE_PRODUCT_NAME;
		ENGINE_TRY(ApplyEngineCommandLine(commandLine, specification));
		return CreateScope<RuntimeApp>(std::move(specification));
	}

}
