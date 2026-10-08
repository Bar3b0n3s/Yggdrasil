#include "Runtime/RuntimeApp.h"

#include "Engine/App/CommandLine.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/RuntimeAssetManager.h"
#include "Engine/Core/Log.h"

#include <utility>

namespace Engine {

	struct RuntimeApp::State
	{
		RuntimeOptions Options{};
		GameManifest Manifest{};
	};

	std::span<const CommandLineOption> GetRuntimeCommandLineOptions()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<RuntimeOptions> ParseRuntimeOptions(const CommandLine& /*commandLine*/, const std::filesystem::path& /*executablePath*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the Runtime's options are not implemented yet (M7 stream C)");
	}

	RuntimeApp::RuntimeApp(ApplicationSpecification specification, RuntimeOptions options, GameManifest manifest)
		: Application(std::move(specification)), m_State(CreateScope<State>())
	{
		m_State->Options = std::move(options);
		m_State->Manifest = std::move(manifest);
	}

	// OnShutdown released the manager while the engine context existed.
	RuntimeApp::~RuntimeApp() = default;

	Status RuntimeApp::OnInitialize()
	{
		// Contract stub: the M6 behaviour (the procedural built-ins only, no paks, no play session) keeps the existing Runtime
		// tests green until stream C replaces this body with the documented one.
		ENGINE_CONTRACT_STUB();
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
		// Contract stub: the M6 teardown until stream C releases the documented objects.
		ENGINE_CONTRACT_STUB();
		EngineContext& context = GetContext();
		if (context.GetAssetManager() == m_Assets.get())
			context.SetAssetManager(nullptr);
		// The manager finishes its loads on the context's job system and main-thread queue, so it goes before them.
		m_Assets.reset();
		m_Loaders.reset();
	}

	void RuntimeApp::OnEvent(Event& /*event*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RuntimeApp::OnSafePoint()
	{
		ENGINE_CONTRACT_STUB();
	}

	void RuntimeApp::OnFixedStep(const SimStep& /*step*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RuntimeApp::OnUpdate(const FrameTime& /*frame*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RuntimeApp::OnRender(RenderContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<Scope<Application>> CreateRuntimeApp(std::span<const std::string> arguments)
	{
		// Contract stub: the M6 factory (the engine options only and the product name) until stream C reads the manifest.
		ENGINE_CONTRACT_STUB();
		ENGINE_TRY_ASSIGN(CommandLine commandLine, CommandLine::Parse(arguments, GetEngineCommandLineOptions()));
		if (!commandLine.GetPositional().empty())
		{
			return MakeError(ErrorCode::InvalidArgument, "unexpected argument '{}': the runtime takes no positional arguments",
				commandLine.GetPositional().front());
		}
		ApplicationSpecification specification;
		specification.Name = ENGINE_PRODUCT_NAME;
		ENGINE_TRY(ApplyEngineCommandLine(commandLine, specification));
		return CreateScope<RuntimeApp>(std::move(specification), RuntimeOptions{}, GameManifest{});
	}

}
