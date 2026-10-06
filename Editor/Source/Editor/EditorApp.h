#pragma once

#include "Engine/App/Application.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <span>
#include <string>

namespace Engine {

	// The editor application (Architecture §12). For now an empty Application subclass: the editor executable runs the
	// frame loop with a window, or headless with --headless; EditorCore, automation and the panels arrive with their
	// milestones.
	class EditorApp final : public Application
	{
	public:
		explicit EditorApp(ApplicationSpecification specification);
	};

	// The editor's ApplicationFactory: parses the engine options (the editor adds its own with their milestones) and
	// names the application ENGINE_PRODUCT_NAME (§4.4). Errors: InvalidArgument for a bad command line, including any
	// positional argument (the editor declares none yet).
	[[nodiscard]] Result<Scope<Application>> CreateEditorApp(std::span<const std::string> arguments);

}
