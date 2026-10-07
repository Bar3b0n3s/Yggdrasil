#pragma once

#include "Engine/App/Application.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <span>
#include <string>

namespace Engine {

	class AssetLoaderRegistry;
	class RuntimeAssetManager;

	// The runtime application that runs exported games (Architecture §14.3). The runtime executable runs the frame loop with
	// a window, or headless with --headless, and with the Vulkan renderer Application's frame clears and presents every frame
	// (§8.2); --renderer none (not in Dist) renders nothing. OnInitialize builds the game's asset manager, a
	// RuntimeAssetManager with the built-in loaders on the context's services, and injects it into the engine context
	// (EngineContext::SetAssetManager, §3 rule 4); OnShutdown removes and destroys it while the context still exists. Until
	// M7 (the walking skeleton) reads the game manifest and adds its paks, and brings the play session, it serves the
	// procedural built-ins only.
	class RuntimeApp final : public Application
	{
	public:
		explicit RuntimeApp(ApplicationSpecification specification);
		~RuntimeApp() override;
	private:
		[[nodiscard]] Status OnInitialize() override;
		void OnShutdown() override;
	private:
		// From OnInitialize to OnShutdown; the manager uses the loaders and the context's services, so it goes first.
		Scope<AssetLoaderRegistry> m_Loaders;
		Scope<RuntimeAssetManager> m_Assets;
	};

	// The runtime's ApplicationFactory: parses the engine options and names the application ENGINE_PRODUCT_NAME until the
	// walking-skeleton milestone takes the name from the game manifest (§14.3; ADR 0005). Errors: InvalidArgument for a
	// bad command line, including any positional argument (the runtime declares none).
	[[nodiscard]] Result<Scope<Application>> CreateRuntimeApp(std::span<const std::string> arguments);

}
