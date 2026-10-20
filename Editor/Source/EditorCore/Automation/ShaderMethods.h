#pragma once

#include "Engine/Core/Result.h"

#include <cstdint>

// shaders.reload (M18, R-SHR, the editor's shader hot reload; Docs/Decisions/0029-m18-contract.md decision 3): reloads the
// compiled shaders of the editor's scene renderer pipelines without a restart, after CompileShaders.py or a build of the
// Shaders project rewrote them. The method compiles nothing and starts no process (§13.2): it reads the build output the
// editor's shaders:// mount holds, through SceneRendererPipelines::ReloadShaders, which builds every pipeline of the pass
// list aside and swaps them in only when all succeed. A broken shader (a layout that no longer matches its reflection, a
// variant the build did not write, a pipeline the driver refuses) fails the call and keeps the previous pipelines in use;
// fixing it and calling again swaps the new ones in. Every view of the editor (the scene and game viewports and the
// screenshot capture) shares the reloaded pipelines; ImGui's pipelines are not reloaded. The editor's "Reload Shaders"
// menu item runs the same action. Conventions as in MethodRegistry.h.
//
// The reload is the editor's: AutomationServerSpecification::ReloadShaders, which the editor injects when it has a
// GraphicsDevice; without it (--renderer none) the method is Unsupported. Not in the Runtime (exported games never reload
// shaders), not a tool (engine_call reaches it), not in the launcher state, no dry run, not a batch op, not flagged Mutates
// (it changes no project file and no scene).

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;
	struct NoParams;

	// What shaders.reload swapped in.
	struct ShadersReloadResult
	{
		// The scene renderer pipelines in use after the reload (SceneRendererPipelines::GetPipelineCount: the startup ones
		// plus the debug views' already created again).
		uint32_t PipelineCount = 0;
		// SceneRendererPipelines::GetGeneration after the reload (ToAutomationCounter): it increases with every reload that
		// succeeds.
		uint32_t Generation = 0;
	};

	namespace Automation {

		// shaders.reload {}. Errors: Unsupported without the host's reload (--renderer none); the reload's errors unchanged
		// (Validation, NotFound, Gpu, with the context "while reloading the shaders; the previous pipelines stay in use").
		[[nodiscard]] Result<ShadersReloadResult> ShadersReload(EditorMethodContext& context, const NoParams& params);

	}

	void RegisterShaderMethodTypes(TypeRegistry& registry);
	// Registers shaders.reload (see the file comment for its flags).
	void RegisterShaderMethods(MethodRegistry& methods);

}
