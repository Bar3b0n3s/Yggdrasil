#include "EnginePCH.h"
#include "Engine/ImGui/ImGuiLayer.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/ImGui/ImGuiRenderer.h"
#include "Engine/Platform/Window.h"

// M5 contract stub (Roadmap rule 3): stream D (ImGui renderer) implements the context, the GLFW backend, the deterministic
// frame delta and the ini handling of Architecture §8.11. Until then Create fails with Unsupported, so no ImGuiLayer
// exists; the accessors of stored state are implemented.

namespace Engine {

	ImGuiLayer::ImGuiLayer(ConstructionKey /*key*/)
	{
	}

	ImGuiLayer::~ImGuiLayer()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<Scope<ImGuiLayer>> ImGuiLayer::Create(Window& /*window*/, GraphicsDevice& /*device*/, PipelineFactory& /*pipelines*/,
		const ImGuiLayerSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ImGuiLayer::Create is not implemented yet");
	}

	void ImGuiLayer::BeginFrame(double /*deltaSeconds*/, uint32_t /*frameSlot*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ImGuiLayer::EndFrame()
	{
		ENGINE_CONTRACT_STUB();
	}

	Status ImGuiLayer::Render(nvrhi::ICommandList& /*commandList*/, nvrhi::IFramebuffer& /*framebuffer*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ImGuiLayer::Render is not implemented yet");
	}

	Status ImGuiLayer::SetIniFilePath(const std::filesystem::path& /*iniFilePath*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ImGuiLayer::SetIniFilePath is not implemented yet");
	}

	const std::filesystem::path& ImGuiLayer::GetIniFilePath() const
	{
		return m_IniFilePath;
	}

	ImGuiRenderer& ImGuiLayer::GetRenderer()
	{
		ENGINE_CORE_VERIFY(m_Renderer != nullptr, "ImGuiLayer::GetRenderer before the layer was created");
		return *m_Renderer;
	}

	ImDrawData* ImGuiLayer::GetDrawData() const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

}
