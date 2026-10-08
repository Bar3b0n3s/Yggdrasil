#include "EnginePCH.h"
#include "Engine/Renderer/BloomPass.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Shared/BloomConstants.h"

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Bloom program (Passes/Bloom.slang) has three entry points with one binding layout: the push constants
// (BloomConstants), t0 the level read, s0 LinearClamp and u0 the mip written. The three pipelines share that layout
// (PipelineFactory::CreateBindingLayouts), and Record binds one set per step, each naming the single mips it reads and
// writes, so NVRHI's per-subresource state tracking places the barriers between the steps. The chain is 2D, kept in
// ShaderResource between command lists: SceneColor -> mip 0 (Karis), mip i -> mip i + 1, then mip i + 1 -> mip i from the
// smallest mip up, the last step dividing by the mip count (BloomPass.h).

namespace Engine {

	namespace Utils {

		// The Bloom program's [numthreads(8, 8, 1)].
		constexpr uint32_t BloomGroupSize = 8;

		enum class BloomStep : uint8_t
		{
			DownsampleKaris,
			Downsample,
			Upsample
		};

		constexpr std::array<std::string_view, BloomPass::PipelineCount> BloomEntries = { "CSDownsampleKaris", "CSDownsample", "CSUpsample" };

		static PipelineLayoutDescription MakeBloomDescription(nvrhi::Format format, std::string_view entry)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {
				nvrhi::BindingLayoutItem::PushConstants(0, sizeof(BloomConstants)),
				nvrhi::BindingLayoutItem::Texture_SRV(0),
				nvrhi::BindingLayoutItem::Sampler(0),
				nvrhi::BindingLayoutItem::Texture_UAV(0),
			};
			return {
				.Name = std::format("Bloom.{}", entry),
				.Program = "Bloom",
				.Entries = { std::string(entry) },
				.Permutation = { { .Key = "BLOOM_RGBA16", .Value = format == nvrhi::Format::RGBA16_FLOAT ? "1" : "0" } },
				.BindingLayouts = { layout },
				.StorageImages = { { .Set = 0, .Register = 0, .Format = format } },
			};
		}

		static std::vector<PipelineLayoutDescription> MakeBloomDescriptions(nvrhi::Format format)
		{
			std::vector<PipelineLayoutDescription> descriptions;
			for (const std::string_view entry : BloomEntries)
				descriptions.push_back(MakeBloomDescription(format, entry));
			return descriptions;
		}

		static uint32_t GetMipExtent(uint32_t extent, uint32_t mip)
		{
			return std::max(extent >> mip, 1U);
		}

		static uint32_t GetGroupCount(uint32_t extent)
		{
			return (extent + BloomGroupSize - 1) / BloomGroupSize;
		}

	}

	struct BloomPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		nvrhi::Format Format = nvrhi::Format::R11G11B10_FLOAT;
		// Indexed by Utils::BloomStep; all three use BindingLayout.
		std::vector<ComputePipeline> Pipelines{};
		nvrhi::BindingLayoutHandle BindingLayout{};
		nvrhi::SamplerHandle LinearClamp{};
	};

	BloomPass::BloomPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	BloomPass::~BloomPass() = default;

	Result<Scope<BloomPass>> BloomPass::Create(GraphicsDevice& device, PipelineFactory& pipelines, nvrhi::Format format)
	{
		ENGINE_CORE_ASSERT(format == nvrhi::Format::R11G11B10_FLOAT || format == nvrhi::Format::RGBA16_FLOAT,
			"BloomPass needs R11G11B10_FLOAT or RGBA16_FLOAT");
		Scope<BloomPass> pass = CreateScope<BloomPass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		state.Format = format;
		std::vector<PipelineLayoutDescription> descriptions = Utils::MakeBloomDescriptions(format);
		ENGINE_TRY_ASSIGN(std::vector<nvrhi::BindingLayoutHandle> layouts, pipelines.CreateBindingLayouts(descriptions.front()));
		state.BindingLayout = layouts.front();
		for (PipelineLayoutDescription& description : descriptions)
		{
			ENGINE_TRY_ASSIGN(ComputePipeline pipeline,
				pipelines.CreateComputePipeline({ .Layout = std::move(description), .Specializations = {}, .SharedBindingLayouts = layouts }));
			state.Pipelines.push_back(std::move(pipeline));
		}
		nvrhi::SamplerDesc sampler;
		sampler.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		ENGINE_TRY_ASSIGN(state.LinearClamp, device.CreateSampler(sampler));
		return pass;
	}

	std::vector<PipelineLayoutDescription> BloomPass::GetLayoutDescriptions(nvrhi::Format format)
	{
		return Utils::MakeBloomDescriptions(format);
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

	Result<nvrhi::ITexture*> BloomPass::Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const BloomPassInputs& inputs)
	{
		ENGINE_CORE_ASSERT(inputs.SceneColor != nullptr && inputs.Chain != nullptr, "BloomPass::Record needs SceneColor and a chain");
		State& state = *m_State;
		const nvrhi::TextureDesc& scene = inputs.SceneColor->getDesc();
		const nvrhi::TextureDesc& chain = inputs.Chain->getDesc();
		[[maybe_unused]] const nvrhi::TextureDesc expected = GetChainDesc(scene.width, scene.height, state.Format);
		ENGINE_CORE_ASSERT(chain.width == expected.width && chain.height == expected.height && chain.mipLevels == expected.mipLevels
				&& chain.format == expected.format && chain.isUAV,
			"BloomPass::Record needs the chain of BloomPass::GetChainDesc for a {}x{} SceneColor", scene.width, scene.height);
		const uint32_t mipCount = chain.mipLevels;

		// One step: `source` (SceneColor, or mip `sourceMip` of the chain) into mip `destinationMip` of the chain.
		const auto recordStep = [&commandList, &bindings, &state, &inputs, &chain, mipCount](Utils::BloomStep step, nvrhi::ITexture* source,
									uint32_t sourceMip, uint32_t sourceWidth, uint32_t sourceHeight, uint32_t destinationMip) -> Status
		{
			const uint32_t width = Utils::GetMipExtent(chain.width, destinationMip);
			const uint32_t height = Utils::GetMipExtent(chain.height, destinationMip);
			BloomConstants constants;
			constants.SourceTexelSize = glm::vec2(1.0f / static_cast<float>(sourceWidth), 1.0f / static_cast<float>(sourceHeight));
			constants.DestinationTexelSize = glm::vec2(1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
			constants.DestinationSize = glm::uvec2(width, height);
			constants.Scale = step == Utils::BloomStep::Upsample && destinationMip == 0 ? 1.0f / static_cast<float>(mipCount) : 1.0f;

			const nvrhi::TextureSubresourceSet sourceSubresources =
				source == inputs.Chain ? nvrhi::TextureSubresourceSet(sourceMip, 1, 0, 1) : nvrhi::AllSubresources;
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::PushConstants(0, sizeof(BloomConstants)),
				nvrhi::BindingSetItem::Texture_SRV(0, source, nvrhi::Format::UNKNOWN, sourceSubresources),
				nvrhi::BindingSetItem::Sampler(0, state.LinearClamp),
				nvrhi::BindingSetItem::Texture_UAV(0, inputs.Chain, state.Format, nvrhi::TextureSubresourceSet(destinationMip, 1, 0, 1)),
			};
			ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, bindings.GetOrCreate(*state.Device, desc, *state.BindingLayout));
			nvrhi::ComputeState compute;
			compute.pipeline = state.Pipelines[std::to_underlying(step)].Pipeline;
			compute.bindings = { set };
			commandList.setComputeState(compute);
			commandList.setPushConstants(&constants, sizeof(constants));
			commandList.dispatch(Utils::GetGroupCount(width), Utils::GetGroupCount(height));
			return {};
		};

		commandList.beginMarker("Bloom");
		Status recorded = recordStep(Utils::BloomStep::DownsampleKaris, inputs.SceneColor, 0, scene.width, scene.height, 0);
		for (uint32_t mip = 1; mip < mipCount && recorded.has_value(); ++mip)
		{
			recorded = recordStep(Utils::BloomStep::Downsample, inputs.Chain, mip - 1, Utils::GetMipExtent(chain.width, mip - 1),
				Utils::GetMipExtent(chain.height, mip - 1), mip);
		}
		for (uint32_t mip = mipCount - 1; mip > 0 && recorded.has_value(); --mip)
		{
			recorded = recordStep(Utils::BloomStep::Upsample, inputs.Chain, mip, Utils::GetMipExtent(chain.width, mip),
				Utils::GetMipExtent(chain.height, mip), mip - 1);
		}
		commandList.endMarker();
		ENGINE_TRY(recorded);
		return inputs.Chain;
	}

}
