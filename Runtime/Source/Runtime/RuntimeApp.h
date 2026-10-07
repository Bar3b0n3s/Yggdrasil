#pragma once

#include "Engine/App/Application.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <span>
#include <string>

namespace Engine {

	// The runtime application that runs exported games (Architecture §14.3). For now an empty Application subclass: the
	// runtime executable runs the frame loop with a window, or headless with --headless, and with the Vulkan renderer
	// Application's frame clears and presents every frame (§8.2); --renderer none (not in Dist) renders nothing. The game
	// manifest, paks and the play session arrive with the walking-skeleton milestone.
	class RuntimeApp final : public Application
	{
	public:
		explicit RuntimeApp(ApplicationSpecification specification);
	};

	// The runtime's ApplicationFactory: parses the engine options and names the application ENGINE_PRODUCT_NAME until the
	// walking-skeleton milestone takes the name from the game manifest (§14.3; ADR 0005). Errors: InvalidArgument for a
	// bad command line, including any positional argument (the runtime declares none).
	[[nodiscard]] Result<Scope<Application>> CreateRuntimeApp(std::span<const std::string> arguments);

}
