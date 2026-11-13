#include "EnginePCH.h"
#include "Engine/Renderer/Private/ProbeShading.h"

#include "Engine/Core/Aabb.h"
#include "Engine/Graphics/CommandListRecording.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/Private/LightingInputs.h"
#include "Engine/Renderer/RenderPrepare.h"

#include <glm/glm.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

namespace Engine {

	namespace Utils {

		namespace {

			// A probe's box in the world: its rendered world matrix's inverse, the lengths of its columns and the box's world
			// volume (the smaller box wins where boxes overlap).
			struct ProbeBox
			{
				glm::mat4 WorldToLocal = glm::mat4(1.0f);
				glm::vec3 AxisScale = glm::vec3(1.0f);
				float WorldVolume = 0.0f;
			};

			struct VolumeCandidate
			{
				const IrradianceVolumeItem* Item = nullptr;
				ProbeBox Box{};
				nvrhi::BufferHandle Probes{};
				ProbeMirrorId Id{};
				glm::uvec3 Counts = glm::uvec3(0);
			};

			struct ReflectionCandidate
			{
				const ReflectionProbeItem* Item = nullptr;
				ProbeBox Box{};
				nvrhi::TextureHandle Cube{};
				ProbeMirrorId Id{};
			};

		}

		static bool IsFiniteMatrix(const glm::mat4& matrix)
		{
			for (int column = 0; column < 4; ++column)
			{
				for (int row = 0; row < 4; ++row)
				{
					if (!std::isfinite(matrix[column][row]))
						return false;
				}
			}
			return true;
		}

		// The box of a probe whose rendered world matrix is `world` and whose box is `size` about its origin, or nullopt when it
		// has none: a matrix or an inverse that is not finite, a singular matrix, or a size that is not finite and positive.
		static std::optional<ProbeBox> MakeProbeBox(const glm::mat4& world, const glm::vec3& size)
		{
			if (!IsFiniteMatrix(world) || !std::isfinite(size.x) || !std::isfinite(size.y) || !std::isfinite(size.z) || size.x <= 0.0f || size.y <= 0.0f
				|| size.z <= 0.0f)
				return std::nullopt;
			const float determinant = glm::determinant(glm::mat3(world));
			if (!std::isfinite(determinant) || determinant == 0.0f)
				return std::nullopt;
			ProbeBox box;
			box.WorldToLocal = glm::inverse(world);
			if (!IsFiniteMatrix(box.WorldToLocal))
				return std::nullopt;
			box.AxisScale = glm::vec3(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2])));
			box.WorldVolume = std::abs(determinant) * size.x * size.y * size.z;
			return box;
		}

		static float ClampProbeIntensity(float intensity)
		{
			return std::isfinite(intensity) ? std::clamp(intensity, 0.0f, MaxEnvironmentIntensity) : 0.0f;
		}

		static float ClampBlendDistance(float distance)
		{
			return std::isfinite(distance) && distance > 0.0f ? distance : 0.0f;
		}

		// Whether `box` of `size` about the origin of `world` lies outside the view of `camera`.
		static bool IsProbeOutsideView(const glm::mat4& world, const glm::vec3& size, const CameraData& camera)
		{
			return IsOutsideView(Aabb{ .Min = -size * 0.5f, .Max = size * 0.5f }, world, camera);
		}

		// Orders `candidates` from the largest box to the smallest, ties in snapshot order.
		template<typename Candidate>
		static void SortLargestFirst(std::vector<Candidate>& candidates)
		{
			std::ranges::stable_sort(candidates, [](const Candidate& left, const Candidate& right)
			{
				return left.Box.WorldVolume > right.Box.WorldVolume;
			});
		}

		ProbeShadingInputs PrepareProbeShading(GpuResourceCache& cache, const RenderSnapshot& snapshot, const CameraData& camera, bool enabled)
		{
			ProbeShadingInputs inputs;
			if (!enabled)
				return inputs;

			std::vector<VolumeCandidate> volumes;
			for (const IrradianceVolumeItem& item : snapshot.IrradianceVolumes)
			{
				const std::optional<ProbeBox> box = MakeProbeBox(item.World, item.Size);
				if (!box.has_value() || IsProbeOutsideView(item.World, item.Size, camera))
					continue;
				if (volumes.size() == MaxViewIrradianceVolumes)
				{
					++inputs.DroppedVolumes;
					continue;
				}
				const GpuIrradianceVolume* mirror = cache.GetIrradianceVolume(item.Data);
				if (mirror == nullptr || mirror->Probes == nullptr)
					continue;
				volumes.push_back({ .Item = &item,
					.Box = *box,
					.Probes = mirror->Probes,
					.Id = { .Asset = item.Data, .Version = mirror->Version },
					.Counts = glm::uvec3(mirror->CountX, mirror->CountY, mirror->CountZ) });
			}

			std::vector<ReflectionCandidate> reflections;
			for (const ReflectionProbeItem& item : snapshot.ReflectionProbes)
			{
				const std::optional<ProbeBox> box = MakeProbeBox(item.World, item.Size);
				if (!box.has_value() || IsProbeOutsideView(item.World, item.Size, camera))
					continue;
				if (reflections.size() == MaxViewReflectionProbes)
				{
					++inputs.DroppedReflectionProbes;
					continue;
				}
				const GpuEnvironment* mirror = cache.GetEnvironment(item.Data);
				if (mirror == nullptr || mirror->Specular == nullptr)
					continue;
				if (!reflections.empty())
				{
					// One cube array holds the view's cubes: they share the first one's format, size and levels.
					const nvrhi::TextureDesc& first = reflections.front().Cube->getDesc();
					const nvrhi::TextureDesc& cube = mirror->Specular->getDesc();
					if (cube.format != first.format || cube.width != first.width || cube.height != first.height || cube.mipLevels != first.mipLevels)
					{
						++inputs.MismatchedReflectionProbes;
						continue;
					}
				}
				reflections.push_back({ .Item = &item, .Box = *box, .Cube = mirror->Specular, .Id = { .Asset = item.Data, .Version = mirror->Version } });
			}

			SortLargestFirst(volumes);
			for (size_t index = 0; index < volumes.size(); ++index)
			{
				const VolumeCandidate& candidate = volumes[index];
				ProbeVolumeConstants& constants = inputs.Constants.Volumes[index];
				constants.WorldToLocal = candidate.Box.WorldToLocal;
				constants.HalfSize = glm::vec4(candidate.Item->Size * 0.5f, ClampBlendDistance(candidate.Item->BlendDistance));
				constants.AxisScale = glm::vec4(candidate.Box.AxisScale, ClampProbeIntensity(candidate.Item->Intensity));
				constants.Probes = glm::uvec4(candidate.Counts, inputs.CoefficientCount);
				inputs.CoefficientCount += candidate.Counts.x * candidate.Counts.y * candidate.Counts.z * IrradianceProbeCoefficientCount;
				inputs.Volumes.push_back(candidate.Probes);
				inputs.VolumeIds.push_back(candidate.Id);
			}

			SortLargestFirst(reflections);
			for (size_t index = 0; index < reflections.size(); ++index)
			{
				const ReflectionCandidate& candidate = reflections[index];
				ProbeReflectionConstants& constants = inputs.Constants.Reflections[index];
				constants.WorldToLocal = candidate.Box.WorldToLocal;
				constants.LocalToWorld = candidate.Item->World;
				constants.HalfSize = glm::vec4(candidate.Item->Size * 0.5f, ClampBlendDistance(candidate.Item->BlendDistance));
				constants.AxisScale = glm::vec4(candidate.Box.AxisScale, ClampProbeIntensity(candidate.Item->Intensity));
				constants.Lookup = glm::vec4(static_cast<float>(candidate.Cube->getDesc().mipLevels - 1), 0.0f, 0.0f, 0.0f);
				constants.Indices = glm::uvec4(static_cast<uint32_t>(index), candidate.Item->BoxProjection ? 1U : 0U, 0U, 0U);
				inputs.Cubes.push_back(candidate.Cube);
				inputs.CubeIds.push_back(candidate.Id);
			}
			inputs.Constants.Counts = glm::uvec4(static_cast<uint32_t>(volumes.size()), static_cast<uint32_t>(reflections.size()), 0U, 0U);
			return inputs;
		}

		Status EnsureProbeViewResources(GraphicsDevice& device, ProbeViewResources& resources, ProbeShadingInputs& inputs)
		{
			Status result;
			const auto keepFirstError = [&result](Status status)
			{
				if (!status && result)
					result = std::move(status);
			};
			const auto withoutVolumes = [&inputs]()
			{
				std::ranges::fill(inputs.Constants.Volumes, ProbeVolumeConstants{});
				inputs.Constants.Counts.x = 0;
				inputs.Volumes.clear();
				inputs.VolumeIds.clear();
				inputs.CoefficientCount = 0;
			};
			const auto withoutReflections = [&inputs]()
			{
				std::ranges::fill(inputs.Constants.Reflections, ProbeReflectionConstants{});
				inputs.Constants.Counts.y = 0;
				inputs.Cubes.clear();
				inputs.CubeIds.clear();
			};

			if (resources.Constants == nullptr)
			{
				nvrhi::BufferDesc desc;
				desc.byteSize = sizeof(ProbeConstants);
				desc.isConstantBuffer = true;
				desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
				desc.keepInitialState = true;
				desc.debugName = "SceneRenderer.ProbeConstants";
				Result<nvrhi::BufferHandle> created = device.CreateBuffer(desc);
				if (!created.has_value())
				{
					// Without its constants the forward set cannot be made; the render reports the error.
					withoutVolumes();
					withoutReflections();
					return std::unexpected(std::move(created).error().WithContext("while creating the view's probe constants"));
				}
				resources.Constants = std::move(*created);
			}

			if (inputs.CoefficientCount > 0)
			{
				const uint64_t needed = static_cast<uint64_t>(inputs.CoefficientCount) * sizeof(glm::vec4);
				if (resources.Coefficients == nullptr || resources.Coefficients->getDesc().byteSize < needed)
				{
					resources.Coefficients = nullptr;
					resources.CopiedVolumes.clear();
					nvrhi::BufferDesc desc;
					desc.byteSize = needed;
					desc.structStride = sizeof(glm::vec4);
					desc.initialState = nvrhi::ResourceStates::ShaderResource;
					desc.keepInitialState = true;
					desc.debugName = "SceneRenderer.IrradianceProbes";
					Result<nvrhi::BufferHandle> created = device.CreateBuffer(desc);
					if (created.has_value())
						resources.Coefficients = std::move(*created);
					else
					{
						withoutVolumes();
						keepFirstError(std::unexpected(std::move(created).error().WithContext("while creating the view's irradiance probe buffer")));
					}
				}
			}

			if (!inputs.Cubes.empty())
			{
				const nvrhi::TextureDesc& source = inputs.Cubes.front()->getDesc();
				const auto layers = static_cast<uint32_t>(inputs.Cubes.size() * 6);
				const bool fits = resources.Cubes != nullptr && resources.Cubes->getDesc().format == source.format && resources.Cubes->getDesc().width == source.width
					&& resources.Cubes->getDesc().height == source.height && resources.Cubes->getDesc().mipLevels == source.mipLevels
					&& resources.Cubes->getDesc().arraySize >= layers;
				if (!fits)
				{
					resources.Cubes = nullptr;
					resources.CopiedCubes.clear();
					nvrhi::TextureDesc desc;
					desc.width = source.width;
					desc.height = source.height;
					desc.arraySize = layers;
					desc.mipLevels = source.mipLevels;
					desc.format = source.format;
					desc.dimension = nvrhi::TextureDimension::TextureCubeArray;
					desc.isShaderResource = true;
					desc.initialState = nvrhi::ResourceStates::ShaderResource;
					desc.keepInitialState = true;
					desc.debugName = "SceneRenderer.ReflectionProbeCubes";
					Result<nvrhi::TextureHandle> created = device.CreateTexture(desc);
					if (created.has_value())
						resources.Cubes = std::move(*created);
					else
					{
						withoutReflections();
						keepFirstError(std::unexpected(std::move(created).error().WithContext("while creating the view's reflection probe cubes")));
					}
				}
			}
			return result;
		}

		void RecordProbeUploads(nvrhi::ICommandList& commandList, ProbeViewResources& resources, const ProbeShadingInputs& inputs)
		{
			if (resources.Constants == nullptr)
				return;
			commandList.writeBuffer(resources.Constants, &inputs.Constants, sizeof(inputs.Constants));
			if (!inputs.Volumes.empty() && resources.CopiedVolumes != inputs.VolumeIds)
			{
				for (size_t index = 0; index < inputs.Volumes.size(); ++index)
				{
					const glm::uvec4& probes = inputs.Constants.Volumes[index].Probes;
					const uint64_t bytes = static_cast<uint64_t>(probes.x) * probes.y * probes.z * IrradianceProbeCoefficientCount * sizeof(glm::vec4);
					commandList.copyBuffer(resources.Coefficients, static_cast<uint64_t>(probes.w) * sizeof(glm::vec4), inputs.Volumes[index], 0, bytes);
				}
				resources.CopiedVolumes = inputs.VolumeIds;
			}
			if (!inputs.Cubes.empty() && resources.CopiedCubes != inputs.CubeIds)
			{
				for (size_t cube = 0; cube < inputs.Cubes.size(); ++cube)
				{
					nvrhi::ITexture* source = inputs.Cubes[cube];
					const uint32_t mipLevels = source->getDesc().mipLevels;
					for (uint32_t face = 0; face < 6; ++face)
					{
						for (uint32_t level = 0; level < mipLevels; ++level)
						{
							commandList.copyTexture(resources.Cubes, nvrhi::TextureSlice().setArraySlice(static_cast<uint32_t>(cube * 6) + face).setMipLevel(level), source,
								nvrhi::TextureSlice().setArraySlice(face).setMipLevel(level));
						}
					}
				}
				resources.CopiedCubes = inputs.CubeIds;
			}
		}

		Result<nvrhi::BufferHandle> CreateEmptyIrradianceProbes(GraphicsDevice& device)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = sizeof(glm::vec4);
			desc.structStride = sizeof(glm::vec4);
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = "SceneRenderer.EmptyIrradianceProbes";
			ENGINE_TRY_ASSIGN(nvrhi::BufferHandle buffer, device.CreateBuffer(desc));
			// Not an immediate command list: a caller may have the frame's immediate list open (NVRHI's validation allows one).
			ENGINE_TRY_ASSIGN(const nvrhi::CommandListHandle commandList,
				device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
			CommandListRecording recording(device, *commandList);
			const glm::vec4 zero(0.0f);
			commandList->writeBuffer(buffer, &zero, sizeof(zero));
			recording.Execute();
			return buffer;
		}

		Result<nvrhi::TextureHandle> CreateEmptyReflectionProbeCubes(GraphicsDevice& device)
		{
			nvrhi::TextureDesc desc;
			desc.width = 1;
			desc.height = 1;
			desc.arraySize = 6;
			desc.mipLevels = 1;
			desc.format = nvrhi::Format::RGBA16_FLOAT;
			desc.dimension = nvrhi::TextureDimension::TextureCubeArray;
			desc.isShaderResource = true;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = "SceneRenderer.EmptyReflectionProbeCubes";
			ENGINE_TRY_ASSIGN(nvrhi::TextureHandle texture, device.CreateTexture(desc));
			ENGINE_TRY_ASSIGN(const nvrhi::CommandListHandle commandList,
				device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
			CommandListRecording recording(device, *commandList);
			commandList->clearTextureFloat(texture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
			recording.Execute();
			return texture;
		}

	}

}
