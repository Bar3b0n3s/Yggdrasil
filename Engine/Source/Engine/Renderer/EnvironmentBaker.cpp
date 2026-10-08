#include "EnginePCH.h"
#include "Engine/Renderer/EnvironmentBaker.h"

#include "Engine/Core/Error.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Shared/EnvironmentBakeConstants.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <initializer_list>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

// The five compute pipelines of Passes/EnvironmentBake.slang and the bake that records them (EnvironmentBaker.h). One bake
// records every step into one command list, executes it, waits (bounded) and reads the cubes and the SH9 totals back.

namespace Engine {

	namespace {

		// The pipelines in GetLayoutDescriptions' order.
		enum class BakeEntry : uint8_t
		{
			EquirectToCube,
			DownsampleCube,
			PrefilterSpecular,
			ProjectIrradiance,
			ReduceIrradiance
		};

		// What one bake writes, derived from its input and options.
		struct BakePlan
		{
			uint32_t SkyboxFaceSize = 0;
			uint32_t SkyboxMipCount = 0;
			uint32_t SpecularFaceSize = 0;
			uint32_t SpecularMipCount = 0;
			uint32_t SpecularSourceLevel = 0; // the skybox level specular mip 0 is resampled from
			uint32_t IrradianceLevel = 0;     // the skybox level the SH9 projection reads
			uint32_t MinSamples = 0;
			uint32_t MaxSamples = 0;
		};

		// One compute dispatch of a bake, recorded after every binding set was created.
		struct BakeDispatch
		{
			BakeEntry Entry = BakeEntry::EquirectToCube;
			nvrhi::IBindingSet* Set = nullptr; // held by the bake's set list, then by the command list
			EnvironmentBakeConstants Constants{};
			uint32_t GroupsX = 1;
			uint32_t GroupsY = 1;
			uint32_t GroupsZ = 1;
		};

	}

	namespace Utils {

		constexpr std::string_view BakeProgram = "EnvironmentBake";
		// The Slang program's [numthreads(8, 8, 1)] of the texel passes.
		constexpr uint32_t BakeGroupSize = 8;
		// CSProjectIrradiance's values per thread group: the nine SH9 coefficients, then the solid angle.
		constexpr uint32_t IrradianceTerms = 10;
		// The SH9 projection reads the first skybox level whose face is at most this size: L2 needs no finer detail.
		constexpr uint32_t IrradianceMaxFaceSize = 64;
		// The ranges of EnvironmentBakeOptions (EnvironmentBaker.h).
		constexpr uint32_t MinOptionFaceSize = 4;
		constexpr uint32_t MaxOptionSamples = 4096;
		// The widest equirectangular input, 8k: its RGBA32F upload is 512 MiB, and a 1024² skybox face, the largest, needs no more
		// than a 4k source (EnvironmentData::MaxSkyboxFaceSize).
		constexpr uint32_t MaxEquirectWidth = 8192;
		constexpr nvrhi::Format CubeFormat = nvrhi::Format::RGBA16_FLOAT;
		constexpr nvrhi::Format EquirectFormat = nvrhi::Format::RGBA32_FLOAT;
		constexpr uint32_t PushConstantsSlot = 0;
		// Rec. 709 luminance weights (§8.6 step 1: ClampLuminance).
		constexpr std::array<double, 3> LuminanceWeights = { 0.2126, 0.7152, 0.0722 };

		constexpr std::array<std::string_view, EnvironmentBaker::PipelineCount> BakeEntries = { "CSEquirectToCube", "CSDownsampleCube",
			"CSPrefilterSpecular", "CSProjectIrradiance", "CSReduceIrradiance" };

		[[nodiscard]] static size_t ToIndex(BakeEntry entry)
		{
			return static_cast<size_t>(entry);
		}

		[[nodiscard]] static uint32_t GetLevelSize(uint32_t faceSize, uint32_t level)
		{
			return std::max(faceSize >> level, 1U);
		}

		[[nodiscard]] static uint32_t GetGroupCount(uint32_t size)
		{
			return (size + BakeGroupSize - 1) / BakeGroupSize;
		}

		// The set-0 layout of one entry: `items` plus the bake's push constants.
		[[nodiscard]] static nvrhi::BindingLayoutDesc MakeBakeLayout(std::initializer_list<nvrhi::BindingLayoutItem> items)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = items;
			layout.bindings.push_back(nvrhi::BindingLayoutItem::PushConstants(PushConstantsSlot, sizeof(EnvironmentBakeConstants)));
			return layout;
		}

		[[nodiscard]] static PipelineLayoutDescription MakeBakeDescription(BakeEntry entry, nvrhi::BindingLayoutDesc layout, bool writesCube)
		{
			PipelineLayoutDescription description;
			description.Name = std::format("{}.{}", BakeProgram, BakeEntries[ToIndex(entry)]);
			description.Program = std::string(BakeProgram);
			description.Entries = { std::string(BakeEntries[ToIndex(entry)]) };
			description.BindingLayouts = { std::move(layout) };
			if (writesCube)
				description.StorageImages = { { .Set = 0, .Register = 0, .Format = CubeFormat } };
			return description;
		}

		[[nodiscard]] static bool IsPowerOfTwoInRange(uint32_t value, uint32_t minimum, uint32_t maximum)
		{
			return value >= minimum && value <= maximum && std::has_single_bit(value);
		}

		// InvalidArgument for an input EnvironmentBaker.h calls malformed.
		[[nodiscard]] static Status ValidateInput(const EnvironmentBakeInput& input)
		{
			if (input.Width == 0 || input.Height == 0 || input.Width != 2 * static_cast<uint64_t>(input.Height))
			{
				return MakeError(ErrorCode::InvalidArgument, "the equirectangular input is {}x{} texels; it must be non-empty and exactly twice as wide as high",
					input.Width, input.Height);
			}
			if (input.Width > MaxEquirectWidth)
				return MakeError(ErrorCode::InvalidArgument, "the equirectangular input is {} texels wide, more than the {} the bake accepts", input.Width, MaxEquirectWidth);
			const uint64_t expected = static_cast<uint64_t>(input.Width) * input.Height * 3;
			if (input.Texels.size() != expected)
			{
				return MakeError(ErrorCode::InvalidArgument, "the {}x{} equirectangular input holds {} values instead of {} (RGB per texel)", input.Width,
					input.Height, input.Texels.size(), expected);
			}
			const auto bad = std::ranges::find_if(input.Texels, [](float value)
			{
				return !std::isfinite(value) || value < 0.0f;
			});
			if (bad != input.Texels.end())
			{
				const size_t texel = static_cast<size_t>(bad - input.Texels.begin()) / 3;
				return MakeError(ErrorCode::InvalidArgument, "the equirectangular input has a non-finite or negative value ({}) at texel ({}, {})", *bad,
					texel % input.Width, texel / input.Width);
			}
			if (input.ClampLuminance && (!std::isfinite(input.ClampLuminanceMax) || !(input.ClampLuminanceMax > 0.0f)))
				return MakeError(ErrorCode::InvalidArgument, "ClampLuminance needs a positive, finite ClampLuminanceMax (got {})", input.ClampLuminanceMax);
			return {};
		}

		// InvalidArgument for options outside the ranges of EnvironmentBakeOptions.
		[[nodiscard]] static Status ValidateOptions(const EnvironmentBakeOptions& options)
		{
			if (options.SkyboxFaceSize != 0 && !IsPowerOfTwoInRange(options.SkyboxFaceSize, MinOptionFaceSize, EnvironmentData::MaxSkyboxFaceSize))
			{
				return MakeError(ErrorCode::InvalidArgument, "SkyboxFaceSize {} is neither 0 nor a power of two from {} to {}", options.SkyboxFaceSize,
					MinOptionFaceSize, EnvironmentData::MaxSkyboxFaceSize);
			}
			if (!IsPowerOfTwoInRange(options.SpecularFaceSize, MinOptionFaceSize, EnvironmentData::MaxSkyboxFaceSize))
			{
				return MakeError(ErrorCode::InvalidArgument, "SpecularFaceSize {} is not a power of two from {} to {}", options.SpecularFaceSize,
					MinOptionFaceSize, EnvironmentData::MaxSkyboxFaceSize);
			}
			const uint32_t fullChain = static_cast<uint32_t>(std::bit_width(options.SpecularFaceSize));
			if (options.SpecularMipCount < 2 || options.SpecularMipCount > fullChain)
			{
				return MakeError(ErrorCode::InvalidArgument, "SpecularMipCount {} is outside 2 to {} for a {}x{} specular cube", options.SpecularMipCount,
					fullChain, options.SpecularFaceSize, options.SpecularFaceSize);
			}
			if (options.MinSamples < 1 || options.MaxSamples > MaxOptionSamples || options.MinSamples > options.MaxSamples)
			{
				return MakeError(ErrorCode::InvalidArgument, "the sample counts {} to {} are outside 1 to {} or decreasing", options.MinSamples,
					options.MaxSamples, MaxOptionSamples);
			}
			return {};
		}

		[[nodiscard]] static BakePlan MakePlan(const EnvironmentBakeInput& input, const EnvironmentBakeOptions& options)
		{
			BakePlan plan;
			plan.SkyboxFaceSize = options.SkyboxFaceSize != 0 ? options.SkyboxFaceSize
															  : std::max(1U, std::min(EnvironmentData::MaxSkyboxFaceSize, input.Width / 4));
			plan.SkyboxMipCount = static_cast<uint32_t>(std::bit_width(plan.SkyboxFaceSize));
			plan.SpecularFaceSize = options.SpecularFaceSize;
			plan.SpecularMipCount = options.SpecularMipCount;
			// The smallest skybox level at least as large as the specular face, or level 0 for a smaller skybox (EnvironmentBaker.h).
			plan.SpecularSourceLevel = 0;
			while (plan.SpecularSourceLevel + 1 < plan.SkyboxMipCount
				&& GetLevelSize(plan.SkyboxFaceSize, plan.SpecularSourceLevel + 1) >= plan.SpecularFaceSize)
			{
				++plan.SpecularSourceLevel;
			}
			plan.IrradianceLevel = 0;
			while (GetLevelSize(plan.SkyboxFaceSize, plan.IrradianceLevel) > IrradianceMaxFaceSize)
				++plan.IrradianceLevel;
			plan.MinSamples = options.MinSamples;
			plan.MaxSamples = options.MaxSamples;
			return plan;
		}

		// The importance samples of specular mip `level` (>= 1): MinSamples at mip 1, rising linearly to MaxSamples at the last
		// mip (MaxSamples when mip 1 is the last).
		[[nodiscard]] static uint32_t GetSampleCount(const BakePlan& plan, uint32_t level)
		{
			if (plan.SpecularMipCount <= 2)
				return plan.MaxSamples;
			const uint64_t span = plan.MaxSamples - plan.MinSamples;
			return plan.MinSamples + static_cast<uint32_t>(span * (level - 1) / (plan.SpecularMipCount - 2));
		}

		// The input as RGBA32F texels (alpha 1), with ClampLuminance applied: a texel whose Rec. 709 luminance exceeds
		// ClampLuminanceMax is scaled down to it (§8.6 step 1).
		[[nodiscard]] static std::vector<float> MakeEquirectTexels(const EnvironmentBakeInput& input)
		{
			const size_t texelCount = static_cast<size_t>(input.Width) * input.Height;
			std::vector<float> rgba(texelCount * 4);
			for (size_t texel = 0; texel < texelCount; ++texel)
			{
				double red = input.Texels[texel * 3];
				double green = input.Texels[texel * 3 + 1];
				double blue = input.Texels[texel * 3 + 2];
				if (input.ClampLuminance)
				{
					const double luminance = LuminanceWeights[0] * red + LuminanceWeights[1] * green + LuminanceWeights[2] * blue;
					if (luminance > input.ClampLuminanceMax)
					{
						const double scale = input.ClampLuminanceMax / luminance;
						red *= scale;
						green *= scale;
						blue *= scale;
					}
				}
				rgba[texel * 4] = static_cast<float>(red);
				rgba[texel * 4 + 1] = static_cast<float>(green);
				rgba[texel * 4 + 2] = static_cast<float>(blue);
				rgba[texel * 4 + 3] = 1.0f;
			}
			return rgba;
		}

		[[nodiscard]] static nvrhi::TextureDesc MakeCubeDesc(uint32_t faceSize, uint32_t mipCount, const char* name)
		{
			nvrhi::TextureDesc desc;
			desc.width = faceSize;
			desc.height = faceSize;
			desc.arraySize = CubeMapData::FaceCount;
			desc.mipLevels = mipCount;
			desc.dimension = nvrhi::TextureDimension::TextureCube;
			desc.format = CubeFormat;
			desc.isShaderResource = true;
			desc.isUAV = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = name;
			return desc;
		}

		[[nodiscard]] static nvrhi::BufferDesc MakeTermsBufferDesc(uint64_t termCount, const char* name)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = termCount * sizeof(glm::vec4);
			desc.structStride = sizeof(glm::vec4);
			desc.canHaveUAVs = true;
			desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
			desc.keepInitialState = true;
			desc.debugName = name;
			return desc;
		}

		// One level of a cube as six layers, for the bake's Texture2DArray bindings.
		[[nodiscard]] static nvrhi::TextureSubresourceSet GetLevelFaces(uint32_t level)
		{
			return nvrhi::TextureSubresourceSet(level, 1, 0, CubeMapData::FaceCount);
		}

		// Every mip and face of `cube` read back into CubeMapData's layout (level by level, faces in order, rows top first).
		[[nodiscard]] static Result<CubeMapData> ReadCube(Readback& readback, nvrhi::ITexture& cube, uint32_t faceSize, uint32_t mipCount)
		{
			CubeMapData data;
			data.FaceSize = faceSize;
			data.MipCount = mipCount;
			data.Texels.reserve(ComputeCubeMapByteSize(faceSize, mipCount));
			for (uint32_t level = 0; level < mipCount; ++level)
			{
				const size_t levelBytes = static_cast<size_t>(GetLevelSize(faceSize, level)) * GetLevelSize(faceSize, level) * CubeMapData::BytesPerTexel;
				for (uint32_t face = 0; face < CubeMapData::FaceCount; ++face)
				{
					ENGINE_TRY_ASSIGN(const Image image, readback.ReadTexture(cube, level, face));
					if (image.Pixels.size() != levelBytes)
					{
						return MakeError(ErrorCode::Gpu, "the readback of mip {}, face {} of '{}' holds {} bytes instead of {}", level, face,
							cube.getDesc().debugName, image.Pixels.size(), levelBytes);
					}
					data.Texels.insert(data.Texels.end(), image.Pixels.begin(), image.Pixels.end());
				}
			}
			return data;
		}

		// The SH9 coefficients of irradiance / pi (EnvironmentData.h) from the projected radiance totals: each normalized by the
		// solid angles (4 pi over their sum, which removes the projection's rounding), convolved with the clamped cosine (A_l /
		// pi = 1, 2/3, 1/4) and windowed with the Hanning window of EnvironmentData::IrradianceShWindow.
		[[nodiscard]] static std::array<glm::vec3, 9> MakeIrradianceSH9(std::span<const glm::vec4, IrradianceTerms> totals)
		{
			constexpr double Pi = std::numbers::pi;
			constexpr std::array<double, 3> CosineLobe = { 1.0, 2.0 / 3.0, 0.25 };
			const double solidAngle = totals[IrradianceTerms - 1].x;
			const double normalization = solidAngle > 0.0 ? 4.0 * Pi / solidAngle : 0.0;
			std::array<glm::vec3, 9> coefficients{};
			for (size_t index = 0; index < coefficients.size(); ++index)
			{
				const size_t band = index == 0 ? 0 : (index < 4 ? 1 : 2);
				const double window = (1.0 + std::cos(Pi * static_cast<double>(band) / EnvironmentData::IrradianceShWindow)) * 0.5;
				const double factor = normalization * CosineLobe[band] * window;
				coefficients[index] = glm::vec3(glm::dvec3(totals[index]) * factor);
			}
			return coefficients;
		}

	}

	struct EnvironmentBaker::State
	{
		GraphicsDevice* Device = nullptr;         // documented back-reference
		std::vector<ComputePipeline> Pipelines{}; // in BakeEntry order
		nvrhi::SamplerHandle LinearClamp{};       // trilinear, for the prefilter's cube reads
	};

	EnvironmentBaker::EnvironmentBaker(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	EnvironmentBaker::~EnvironmentBaker() = default;

	Result<Scope<EnvironmentBaker>> EnvironmentBaker::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<EnvironmentBaker> baker = CreateScope<EnvironmentBaker>(ConstructionKey());
		State& state = *baker->m_State;
		state.Device = &device;
		for (PipelineLayoutDescription& description : GetLayoutDescriptions())
		{
			ENGINE_TRY_ASSIGN(ComputePipeline pipeline, pipelines.CreateComputePipeline({ .Layout = std::move(description), .Specializations = {}, .SharedBindingLayouts = {} }));
			state.Pipelines.push_back(std::move(pipeline));
		}
		nvrhi::SamplerDesc sampler;
		sampler.setAllFilters(true);
		sampler.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		ENGINE_TRY_ASSIGN(state.LinearClamp, device.CreateSampler(sampler));
		return baker;
	}

	std::vector<PipelineLayoutDescription> EnvironmentBaker::GetLayoutDescriptions()
	{
		using nvrhi::BindingLayoutItem;
		std::vector<PipelineLayoutDescription> descriptions;
		descriptions.push_back(Utils::MakeBakeDescription(BakeEntry::EquirectToCube,
			Utils::MakeBakeLayout({ BindingLayoutItem::Texture_SRV(0), BindingLayoutItem::Texture_UAV(0) }), true));
		descriptions.push_back(Utils::MakeBakeDescription(BakeEntry::DownsampleCube,
			Utils::MakeBakeLayout({ BindingLayoutItem::Texture_SRV(1), BindingLayoutItem::Texture_UAV(0) }), true));
		descriptions.push_back(Utils::MakeBakeDescription(BakeEntry::PrefilterSpecular,
			Utils::MakeBakeLayout({ BindingLayoutItem::Texture_SRV(1), BindingLayoutItem::Texture_SRV(2), BindingLayoutItem::Sampler(0),
				BindingLayoutItem::Texture_UAV(0) }),
			true));
		descriptions.push_back(Utils::MakeBakeDescription(BakeEntry::ProjectIrradiance,
			Utils::MakeBakeLayout({ BindingLayoutItem::Texture_SRV(1), BindingLayoutItem::StructuredBuffer_UAV(1) }), false));
		descriptions.push_back(Utils::MakeBakeDescription(BakeEntry::ReduceIrradiance,
			Utils::MakeBakeLayout({ BindingLayoutItem::StructuredBuffer_SRV(3), BindingLayoutItem::StructuredBuffer_UAV(2) }), false));
		return descriptions;
	}

	uint32_t EnvironmentBaker::GetPipelineCount() const
	{
		return static_cast<uint32_t>(m_State->Pipelines.size());
	}

	Result<EnvironmentData> EnvironmentBaker::Bake(const EnvironmentBakeInput& input)
	{
		return BakeWithOptions(input, EnvironmentBakeOptions{});
	}

	Result<EnvironmentData> EnvironmentBaker::BakeWithOptions(const EnvironmentBakeInput& input, const EnvironmentBakeOptions& options)
	{
		ENGINE_TRY(Utils::ValidateInput(input));
		ENGINE_TRY(Utils::ValidateOptions(options));
		State& state = *m_State;
		GraphicsDevice& device = *state.Device;
		const BakePlan plan = Utils::MakePlan(input, options);

		// The transient GPU objects: the equirectangular source (written by the bake's own command list, not a host-copied
		// image, so it needs no frame's garbage collection: a one-shot --bake-engine-assets run ends without one), both cubes,
		// the SH9 partial sums and totals, and the totals' readback buffer. Dropped when this returns; NVRHI defers their
		// destruction until the bake's command list retired (§8.14 item 2).
		const std::vector<float> equirectTexels = Utils::MakeEquirectTexels(input);
		nvrhi::TextureDesc equirectDesc;
		equirectDesc.width = input.Width;
		equirectDesc.height = input.Height;
		equirectDesc.format = Utils::EquirectFormat;
		equirectDesc.isShaderResource = true;
		equirectDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		equirectDesc.keepInitialState = true;
		equirectDesc.debugName = "EnvironmentBaker.Equirect";
		ENGINE_TRY_ASSIGN(const nvrhi::TextureHandle equirect, device.CreateTexture(equirectDesc));
		ENGINE_TRY_ASSIGN(const nvrhi::TextureHandle skybox, device.CreateTexture(Utils::MakeCubeDesc(plan.SkyboxFaceSize, plan.SkyboxMipCount, "EnvironmentBaker.Skybox")));
		ENGINE_TRY_ASSIGN(const nvrhi::TextureHandle specular, device.CreateTexture(Utils::MakeCubeDesc(plan.SpecularFaceSize, plan.SpecularMipCount, "EnvironmentBaker.Specular")));
		const uint32_t irradianceSize = Utils::GetLevelSize(plan.SkyboxFaceSize, plan.IrradianceLevel);
		const uint32_t irradianceGroups = Utils::GetGroupCount(irradianceSize) * Utils::GetGroupCount(irradianceSize) * CubeMapData::FaceCount;
		ENGINE_TRY_ASSIGN(const nvrhi::BufferHandle partials,
			device.CreateBuffer(Utils::MakeTermsBufferDesc(static_cast<uint64_t>(irradianceGroups) * Utils::IrradianceTerms, "EnvironmentBaker.IrradiancePartials")));
		ENGINE_TRY_ASSIGN(const nvrhi::BufferHandle totals, device.CreateBuffer(Utils::MakeTermsBufferDesc(Utils::IrradianceTerms, "EnvironmentBaker.IrradianceTotals")));
		nvrhi::BufferDesc stagingDesc;
		stagingDesc.byteSize = Utils::IrradianceTerms * sizeof(glm::vec4);
		stagingDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
		stagingDesc.initialState = nvrhi::ResourceStates::CopyDest;
		stagingDesc.keepInitialState = true;
		stagingDesc.debugName = "EnvironmentBaker.IrradianceReadback";
		ENGINE_TRY_ASSIGN(const nvrhi::BufferHandle staging, device.CreateBuffer(stagingDesc));
		// Not an immediate command list: an import may run while the caller's frame has its own list open (as Readback).
		ENGINE_TRY_ASSIGN(const nvrhi::CommandListHandle commandList, device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));

		// Each dispatch's binding set, created up front so a failure leaves nothing recorded; the command list holds the sets
		// it uses until it retires.
		std::vector<nvrhi::BindingSetHandle> sets;
		const auto makeSet = [&device, &state, &sets](BakeEntry entry, std::vector<nvrhi::BindingSetItem> items) -> Result<nvrhi::IBindingSet*>
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = std::move(items);
			desc.bindings.push_back(nvrhi::BindingSetItem::PushConstants(Utils::PushConstantsSlot, sizeof(EnvironmentBakeConstants)));
			ENGINE_TRY_ASSIGN(nvrhi::BindingSetHandle set, device.CreateBindingSet(desc, *state.Pipelines[Utils::ToIndex(entry)].BindingLayouts[0]));
			sets.push_back(set);
			return set.Get();
		};
		std::vector<BakeDispatch> dispatches;
		const auto cubeLevel = [](nvrhi::ITexture* cube, uint32_t slot, uint32_t level)
		{
			return nvrhi::BindingSetItem::Texture_SRV(slot, cube, Utils::CubeFormat, Utils::GetLevelFaces(level), nvrhi::TextureDimension::Texture2DArray);
		};
		const auto cubeTarget = [](nvrhi::ITexture* cube, uint32_t level)
		{
			return nvrhi::BindingSetItem::Texture_UAV(0, cube, Utils::CubeFormat, Utils::GetLevelFaces(level), nvrhi::TextureDimension::Texture2DArray);
		};

		// Step 2: the skybox's level 0 from the source, then its mip chain.
		{
			ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, makeSet(BakeEntry::EquirectToCube, { nvrhi::BindingSetItem::Texture_SRV(0, equirect), cubeTarget(skybox, 0) }));
			BakeDispatch dispatch{ .Entry = BakeEntry::EquirectToCube, .Set = set };
			dispatch.Constants.FaceSize = plan.SkyboxFaceSize;
			dispatch.Constants.SourceWidth = input.Width;
			dispatch.GroupsX = dispatch.GroupsY = Utils::GetGroupCount(plan.SkyboxFaceSize);
			dispatch.GroupsZ = CubeMapData::FaceCount;
			dispatches.push_back(dispatch);
		}
		for (uint32_t level = 1; level < plan.SkyboxMipCount; ++level)
		{
			ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, makeSet(BakeEntry::DownsampleCube, { cubeLevel(skybox, 1, level - 1), cubeTarget(skybox, level) }));
			BakeDispatch dispatch{ .Entry = BakeEntry::DownsampleCube, .Set = set };
			dispatch.Constants.FaceSize = Utils::GetLevelSize(plan.SkyboxFaceSize, level);
			dispatch.Constants.SourceFaceSize = Utils::GetLevelSize(plan.SkyboxFaceSize, level - 1);
			dispatch.GroupsX = dispatch.GroupsY = Utils::GetGroupCount(dispatch.Constants.FaceSize);
			dispatch.GroupsZ = CubeMapData::FaceCount;
			dispatches.push_back(dispatch);
		}

		// Step 3: specular mip 0 resampled from the skybox, then the importance-sampled mips.
		const double skyboxTexelSolidAngle = 4.0 * std::numbers::pi / (6.0 * static_cast<double>(plan.SkyboxFaceSize) * plan.SkyboxFaceSize);
		for (uint32_t level = 0; level < plan.SpecularMipCount; ++level)
		{
			ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, makeSet(BakeEntry::PrefilterSpecular, { cubeLevel(skybox, 1, plan.SpecularSourceLevel), nvrhi::BindingSetItem::Texture_SRV(2, skybox, Utils::CubeFormat, nvrhi::AllSubresources, nvrhi::TextureDimension::TextureCube), nvrhi::BindingSetItem::Sampler(0, state.LinearClamp), cubeTarget(specular, level) }));
			BakeDispatch dispatch{ .Entry = BakeEntry::PrefilterSpecular, .Set = set };
			dispatch.Constants.FaceSize = Utils::GetLevelSize(plan.SpecularFaceSize, level);
			dispatch.Constants.SourceFaceSize = Utils::GetLevelSize(plan.SkyboxFaceSize, plan.SpecularSourceLevel);
			if (level > 0)
			{
				const double roughness = static_cast<double>(level) / (plan.SpecularMipCount - 1);
				dispatch.Constants.SampleCount = Utils::GetSampleCount(plan, level);
				dispatch.Constants.Alpha = static_cast<float>(roughness * roughness);
				dispatch.Constants.SourceTexelSolidAngle = static_cast<float>(skyboxTexelSolidAngle);
				dispatch.Constants.SourceMaxLod = static_cast<float>(plan.SkyboxMipCount - 1);
			}
			dispatch.GroupsX = dispatch.GroupsY = Utils::GetGroupCount(dispatch.Constants.FaceSize);
			dispatch.GroupsZ = CubeMapData::FaceCount;
			dispatches.push_back(dispatch);
		}

		// Step 4: the SH9 projection of one skybox level and its reduction.
		{
			ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set,
				makeSet(BakeEntry::ProjectIrradiance, { cubeLevel(skybox, 1, plan.IrradianceLevel), nvrhi::BindingSetItem::StructuredBuffer_UAV(1, partials) }));
			BakeDispatch dispatch{ .Entry = BakeEntry::ProjectIrradiance, .Set = set };
			dispatch.Constants.FaceSize = irradianceSize;
			dispatch.GroupsX = dispatch.GroupsY = Utils::GetGroupCount(irradianceSize);
			dispatch.GroupsZ = CubeMapData::FaceCount;
			dispatches.push_back(dispatch);
		}
		{
			ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, makeSet(BakeEntry::ReduceIrradiance, { nvrhi::BindingSetItem::StructuredBuffer_SRV(3, partials), nvrhi::BindingSetItem::StructuredBuffer_UAV(2, totals) }));
			BakeDispatch dispatch{ .Entry = BakeEntry::ReduceIrradiance, .Set = set };
			dispatch.Constants.GroupCount = irradianceGroups;
			dispatches.push_back(dispatch);
		}

		commandList->open();
		commandList->beginMarker("EnvironmentBake");
		commandList->writeTexture(equirect, 0, 0, equirectTexels.data(), static_cast<size_t>(input.Width) * 4 * sizeof(float));
		for (const BakeDispatch& dispatch : dispatches)
		{
			nvrhi::ComputeState compute;
			compute.pipeline = state.Pipelines[Utils::ToIndex(dispatch.Entry)].Pipeline;
			compute.bindings = { dispatch.Set };
			commandList->setComputeState(compute);
			commandList->setPushConstants(&dispatch.Constants, sizeof(dispatch.Constants));
			commandList->dispatch(dispatch.GroupsX, dispatch.GroupsY, dispatch.GroupsZ);
		}
		commandList->copyBuffer(staging, 0, totals, 0, stagingDesc.byteSize);
		commandList->endMarker();
		commandList->close();
		const uint64_t submission = device.ExecuteCommandList(*commandList);
		// Bounded (a hang or device loss is fatal), so the readbacks and the map below never wait.
		WaitForSubmission(device, submission, "EnvironmentBaker");

		EnvironmentData environment;
		Readback readback(device);
		ENGINE_TRY_ASSIGN(environment.Skybox, Utils::ReadCube(readback, *skybox, plan.SkyboxFaceSize, plan.SkyboxMipCount));
		ENGINE_TRY_ASSIGN(environment.Specular, Utils::ReadCube(readback, *specular, plan.SpecularFaceSize, plan.SpecularMipCount));

		nvrhi::IDevice* nvrhiDevice = device.GetNvrhiDevice();
		const void* mapped = nvrhiDevice->mapBuffer(staging, nvrhi::CpuAccessMode::Read);
		if (mapped == nullptr)
			return MakeError(ErrorCode::Gpu, "cannot map the buffer '{}'", stagingDesc.debugName);
		std::array<glm::vec4, Utils::IrradianceTerms> projected{};
		std::memcpy(projected.data(), mapped, sizeof(projected));
		nvrhiDevice->unmapBuffer(staging);
		environment.IrradianceSH9 = Utils::MakeIrradianceSH9(projected);
		return environment;
	}

}
