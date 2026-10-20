#include "EditorPCH.h"
#include "EditorCore/Automation/ShaderMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace Automation {

		Result<ShadersReloadResult> ShadersReload(EditorMethodContext& context, const NoParams& /*params*/)
		{
			const AutomationServerSpecification& specification = context.GetServer().GetSpecification();
			if (specification.RendererName == "none" || !specification.ReloadShaders)
			{
				Error error(ErrorCode::Unsupported, "This editor has no renderer whose shaders could be reloaded (--renderer none)");
				return std::unexpected(std::move(error).WithHint("start the editor with --renderer vulkan"));
			}
			// The host's reload is also the editor's Tools > Reload shaders; its errors already say that the previous pipelines
			// stay in use.
			return specification.ReloadShaders();
		}

	}

	void RegisterShaderMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<ShadersReloadResult>("ShadersReloadResult", "The scene renderer pipelines shaders.reload swapped in.")
			.Field("pipelineCount", &ShadersReloadResult::PipelineCount, "The scene renderer pipelines in use after the reload.")
			.Field("generation", &ShadersReloadResult::Generation,
				"The pipeline generation after the reload; it increases with every reload that succeeds.");
	}

	void RegisterShaderMethods(MethodRegistry& methods)
	{
		methods.Add(
			{
				.Name = "shaders.reload",
				.Description = "Reloads the editor's compiled shaders without a restart, after CompileShaders.py or a build of the Shaders "
							   "project rewrote them: every scene renderer pipeline is rebuilt and swapped in only when all succeed, so a "
							   "broken shader fails the call and keeps the previous pipelines. Compiles nothing; Unsupported without a "
							   "renderer.",
				.TimeoutSeconds = 120,
				.Examples = { { .Description = "Swap in shaders rebuilt by CompileShaders.py.", .Params = Json::object() } },
			},
			&Automation::ShadersReload);
	}

}
