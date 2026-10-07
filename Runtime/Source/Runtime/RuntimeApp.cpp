#include "Runtime/RuntimeApp.h"

#include "Engine/App/CommandLine.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/RuntimeAssetManager.h"
#include "Engine/Core/Log.h"

#include <utility>

namespace Engine {

	RuntimeApp::RuntimeApp(ApplicationSpecification specification)
		: Application(std::move(specification))
	{
	}

	// OnShutdown released the manager while the engine context existed.
	RuntimeApp::~RuntimeApp() = default;

	Status RuntimeApp::OnInitialize()
	{
		EngineContext& context = GetContext();
		m_Loaders = CreateScope<AssetLoaderRegistry>();
		RegisterBuiltinLoaders(*m_Loaders);
		m_Assets = CreateScope<RuntimeAssetManager>(RuntimeAssetManagerSpecification{
			.Jobs = &context.GetJobSystem(),
			.MainThread = &context.GetMainThreadQueue(),
			.Registry = &context.GetTypeRegistry(),
			.Loaders = m_Loaders.get(),
		});
		context.SetAssetManager(m_Assets.get());
		ENGINE_INFO("Runtime asset manager: no paks, {} procedural built-ins", GetProceduralBuiltinEntries().size());
		return {};
	}

	void RuntimeApp::OnShutdown()
	{
		EngineContext& context = GetContext();
		if (context.GetAssetManager() == m_Assets.get())
			context.SetAssetManager(nullptr);
		// The manager finishes its loads on the context's job system and main-thread queue, so it goes before them.
		m_Assets.reset();
		m_Loaders.reset();
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
