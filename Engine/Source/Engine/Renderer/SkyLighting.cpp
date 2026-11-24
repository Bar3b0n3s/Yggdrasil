#include "EnginePCH.h"
#include "Engine/Renderer/SkyLighting.h"

#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/EnvironmentBaker.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/SkyAtmosphere.h"
#include "Shared/EnvironmentBakeConstants.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/FogConstants.h"
#include "Shared/ProbeConstants.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Engine {

	namespace Utils {

		constexpr std::string_view SkyComputeProgram = "Sky";
		// The Sky program's compute registers (Passes/Sky.slang).
		constexpr uint32_t SkyComputeConstantsSlot = 1;
		constexpr uint32_t SkyComputeTransmittanceSlot = 0;
		constexpr uint32_t SkyComputeMultipleScatteringSlot = 1;
		constexpr uint32_t SkyComputeSkyViewSlot = 2;
		constexpr uint32_t SkyComputeTotalsSlot = 3;
		constexpr uint32_t SkyComputeLinearClampSlot = 0;
		constexpr uint32_t SkyComputeLinearWrapUSlot = 1;
		constexpr uint32_t SkyComputeLutSlot = 0;
		constexpr uint32_t SkyComputeCubeSlot = 1;
		constexpr uint32_t SkyComputeResultsSlot = 2;
		// The push constants' slot, EnvironmentBake's too (its PushConstantsSlot).
		constexpr uint32_t SkyComputePushConstantsSlot = 0;

		enum class SkyEntry : uint32_t
		{
			TransmittanceLut,
			MultipleScatteringLut,
			SkyViewLut,
			SkyCube,
			SkyFinalize,
			Count
		};

		[[nodiscard]] static PipelineLayoutDescription MakeSkyComputeDescription(SkyEntry entry)
		{
			using nvrhi::BindingLayoutItem;
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			std::vector<StorageImageFormat> images;
			std::string name;
			std::string function;
			switch (entry)
			{
				case SkyEntry::TransmittanceLut:
					name = "SkyTransmittanceLut";
					function = "CSTransmittanceLut";
					layout.bindings = { BindingLayoutItem::ConstantBuffer(SkyComputeConstantsSlot), BindingLayoutItem::Texture_UAV(SkyComputeLutSlot) };
					images.push_back({ .Set = 0, .Register = SkyComputeLutSlot, .Format = nvrhi::Format::RGBA16_FLOAT });
					break;
				case SkyEntry::MultipleScatteringLut:
					name = "SkyMultipleScatteringLut";
					function = "CSMultipleScatteringLut";
					layout.bindings = { BindingLayoutItem::ConstantBuffer(SkyComputeConstantsSlot), BindingLayoutItem::Texture_SRV(SkyComputeTransmittanceSlot),
						BindingLayoutItem::Sampler(SkyComputeLinearClampSlot), BindingLayoutItem::Texture_UAV(SkyComputeLutSlot) };
					images.push_back({ .Set = 0, .Register = SkyComputeLutSlot, .Format = nvrhi::Format::RGBA16_FLOAT });
					break;
				case SkyEntry::SkyViewLut:
					name = "SkySkyViewLut";
					function = "CSSkyViewLut";
					layout.bindings = { BindingLayoutItem::ConstantBuffer(SkyComputeConstantsSlot), BindingLayoutItem::Texture_SRV(SkyComputeTransmittanceSlot),
						BindingLayoutItem::Texture_SRV(SkyComputeMultipleScatteringSlot), BindingLayoutItem::Sampler(SkyComputeLinearClampSlot),
						BindingLayoutItem::Texture_UAV(SkyComputeLutSlot) };
					images.push_back({ .Set = 0, .Register = SkyComputeLutSlot, .Format = nvrhi::Format::RGBA16_FLOAT });
					break;
				case SkyEntry::SkyCube:
					name = "SkyCube";
					function = "CSSkyCube";
					layout.bindings = { BindingLayoutItem::ConstantBuffer(SkyComputeConstantsSlot), BindingLayoutItem::Texture_SRV(SkyComputeTransmittanceSlot),
						BindingLayoutItem::Texture_SRV(SkyComputeSkyViewSlot), BindingLayoutItem::Sampler(SkyComputeLinearClampSlot),
						BindingLayoutItem::Sampler(SkyComputeLinearWrapUSlot), BindingLayoutItem::Texture_UAV(SkyComputeCubeSlot) };
					images.push_back({ .Set = 0, .Register = SkyComputeCubeSlot, .Format = nvrhi::Format::RGBA16_FLOAT });
					break;
				default:
					name = "SkyFinalize";
					function = "CSSkyFinalize";
					layout.bindings = { BindingLayoutItem::ConstantBuffer(SkyComputeConstantsSlot), BindingLayoutItem::StructuredBuffer_SRV(SkyComputeTotalsSlot),
						BindingLayoutItem::StructuredBuffer_UAV(SkyComputeResultsSlot) };
					break;
			}
			// Every entry's reflection lists the program's push constants (Passes/Sky.slang's Dispatch), read or not.
			layout.bindings.push_back(BindingLayoutItem::PushConstants(SkyComputePushConstantsSlot, sizeof(SkyDispatchConstants)));
			return { .Name = name, .Program = std::string(SkyComputeProgram), .Entries = { function }, .Permutation = {}, .BindingLayouts = { layout }, .StorageImages = std::move(images), .ConstantBuffers = { { .Set = 0, .Register = SkyComputeConstantsSlot, .ByteSize = sizeof(SkyConstants) } } };
		}

		// The cloud noise's period in world metres (Passes/Sky.slang: SkyCloudNoisePeriod cells of SkyCloudCellSize).
		constexpr double SkyCloudPeriod = 256.0 * 4000.0;
		// The sky's computes' thread groups (Passes/Sky.slang's [numthreads(8, 8, 1)]) and the bake's.
		constexpr uint32_t SkyGroupSize = 8;
		// EnvironmentBake's push-constant slot, its terms per SH9 projection, the face it projects at most and its entries' order
		// in EnvironmentBaker::GetLayoutDescriptions (CSEquirectToCube first, which the sky never runs).
		constexpr uint32_t SkyBakeTerms = 10;
		constexpr uint32_t SkyIrradianceMaxFaceSize = 64;
		enum class SkyBakeEntry : uint32_t
		{
			DownsampleCube,
			PrefilterSpecular,
			ProjectIrradiance,
			ReduceIrradiance,
			Count
		};

		[[nodiscard]] static uint32_t GetSkyGroups(uint32_t size)
		{
			return (size + SkyGroupSize - 1) / SkyGroupSize;
		}

		[[nodiscard]] static uint32_t GetSkyCubeMipCount()
		{
			uint32_t count = 1;
			for (uint32_t size = SkyCubeSize; size > 1; size /= 2)
				++count;
			return count;
		}

		// The cube level the SH9 projection reads: the first whose face is at most SkyIrradianceMaxFaceSize (the bake's rule).
		[[nodiscard]] static uint32_t GetSkyIrradianceLevel()
		{
			uint32_t level = 0;
			while ((SkyCubeSize >> level) > SkyIrradianceMaxFaceSize)
				++level;
			return level;
		}

		[[nodiscard]] static glm::vec2 WrapCloudOffset(const glm::dvec2& offset)
		{
			glm::vec2 wrapped(0.0f);
			for (glm::length_t axis = 0; axis < 2; ++axis)
			{
				const double value = std::isfinite(offset[axis]) ? offset[axis] - std::floor(offset[axis] / SkyCloudPeriod) * SkyCloudPeriod : 0.0;
				wrapped[axis] = static_cast<float>(value);
			}
			return wrapped;
		}

		[[nodiscard]] static float GetCosineOfHalfAngle(float degrees)
		{
			const double half = std::isfinite(degrees) ? std::clamp(static_cast<double>(degrees), 0.0, 180.0) * 0.5 : 0.0;
			return static_cast<float>(DetMath::Cos(half * std::numbers::pi / 180.0));
		}

		[[nodiscard]] static glm::vec3 MakeSkyColorUsable(const glm::vec3& color)
		{
			glm::vec3 result(0.0f);
			for (glm::length_t channel = 0; channel < 3; ++channel)
				result[channel] = std::isfinite(color[channel]) ? std::max(color[channel], 0.0f) : 0.0f;
			return result;
		}

		[[nodiscard]] static std::optional<glm::dvec3> NormalizeSkyDirection(const glm::dvec3& vector)
		{
			const double lengthSquared = glm::dot(vector, vector);
			if (!std::isfinite(lengthSquared) || !(lengthSquared > 1e-30))
				return std::nullopt;
			return vector / std::sqrt(lengthSquared);
		}

		[[nodiscard]] static float ClampSkyFloat(float value, float low, float high, float fallback)
		{
			return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
		}

		[[nodiscard]] static nvrhi::TextureDesc MakeSkyLutDesc(uint32_t width, uint32_t height, const char* name)
		{
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = nvrhi::Format::RGBA16_FLOAT;
			desc.isShaderResource = true;
			desc.isUAV = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = name;
			return desc;
		}

		[[nodiscard]] static nvrhi::TextureDesc MakeSkyCubeDesc(uint32_t mipCount, const char* name)
		{
			nvrhi::TextureDesc desc = MakeSkyLutDesc(SkyCubeSize, SkyCubeSize, name);
			desc.arraySize = 6;
			desc.mipLevels = mipCount;
			desc.dimension = nvrhi::TextureDimension::TextureCube;
			return desc;
		}

		[[nodiscard]] static nvrhi::BufferDesc MakeSkyVectorsDesc(uint64_t count, const char* name)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = count * sizeof(glm::vec4);
			desc.structStride = sizeof(glm::vec4);
			desc.canHaveUAVs = true;
			desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
			desc.keepInitialState = true;
			desc.debugName = name;
			return desc;
		}

		// One level of a cube as six layers.
		[[nodiscard]] static nvrhi::TextureSubresourceSet GetSkyCubeLevel(uint32_t level)
		{
			return nvrhi::TextureSubresourceSet(level, 1, 0, 6);
		}

	}

	glm::vec2 ComputeSkyCloudOffset(const glm::vec2& cloudSpeed, double simulationTime, double step)
	{
		const double time = std::isfinite(simulationTime) && simulationTime > 0.0 ? simulationTime : 0.0;
		const double stepped = step > 0.0 ? std::floor(time / step) * step : time;
		glm::dvec2 speed(0.0);
		for (glm::length_t axis = 0; axis < 2; ++axis)
			speed[axis] = std::isfinite(cloudSpeed[axis]) ? std::clamp(static_cast<double>(cloudSpeed[axis]), -100.0, 100.0) : 0.0;
		return Utils::WrapCloudOffset(speed * stepped);
	}

	SkyConstants MakeSkyConstants(const RenderSky& sky, std::span<const LightData> lights, const CameraData& camera, const RenderFog& fog)
	{
		SkyConstants constants;
		const SkyAtmosphere atmosphere = MakeSkyAtmosphere(sky.Haze, sky.Overcast, sky.GroundAlbedo);
		constants.RayleighScattering = glm::vec3(atmosphere.RayleighScattering);
		constants.BottomRadius = static_cast<float>(SkyBottomRadius);
		constants.OzoneAbsorption = glm::vec3(atmosphere.OzoneAbsorption);
		constants.TopRadius = static_cast<float>(SkyTopRadius);
		constants.GroundAlbedo = glm::vec3(atmosphere.GroundAlbedo);
		constants.MieScattering = static_cast<float>(atmosphere.MieScattering);
		constants.MieExtinction = static_cast<float>(atmosphere.MieExtinction);
		constants.MieAsymmetry = static_cast<float>(atmosphere.MieAsymmetry);
		constants.CameraAltitude = static_cast<float>(ComputeSkyCameraAltitude(camera.Position));
		constants.Intensity = Utils::ClampSkyFloat(sky.Intensity, 0.0f, 65504.0f, 1.0f);
		// The Sun: its light's direction and authored illuminance, a disc of half its LightAngle.
		glm::dvec3 toSun(0.0, 1.0, 0.0);
		constants.SunDirection = glm::vec3(0.0f, 1.0f, 0.0f);
		if (sky.SunLight.has_value() && *sky.SunLight < lights.size())
		{
			const LightData& sun = lights[*sky.SunLight];
			if (const std::optional<glm::dvec3> direction = Utils::NormalizeSkyDirection(-glm::dvec3(sun.Direction)))
			{
				toSun = *direction;
				constants.SunDirection = glm::vec3(toSun);
				constants.SunIlluminance = Utils::MakeSkyColorUsable(sky.SunLightColor) * Utils::ClampSkyFloat(sun.Intensity, 0.0f, 65504.0f, 0.0f);
				constants.SunCosRadius = Utils::GetCosineOfHalfAngle(sun.LightAngle);
				const double solidAngle = 2.0 * std::numbers::pi * (1.0 - static_cast<double>(constants.SunCosRadius));
				const double disc = Utils::ClampSkyFloat(sky.SunDisc, 0.0f, 1000.0f, 1.0f);
				constants.SunDiscRadiance = solidAngle > 1e-12 ? static_cast<float>(disc / solidAngle) : 0.0f;
			}
		}
		// The Moon.
		constants.MoonCosRadius = Utils::GetCosineOfHalfAngle(Utils::ClampSkyFloat(sky.MoonSize, 0.1f, 10.0f, 0.52f));
		constants.MoonDirection = glm::vec3(0.0f, 1.0f, 0.0f);
		if (sky.HasMoon)
		{
			const glm::dvec3 toMoon = Utils::NormalizeSkyDirection(glm::dvec3(sky.MoonDirection)).value_or(glm::dvec3(0.0, 1.0, 0.0));
			constants.MoonDirection = glm::vec3(toMoon);
			constants.HasMoon = 1;
			constants.MoonFraction = static_cast<float>(ComputeMoonIlluminatedFraction(toSun, toMoon));
			if (sky.MoonLight.has_value() && *sky.MoonLight < lights.size())
				constants.MoonIlluminance = Utils::MakeSkyColorUsable(sky.MoonLightColor) * Utils::ClampSkyFloat(lights[*sky.MoonLight].Intensity, 0.0f, 65504.0f, 0.0f);
		}
		// The stars' frame, as rows.
		const glm::dmat3 frame = ComputeSkyStarFrame(toSun, sky.Latitude, sky.NorthAngle);
		constants.StarFrame0 = glm::vec4(glm::vec3(frame[0][0], frame[1][0], frame[2][0]), 0.0f);
		constants.StarFrame1 = glm::vec4(glm::vec3(frame[0][1], frame[1][1], frame[2][1]), 0.0f);
		constants.StarFrame2 = glm::vec4(glm::vec3(frame[0][2], frame[1][2], frame[2][2]), 0.0f);
		constants.Stars = Utils::ClampSkyFloat(sky.Stars, 0.0f, 100.0f, 1.0f);
		constants.Overcast = Utils::ClampSkyFloat(sky.Overcast, 0.0f, 1.0f, 0.0f);
		constants.CloudCoverage = Utils::ClampSkyFloat(sky.Clouds, 0.0f, 1.0f, 0.0f);
		constants.CloudHeight = SkyCloudHeight;
		constants.CloudOffset = ComputeSkyCloudOffset(sky.CloudSpeed, sky.SimulationTime, 0.0);
		constants.FogColor = Utils::MakeSkyColorUsable(fog.Color);
		constants.FogAmbientTint = Utils::ClampSkyFloat(fog.AmbientTint, 0.0f, 1.0f, 0.0f);
		return constants;
	}

	SkyLightingKey MakeSkyLightingKey(const SkyConstants& constants, const RenderSky& sky)
	{
		SkyLightingKey key;
		key.Constants = constants;
		// The image-based light is taken at sea level and with the stepped clouds: the camera and the live drift are not in it.
		key.Constants.CameraAltitude = 0.0f;
		key.Constants.CloudOffset = glm::vec2(0.0f);
		// The fog's colour is the finalize's, every frame.
		key.Constants.FogColor = glm::vec3(0.0f);
		key.Constants.FogAmbientTint = 0.0f;
		key.CloudOffset = ComputeSkyCloudOffset(sky.CloudSpeed, sky.SimulationTime, SkyCloudLightingStep);
		return key;
	}

	bool SkyLightingKey::operator==(const SkyLightingKey& other) const
	{
		return std::memcmp(&Constants, &other.Constants, sizeof(Constants)) == 0 && CloudOffset == other.CloudOffset;
	}

	Result<SkyViewResources> CreateSkyViewResources(GraphicsDevice& device)
	{
		SkyViewResources resources;
		nvrhi::BufferDesc constants;
		constants.byteSize = sizeof(SkyConstants);
		constants.isConstantBuffer = true;
		constants.initialState = nvrhi::ResourceStates::ConstantBuffer;
		constants.keepInitialState = true;
		constants.debugName = "Sky.Constants";
		ENGINE_TRY_ASSIGN(resources.Constants, device.CreateBuffer(constants));
		ENGINE_TRY_ASSIGN(resources.Transmittance, device.CreateTexture(Utils::MakeSkyLutDesc(SkyTransmittanceLutWidth, SkyTransmittanceLutHeight, "Sky.Transmittance")));
		ENGINE_TRY_ASSIGN(resources.MultipleScattering,
			device.CreateTexture(Utils::MakeSkyLutDesc(SkyMultipleScatteringLutSize, SkyMultipleScatteringLutSize, "Sky.MultipleScattering")));
		ENGINE_TRY_ASSIGN(resources.SkyView, device.CreateTexture(Utils::MakeSkyLutDesc(SkyViewLutWidth, SkyViewLutHeight, "Sky.SkyView")));
		ENGINE_TRY_ASSIGN(resources.SeaLevelSkyView, device.CreateTexture(Utils::MakeSkyLutDesc(SkyViewLutWidth, SkyViewLutHeight, "Sky.SeaLevelSkyView")));
		ENGINE_TRY_ASSIGN(resources.Cube, device.CreateTexture(Utils::MakeSkyCubeDesc(Utils::GetSkyCubeMipCount(), "Sky.Cube")));
		ENGINE_TRY_ASSIGN(resources.Specular, device.CreateTexture(Utils::MakeSkyCubeDesc(EnvironmentData::SpecularMipCount, "Sky.Specular")));
		const uint32_t irradianceSize = SkyCubeSize >> Utils::GetSkyIrradianceLevel();
		const uint64_t groups = static_cast<uint64_t>(Utils::GetSkyGroups(irradianceSize)) * Utils::GetSkyGroups(irradianceSize) * 6;
		ENGINE_TRY_ASSIGN(resources.Partials, device.CreateBuffer(Utils::MakeSkyVectorsDesc(groups * Utils::SkyBakeTerms, "Sky.IrradiancePartials")));
		ENGINE_TRY_ASSIGN(resources.Totals, device.CreateBuffer(Utils::MakeSkyVectorsDesc(Utils::SkyBakeTerms, "Sky.IrradianceTotals")));
		ENGINE_TRY_ASSIGN(resources.Results, device.CreateBuffer(Utils::MakeSkyVectorsDesc(SkyResultsCount, "Sky.Results")));
		return resources;
	}

	struct SkyLightingPass::State
	{
		GraphicsDevice* Device = nullptr;
		std::array<ComputePipeline, static_cast<size_t>(Utils::SkyEntry::Count)> Sky{};
		// EnvironmentBake's CSDownsampleCube, CSPrefilterSpecular, CSProjectIrradiance and CSReduceIrradiance.
		std::array<ComputePipeline, 4> Bake{};
		nvrhi::SamplerHandle LinearClamp{};
		nvrhi::SamplerHandle LinearWrapU{};
	};

	Result<Scope<SkyLightingPass>> SkyLightingPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<SkyLightingPass> pass = CreateScope<SkyLightingPass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		for (uint32_t entry = 0; entry < state.Sky.size(); ++entry)
		{
			ENGINE_TRY_ASSIGN(state.Sky[entry], pipelines.CreateComputePipeline({ .Layout = Utils::MakeSkyComputeDescription(static_cast<Utils::SkyEntry>(entry)) }));
		}
		std::vector<PipelineLayoutDescription> bake = EnvironmentBaker::GetLayoutDescriptions();
		// [0] is CSEquirectToCube, which the sky never runs.
		for (size_t index = 0; index < state.Bake.size(); ++index)
		{
			ENGINE_TRY_ASSIGN(state.Bake[index], pipelines.CreateComputePipeline({ .Layout = std::move(bake[index + 1]) }));
		}
		nvrhi::SamplerDesc clamp;
		clamp.setAllFilters(true);
		clamp.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		ENGINE_TRY_ASSIGN(state.LinearClamp, device.CreateSampler(clamp));
		nvrhi::SamplerDesc wrap = clamp;
		wrap.setAddressU(nvrhi::SamplerAddressMode::Wrap);
		ENGINE_TRY_ASSIGN(state.LinearWrapU, device.CreateSampler(wrap));
		return pass;
	}

	SkyLightingPass::SkyLightingPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	SkyLightingPass::~SkyLightingPass() = default;

	std::vector<PipelineLayoutDescription> SkyLightingPass::GetLayoutDescriptions()
	{
		std::vector<PipelineLayoutDescription> descriptions;
		for (uint32_t entry = 0; entry < static_cast<uint32_t>(Utils::SkyEntry::Count); ++entry)
			descriptions.push_back(Utils::MakeSkyComputeDescription(static_cast<Utils::SkyEntry>(entry)));
		std::vector<PipelineLayoutDescription> bake = EnvironmentBaker::GetLayoutDescriptions();
		// [0] is CSEquirectToCube, which the sky never runs.
		for (size_t index = 1; index < bake.size(); ++index)
			descriptions.push_back(std::move(bake[index]));
		return descriptions;
	}

	uint32_t SkyLightingPass::GetPipelineCount() const
	{
		return static_cast<uint32_t>(m_State->Sky.size() + m_State->Bake.size());
	}

	Status SkyLightingPass::Dispatch(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const ComputePipeline& pipeline, nvrhi::BindingSetDesc desc,
		const void* pushConstants, uint32_t pushSize, const glm::uvec3& groups)
	{
		State& state = *m_State;
		// Every layout holds push constants: an entry that takes none is given zeroed SkyDispatchConstants.
		const SkyDispatchConstants none{};
		const void* pushed = pushConstants != nullptr ? pushConstants : &none;
		const uint32_t pushedSize = pushConstants != nullptr ? pushSize : static_cast<uint32_t>(sizeof(none));
		desc.bindings.push_back(nvrhi::BindingSetItem::PushConstants(Utils::SkyComputePushConstantsSlot, pushedSize));
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, bindings.GetOrCreate(*state.Device, desc, *pipeline.BindingLayouts[0]));
		nvrhi::ComputeState compute;
		compute.pipeline = pipeline.Pipeline;
		compute.bindings = { set };
		commandList.setComputeState(compute);
		commandList.setPushConstants(pushed, pushedSize);
		commandList.dispatch(groups.x, groups.y, groups.z);
		return {};
	}

	Result<uint32_t> SkyLightingPass::RecordLuts(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SkyViewResources& resources, bool atmosphere,
		float altitude)
	{
		using nvrhi::BindingSetItem;
		State& state = *m_State;
		uint32_t dispatches = 0;
		if (atmosphere)
		{
			ENGINE_TRY(Dispatch(commandList, bindings, state.Sky[static_cast<size_t>(Utils::SkyEntry::TransmittanceLut)],
				{ .bindings = { BindingSetItem::ConstantBuffer(Utils::SkyComputeConstantsSlot, resources.Constants),
					  BindingSetItem::Texture_UAV(Utils::SkyComputeLutSlot, resources.Transmittance) } },
				nullptr, 0, glm::uvec3(Utils::GetSkyGroups(SkyTransmittanceLutWidth), Utils::GetSkyGroups(SkyTransmittanceLutHeight), 1)));
			ENGINE_TRY(Dispatch(commandList, bindings, state.Sky[static_cast<size_t>(Utils::SkyEntry::MultipleScatteringLut)],
				{ .bindings = { BindingSetItem::ConstantBuffer(Utils::SkyComputeConstantsSlot, resources.Constants),
					  BindingSetItem::Texture_SRV(Utils::SkyComputeTransmittanceSlot, resources.Transmittance),
					  BindingSetItem::Sampler(Utils::SkyComputeLinearClampSlot, state.LinearClamp),
					  BindingSetItem::Texture_UAV(Utils::SkyComputeLutSlot, resources.MultipleScattering) } },
				nullptr, 0, glm::uvec3(Utils::GetSkyGroups(SkyMultipleScatteringLutSize), Utils::GetSkyGroups(SkyMultipleScatteringLutSize), 1)));
			dispatches += 2;
		}
		ENGINE_TRY(RecordSkyView(commandList, bindings, resources, *resources.SkyView, altitude));
		return dispatches + 1;
	}

	Status SkyLightingPass::RecordSkyView(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SkyViewResources& resources, nvrhi::ITexture& target,
		float altitude)
	{
		using nvrhi::BindingSetItem;
		State& state = *m_State;
		const SkyDispatchConstants push{ .CloudOffset = glm::vec2(0.0f), .Altitude = altitude };
		return Dispatch(commandList, bindings, state.Sky[static_cast<size_t>(Utils::SkyEntry::SkyViewLut)],
			{ .bindings = { BindingSetItem::ConstantBuffer(Utils::SkyComputeConstantsSlot, resources.Constants),
				  BindingSetItem::Texture_SRV(Utils::SkyComputeTransmittanceSlot, resources.Transmittance),
				  BindingSetItem::Texture_SRV(Utils::SkyComputeMultipleScatteringSlot, resources.MultipleScattering),
				  BindingSetItem::Sampler(Utils::SkyComputeLinearClampSlot, state.LinearClamp), BindingSetItem::Texture_UAV(Utils::SkyComputeLutSlot, &target) } },
			&push, sizeof(push), glm::uvec3(Utils::GetSkyGroups(SkyViewLutWidth), Utils::GetSkyGroups(SkyViewLutHeight), 1));
	}

	Result<uint32_t> SkyLightingPass::RecordImageBasedLight(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SkyViewResources& resources,
		const glm::vec2& cloudOffset)
	{
		using nvrhi::BindingSetItem;
		State& state = *m_State;
		constexpr nvrhi::Format CubeFormat = nvrhi::Format::RGBA16_FLOAT;
		uint32_t dispatches = 0;
		// The sea-level sky view, then the cube's level 0 from it.
		ENGINE_TRY(RecordSkyView(commandList, bindings, resources, *resources.SeaLevelSkyView, static_cast<float>(SkyCameraHeightOffset)));
		++dispatches;
		const SkyDispatchConstants cube{ .CloudOffset = cloudOffset, .Altitude = static_cast<float>(SkyCameraHeightOffset) };
		ENGINE_TRY(Dispatch(commandList, bindings, state.Sky[static_cast<size_t>(Utils::SkyEntry::SkyCube)],
			{ .bindings = { BindingSetItem::ConstantBuffer(Utils::SkyComputeConstantsSlot, resources.Constants),
				  BindingSetItem::Texture_SRV(Utils::SkyComputeTransmittanceSlot, resources.Transmittance),
				  BindingSetItem::Texture_SRV(Utils::SkyComputeSkyViewSlot, resources.SeaLevelSkyView),
				  BindingSetItem::Sampler(Utils::SkyComputeLinearClampSlot, state.LinearClamp),
				  BindingSetItem::Sampler(Utils::SkyComputeLinearWrapUSlot, state.LinearWrapU),
				  BindingSetItem::Texture_UAV(Utils::SkyComputeCubeSlot, resources.Cube, CubeFormat, Utils::GetSkyCubeLevel(0), nvrhi::TextureDimension::Texture2DArray) } },
			&cube, sizeof(cube), glm::uvec3(Utils::GetSkyGroups(SkyCubeSize), Utils::GetSkyGroups(SkyCubeSize), 6)));
		++dispatches;
		ENGINE_TRY_ASSIGN(const uint32_t lighting, RecordCubeLighting(commandList, bindings, resources));
		return dispatches + lighting;
	}

	Result<uint32_t> SkyLightingPass::RecordCubeLighting(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SkyViewResources& resources)
	{
		using nvrhi::BindingSetItem;
		State& state = *m_State;
		constexpr nvrhi::Format CubeFormat = nvrhi::Format::RGBA16_FLOAT;
		uint32_t dispatches = 0;
		// The environment bake's steps on the sky cube: its mip chain, the prefilter, the SH9 projection and its reduction.
		const auto level = [](nvrhi::ITexture* texture, uint32_t slot, uint32_t mip)
		{
			return BindingSetItem::Texture_SRV(slot, texture, CubeFormat, Utils::GetSkyCubeLevel(mip), nvrhi::TextureDimension::Texture2DArray);
		};
		const auto target = [](nvrhi::ITexture* texture, uint32_t mip)
		{
			return BindingSetItem::Texture_UAV(0, texture, CubeFormat, Utils::GetSkyCubeLevel(mip), nvrhi::TextureDimension::Texture2DArray);
		};
		const ComputePipeline& downsample = state.Bake[static_cast<size_t>(Utils::SkyBakeEntry::DownsampleCube)];
		const uint32_t cubeMips = Utils::GetSkyCubeMipCount();
		for (uint32_t mip = 1; mip < cubeMips; ++mip)
		{
			EnvironmentBakeConstants constants{};
			constants.FaceSize = SkyCubeSize >> mip;
			constants.SourceFaceSize = SkyCubeSize >> (mip - 1);
			ENGINE_TRY(Dispatch(commandList, bindings, downsample, { .bindings = { level(resources.Cube, 1, mip - 1), target(resources.Cube, mip) } }, &constants,
				sizeof(constants), glm::uvec3(Utils::GetSkyGroups(constants.FaceSize), Utils::GetSkyGroups(constants.FaceSize), 6)));
			++dispatches;
		}
		const ComputePipeline& prefilter = state.Bake[static_cast<size_t>(Utils::SkyBakeEntry::PrefilterSpecular)];
		const double texelSolidAngle = 4.0 * std::numbers::pi / (6.0 * static_cast<double>(SkyCubeSize) * SkyCubeSize);
		for (uint32_t mip = 0; mip < EnvironmentData::SpecularMipCount; ++mip)
		{
			EnvironmentBakeConstants constants{};
			constants.FaceSize = SkyCubeSize >> mip;
			constants.SourceFaceSize = SkyCubeSize;
			if (mip > 0)
			{
				const double roughness = static_cast<double>(mip) / (EnvironmentData::SpecularMipCount - 1);
				constants.SampleCount = SkySpecularMinSamples + (SkySpecularMaxSamples - SkySpecularMinSamples) * (mip - 1) / (EnvironmentData::SpecularMipCount - 2);
				constants.Alpha = static_cast<float>(roughness * roughness);
				constants.SourceTexelSolidAngle = static_cast<float>(texelSolidAngle);
				constants.SourceMaxLod = static_cast<float>(cubeMips - 1);
			}
			ENGINE_TRY(Dispatch(commandList, bindings, prefilter,
				{ .bindings = { level(resources.Cube, 1, 0),
					  BindingSetItem::Texture_SRV(2, resources.Cube, CubeFormat, nvrhi::AllSubresources, nvrhi::TextureDimension::TextureCube),
					  BindingSetItem::Sampler(0, state.LinearClamp), target(resources.Specular, mip) } },
				&constants, sizeof(constants), glm::uvec3(Utils::GetSkyGroups(constants.FaceSize), Utils::GetSkyGroups(constants.FaceSize), 6)));
			++dispatches;
		}
		const uint32_t irradianceLevel = Utils::GetSkyIrradianceLevel();
		const uint32_t irradianceSize = SkyCubeSize >> irradianceLevel;
		EnvironmentBakeConstants project{};
		project.FaceSize = irradianceSize;
		ENGINE_TRY(Dispatch(commandList, bindings, state.Bake[static_cast<size_t>(Utils::SkyBakeEntry::ProjectIrradiance)],
			{ .bindings = { level(resources.Cube, 1, irradianceLevel), BindingSetItem::StructuredBuffer_UAV(1, resources.Partials) } }, &project, sizeof(project),
			glm::uvec3(Utils::GetSkyGroups(irradianceSize), Utils::GetSkyGroups(irradianceSize), 6)));
		EnvironmentBakeConstants reduce{};
		reduce.GroupCount = Utils::GetSkyGroups(irradianceSize) * Utils::GetSkyGroups(irradianceSize) * 6;
		ENGINE_TRY(Dispatch(commandList, bindings, state.Bake[static_cast<size_t>(Utils::SkyBakeEntry::ReduceIrradiance)],
			{ .bindings = { BindingSetItem::StructuredBuffer_SRV(3, resources.Partials), BindingSetItem::StructuredBuffer_UAV(2, resources.Totals) } }, &reduce,
			sizeof(reduce), glm::uvec3(1, 1, 1)));
		return dispatches + 2;
	}

	Status SkyLightingPass::RecordFinalize(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SkyViewResources& resources)
	{
		using nvrhi::BindingSetItem;
		return Dispatch(commandList, bindings, m_State->Sky[static_cast<size_t>(Utils::SkyEntry::SkyFinalize)],
			{ .bindings = { BindingSetItem::ConstantBuffer(Utils::SkyComputeConstantsSlot, resources.Constants),
				  BindingSetItem::StructuredBuffer_SRV(Utils::SkyComputeTotalsSlot, resources.Totals),
				  BindingSetItem::StructuredBuffer_UAV(Utils::SkyComputeResultsSlot, resources.Results) } },
			nullptr, 0, glm::uvec3(1, 1, 1));
	}

	void RecordSkyResultCopies(nvrhi::ICommandList& commandList, nvrhi::IBuffer* results, nvrhi::IBuffer* environment, std::span<nvrhi::IBuffer* const> fogs,
		nvrhi::IBuffer* probes, uint32_t reflectionProbes)
	{
		ENGINE_CORE_ASSERT(results != nullptr, "RecordSkyResultCopies needs the view's SkyResults buffer");
		// writeBuffer leaves each destination in CopyDest, and NVRHI places no barrier between two writes in one state (W45.1's
		// proof found the write-after-write hazard): moving the destinations to ConstantBuffer first completes their CPU writes,
		// so each copy's own barrier back to CopyDest orders it after them. A destination a pass already read is there already.
		const auto settle = [&commandList](nvrhi::IBuffer* buffer)
		{
			if (buffer != nullptr)
				commandList.setBufferState(buffer, nvrhi::ResourceStates::ConstantBuffer);
		};
		settle(environment);
		for (nvrhi::IBuffer* fog : fogs)
			settle(fog);
		settle(probes);
		commandList.commitBarriers();
		constexpr uint64_t Element = sizeof(glm::vec4);
		if (environment != nullptr)
			commandList.copyBuffer(environment, offsetof(EnvironmentConstants, IrradianceSH9), results, SkyResultsIrradiance * Element,
				sizeof(EnvironmentConstants::IrradianceSH9));
		for (nvrhi::IBuffer* fog : fogs)
			if (fog != nullptr)
				commandList.copyBuffer(fog, offsetof(FogConstants, Color), results, SkyResultsFogColor * Element, sizeof(FogConstants::Color));
		if (probes == nullptr)
			return;
		const uint32_t count = std::min(reflectionProbes, MaxViewReflectionProbes);
		for (uint32_t index = 0; index < count; ++index)
		{
			const uint64_t destination = offsetof(ProbeConstants, Reflections) + index * sizeof(ProbeReflectionConstants) + offsetof(ProbeReflectionConstants, AxisScale)
				+ 3 * sizeof(float);
			commandList.copyBuffer(probes, destination, results, (SkyResultsReflectionScales + index) * Element, sizeof(float));
		}
	}

}
