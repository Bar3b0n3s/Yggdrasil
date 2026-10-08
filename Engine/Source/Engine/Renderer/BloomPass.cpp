#include "EnginePCH.h"
#include "Engine/Renderer/BloomPass.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <string_view>

// The contract stub (Docs/Decisions/0013-m8-decisions.md decision 3): the pipelines of the stub Bloom program (no bindings)
// for the format's permutation, so the program is covered by "Shaders: LayoutsMatchReflection" and the pipeline count
// matches the descriptions; Record records nothing and returns null. GetChainDesc is the frozen rule, implemented. Stream C
// replaces the rest.

namespace Engine {

	namespace Utils {

		static std::vector<PipelineLayoutDescription> MakeBloomStubDescriptions(nvrhi::Format format)
		{
			const std::string permutation = format == nvrhi::Format::RGBA16_FLOAT ? "1" : "0";
			std::vector<PipelineLayoutDescription> descriptions;
			for (const std::string_view entry : std::array<std::string_view, 3>{ "CSDownsampleKaris", "CSDownsample", "CSUpsample" })
			{
				descriptions.push_back({
					.Name = std::format("Bloom.{}", entry),
					.Program = "Bloom",
					.Entries = { std::string(entry) },
					.Permutation = { { .Key = "BLOOM_RGBA16", .Value = permutation } },
				});
			}
			return descriptions;
		}

	}

	struct BloomPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		nvrhi::Format Format = nvrhi::Format::R11G11B10_FLOAT;
		std::vector<ComputePipeline> Pipelines{};
	};

	BloomPass::BloomPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	BloomPass::~BloomPass() = default;

	Result<Scope<BloomPass>> BloomPass::Create(GraphicsDevice& device, PipelineFactory& pipelines, nvrhi::Format format)
	{
		ENGINE_CONTRACT_STUB();
		ENGINE_CORE_ASSERT(format == nvrhi::Format::R11G11B10_FLOAT || format == nvrhi::Format::RGBA16_FLOAT,
			"BloomPass needs R11G11B10_FLOAT or RGBA16_FLOAT");
		Scope<BloomPass> pass = CreateScope<BloomPass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		state.Format = format;
		for (PipelineLayoutDescription& description : Utils::MakeBloomStubDescriptions(format))
		{
			ENGINE_TRY_ASSIGN(ComputePipeline pipeline, pipelines.CreateComputePipeline({ .Layout = std::move(description) }));
			state.Pipelines.push_back(std::move(pipeline));
		}
		return pass;
	}

	std::vector<PipelineLayoutDescription> BloomPass::GetLayoutDescriptions(nvrhi::Format format)
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeBloomStubDescriptions(format);
	}

	nvrhi::TextureDesc BloomPass::GetChainDesc(uint32_t width, uint32_t height, nvrhi::Format format)
	{
		ENGINE_CORE_ASSERT(width > 0 && height > 0, "BloomPass::GetChainDesc needs a size of at least 1x1, got {}x{}", width, height);
		const uint32_t chainWidth = std::max(width / 2, 1U);
		const uint32_t chainHeight = std::max(height / 2, 1U);
		uint32_t fullMipCount = 1;
		for (uint32_t largest = std::max(chainWidth, chainHeight); largest > 1; largest /= 2)
			++fullMipCount;
		nvrhi::TextureDesc desc;
		desc.width = chainWidth;
		desc.height = chainHeight;
		desc.mipLevels = std::min(MaxMipCount, fullMipCount);
		desc.format = format;
		desc.dimension = nvrhi::TextureDimension::Texture2D;
		desc.isShaderResource = true;
		desc.isUAV = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.Bloom";
		return desc;
	}

	nvrhi::Format BloomPass::GetFormat() const
	{
		return m_State->Format;
	}

	uint32_t BloomPass::GetPipelineCount() const
	{
		return static_cast<uint32_t>(m_State->Pipelines.size());
	}

	Result<nvrhi::ITexture*> BloomPass::Record(nvrhi::ICommandList& /*commandList*/, PassBindingCache& /*bindings*/, const BloomPassInputs& /*inputs*/)
	{
		ENGINE_CONTRACT_STUB();
		// Nothing recorded: the caller composites no bloom.
		return static_cast<nvrhi::ITexture*>(nullptr);
	}

}
