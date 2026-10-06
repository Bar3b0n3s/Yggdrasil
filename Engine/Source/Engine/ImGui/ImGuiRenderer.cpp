#include "EnginePCH.h"
#include "Engine/ImGui/ImGuiRenderer.h"

#include "Engine/Graphics/GraphicsDevice.h"

// M5 contract stub (Roadmap rule 3): stream D (ImGui renderer) implements the texture protocol, the per-format pipelines,
// the dynamic buffers and the draw recording of Architecture §8.11. Until then Create fails with Unsupported.

namespace Engine {

	ImGuiRenderer::ImGuiRenderer(ConstructionKey /*key*/)
	{
	}

	ImGuiRenderer::~ImGuiRenderer()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<Scope<ImGuiRenderer>> ImGuiRenderer::Create(GraphicsDevice& /*device*/, PipelineFactory& /*pipelines*/,
		const ImGuiRendererSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ImGuiRenderer::Create is not implemented yet");
	}

	void ImGuiRenderer::BeginFrame(uint32_t /*frameSlot*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Status ImGuiRenderer::RenderDrawData(nvrhi::ICommandList& /*commandList*/, nvrhi::IFramebuffer& /*framebuffer*/, ImDrawData& /*drawData*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ImGuiRenderer::RenderDrawData is not implemented yet");
	}

	ImTextureID ImGuiRenderer::AddTexture(nvrhi::ITexture& /*texture*/, uint32_t /*mipLevel*/, uint32_t /*arraySlice*/)
	{
		ENGINE_CONTRACT_STUB();
		return ImTextureID_Invalid;
	}

	void ImGuiRenderer::RemoveTexture(ImTextureID /*textureID*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ImGuiRenderer::DestroyTextures()
	{
		ENGINE_CONTRACT_STUB();
	}

	size_t ImGuiRenderer::GetTextureCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	PipelineLayoutDescription ImGuiRenderer::GetLayoutDescription()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
