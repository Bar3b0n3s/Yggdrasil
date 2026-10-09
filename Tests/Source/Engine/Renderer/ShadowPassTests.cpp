#include "TestsPCH.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/ShadowPass.h"
#include "Shared/ShadowConstants.h"
#include "Shared/ShadowProbeConstants.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace Engine {

	namespace {

		constexpr AssetHandle ShadowQuadHandle{ 0x9a1001 };
		constexpr AssetHandle ShadowMaterialHandle{ 0x9a1002 };
		constexpr AssetHandle ShadowTextureHandle{ 0x9a1003 };

		void PublishShadowQuad(Test::InMemoryAssetManager& assets)
		{
			auto mesh = CreateRef<MeshData>();
			for (const glm::vec2& xy : { glm::vec2(-1, -1), glm::vec2(1, -1), glm::vec2(1, 1), glm::vec2(-1, 1) })
				mesh->Vertices.push_back({ .Position = glm::vec3(xy, 0),
					.Normal = glm::vec3(0, 0, 1),
					.Tangent = glm::vec4(1, 0, 0, 1),
					.TexCoord = xy * glm::vec2(0.5f, -0.5f) + 0.5f });
			mesh->Indices = { 0, 1, 2, 0, 2, 3 };
			mesh->Bounds = { .Min = glm::vec3(-1, -1, 0), .Max = glm::vec3(1, 1, 0) };
			mesh->Submeshes = { { .IndexOffset = 0, .IndexCount = 6, .MaterialSlot = 0, .Bounds = mesh->Bounds } };
			mesh->Slots = { { .Name = "Default", .DefaultMaterial = ShadowMaterialHandle } };
			assets.Publish(ShadowQuadHandle, mesh);
			assets.Publish(ShadowMaterialHandle, CreateRef<MaterialData>());
		}

		[[nodiscard]] ShadowCascadeSet IdentityShadowCascade()
		{
			ShadowCascadeSet set;
			set.Count = 1;
			set.Cascades[0].ViewProjection = glm::mat4(1);
			set.Cascades[0].LightFar = 1;
			set.Cascades[0].TexelWorldSize = 2.0f / 256;
			return set;
		}

		[[nodiscard]] glm::vec4 ShadowPixel(const Image& image, uint32_t x, uint32_t y)
		{
			glm::vec4 result;
			std::memcpy(&result, image.GetRow(y).data() + x * sizeof(result), sizeof(result));
			return result;
		}

		class ShadowSetup
		{
		public:
			explicit ShadowSetup(Test::HeadlessGpuFixture& gpu)
				: m_Device(gpu.GetDevice()), m_Cache(m_Device, m_Assets)
			{
				PublishShadowQuad(m_Assets);
				const auto descriptions = ShadowPass::GetLayoutDescriptions();
				REQUIRE(descriptions.size() == 6);
				auto pass = ShadowPass::Create(m_Device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pass, pass.error().ToString());
				m_Pass = std::move(*pass);
				auto cascade = m_Device.CreateTexture(ShadowPass::GetCascadeTargetDesc(256));
				auto atlas = m_Device.CreateTexture(ShadowPass::GetAtlasTargetDesc());
				REQUIRE(cascade);
				REQUIRE(atlas);
				m_Cascades = *cascade;
				m_Atlas = *atlas;

				nvrhi::BindingLayoutDesc layout;
				layout.visibility = nvrhi::ShaderType::Compute;
				layout.registerSpaceIsDescriptorSet = true;
				layout.bindings = {
					nvrhi::BindingLayoutItem::ConstantBuffer(1), nvrhi::BindingLayoutItem::Texture_SRV(1),
					nvrhi::BindingLayoutItem::Texture_SRV(2), nvrhi::BindingLayoutItem::Sampler(1),
					nvrhi::BindingLayoutItem::Texture_UAV(0), nvrhi::BindingLayoutItem::PushConstants(3, sizeof(ShadowProbeConstants))
				};
				ComputePipelineSpecification specification;
				specification.Layout = { .Name = "ShadowProbe",
					.Program = "ShadowProbe",
					.Entries = { "CSMain" },
					.Permutation = {},
					.BindingLayouts = { layout },
					.StorageImages = { { .Set = 0, .Register = 0, .Format = nvrhi::Format::RGBA32_FLOAT } },
					.ConstantBuffers = { { .Set = 0, .Register = 1, .ByteSize = sizeof(ShadowConstants) } } };
				auto pipeline = gpu.GetPipelines().CreateComputePipeline(specification);
				REQUIRE_MESSAGE(pipeline, pipeline.error().ToString());
				m_Probe = std::move(*pipeline);
				nvrhi::SamplerDesc sampler;
				sampler.setAllFilters(true)
					.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp)
					.setReductionType(nvrhi::SamplerReductionType::Comparison);
				// NVRHI comparison samplers are strict Less; Common/Shadows complements the result for reverse-Z.
				auto comparison = m_Device.CreateSampler(sampler);
				REQUIRE(comparison);
				m_Comparison = *comparison;
				auto command = m_Device.CreateCommandList();
				REQUIRE(command);
				(*command)->open();
				(*command)->clearDepthStencilTexture(m_Cascades, nvrhi::AllSubresources, true, 0, false, 0);
				(*command)->clearDepthStencilTexture(m_Atlas, nvrhi::AllSubresources, true, 0, false, 0);
				(*command)->close();
				m_Device.ExecuteCommandList(**command);
			}

			[[nodiscard]] Test::InMemoryAssetManager& GetAssets() { return m_Assets; }
			[[nodiscard]] const RenderStats& GetStats() const { return m_Stats; }

			ShadowRenderResult Render(std::span<const MeshDrawItem> items, const ShadowCascadeSet& cascades,
				const SpotShadowAtlas* spots = nullptr, float depthBias = 0)
			{
				auto command = m_Device.CreateCommandList();
				REQUIRE(command);
				(*command)->open();
				m_Stats = {};
				RenderRecordingContext recording(m_Stats, []
				{
					return 0.0;
				});
				auto result = m_Pass->Record(**command, recording, m_Bindings, m_Cache, m_Assets,
					{ .Casters = items,
						.Cascades = &cascades,
						.Spots = spots,
						.CascadeTarget = m_Cascades,
						.AtlasTarget = m_Atlas,
						.DepthBias = depthBias });
				(*command)->close();
				m_Device.ExecuteCommandList(**command);
				REQUIRE_MESSAGE(result, result.error().ToString());
				m_Bindings.ReleaseUnused();
				return *result;
			}

			Image Probe(uint32_t mode, uint32_t width = 256, uint32_t height = 256, const ShadowConstants& shadows = {},
				const glm::vec4& parameters = glm::vec4(0), uint32_t layer = 0)
			{
				nvrhi::TextureDesc outputDesc;
				outputDesc.width = width;
				outputDesc.height = height;
				outputDesc.format = nvrhi::Format::RGBA32_FLOAT;
				outputDesc.isUAV = true;
				outputDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
				outputDesc.keepInitialState = true;
				auto output = m_Device.CreateTexture(outputDesc);
				REQUIRE(output);
				nvrhi::BufferDesc bufferDesc;
				bufferDesc.byteSize = sizeof(shadows);
				bufferDesc.isConstantBuffer = true;
				bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
				bufferDesc.keepInitialState = true;
				auto buffer = m_Device.CreateBuffer(bufferDesc);
				REQUIRE(buffer);
				nvrhi::BindingSetDesc desc;
				desc.bindings = {
					nvrhi::BindingSetItem::ConstantBuffer(1, *buffer), nvrhi::BindingSetItem::Texture_SRV(1, m_Cascades),
					nvrhi::BindingSetItem::Texture_SRV(2, m_Atlas), nvrhi::BindingSetItem::Sampler(1, m_Comparison),
					nvrhi::BindingSetItem::Texture_UAV(0, *output), nvrhi::BindingSetItem::PushConstants(3, sizeof(ShadowProbeConstants))
				};
				auto set = m_Device.CreateBindingSet(desc, *m_Probe.BindingLayouts[0]);
				REQUIRE(set);
				auto command = m_Device.CreateCommandList();
				REQUIRE(command);
				(*command)->open();
				(*command)->writeBuffer(*buffer, &shadows, sizeof(shadows));
				nvrhi::ComputeState state;
				state.pipeline = m_Probe.Pipeline;
				state.bindings = { *set };
				(*command)->setComputeState(state);
				const ShadowProbeConstants push{ .Options = glm::uvec4(mode, layer, width, height), .Parameters = parameters };
				(*command)->setPushConstants(&push, sizeof(push));
				(*command)->dispatch((width + 7) / 8, (height + 7) / 8, 1);
				(*command)->close();
				m_Device.ExecuteCommandList(**command);
				Readback readback(m_Device);
				auto image = readback.ReadTexture(**output);
				REQUIRE_MESSAGE(image, image.error().ToString());
				return std::move(*image);
			}
		private:
			GraphicsDevice& m_Device;
			Test::InMemoryAssetManager m_Assets{};
			GpuResourceCache m_Cache;
			Scope<ShadowPass> m_Pass{};
			PassBindingCache m_Bindings{};
			nvrhi::TextureHandle m_Cascades{};
			nvrhi::TextureHandle m_Atlas{};
			ComputePipeline m_Probe{};
			nvrhi::SamplerHandle m_Comparison{};
			RenderStats m_Stats{};
		};

	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("Shadows: a 0.37 texel camera move stays below the shimmer threshold")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			ShadowSetup setup(gpu);
			CameraData camera;
			camera.ProjectionKind = RenderProjection::Orthographic;
			camera.Projection = ComputeReverseZProjection(RenderProjection::Orthographic, 60, 2, 0.1f, 100, 256, 256);
			camera.FarClip = 100;
			LightData light;
			light.CascadeCount = 1;
			auto cascades = BuildShadowCascades(camera, light, 0, 256);
			REQUIRE(cascades);
			MeshDrawItem item;
			item.Mesh = ShadowQuadHandle;
			item.World = glm::translate(glm::mat4(1), glm::vec3(0, 0, -10));
			CHECK(setup.Render(std::span(&item, 1), *cascades).DrawCalls == 1);
			const Image before = setup.Probe(0);
			// A fixed world receiver grid isolates shadow instability from the camera's expected screen-space motion.
			ShadowConstants shadows{};
			shadows.Counts.x = 1;
			shadows.CascadeViewProjection[0] = cascades->Cascades[0].ViewProjection;
			shadows.CascadeSplits.x = cascades->Cascades[0].SplitFar;
			shadows.CascadeBlendStarts.x = cascades->Cascades[0].BlendStart;
			shadows.CascadeDepthRanges[0] = glm::vec4(cascades->Cascades[0].LightNear, cascades->Cascades[0].LightFar,
				cascades->Cascades[0].PenumbraUvPerMetre, 0.05f);
			shadows.CascadeTexelWorldSizes.x = cascades->Cascades[0].TexelWorldSize;
			const Image visibilityBefore = setup.Probe(3, 256, 256, shadows, glm::vec4(0, 20, -20, 0));
			camera.Position.x = cascades->Cascades[0].TexelWorldSize * 0.37f;
			camera.View = glm::translate(glm::mat4(1), -camera.Position);
			auto moved = BuildShadowCascades(camera, light, 0, 256);
			REQUIRE(moved);
			setup.Render(std::span(&item, 1), *moved);
			const Image after = setup.Probe(0);
			CHECK(before.Pixels == after.Pixels);
			shadows.CascadeViewProjection[0] = moved->Cascades[0].ViewProjection;
			const Image visibilityAfter = setup.Probe(3, 256, 256, shadows, glm::vec4(0, 20, -20, 0));
			CHECK(visibilityBefore.Pixels == visibilityAfter.Pixels);
			CHECK(ShadowPixel(visibilityBefore, 128, 128).x < 0.1f);
			CHECK(ShadowPixel(visibilityBefore, 0, 128).x > 0.1f);
			CHECK(ShadowPixel(before, 128, 128).x > 0);
			REQUIRE(setup.GetStats().Passes.size() == 1);
			CHECK(setup.GetStats().Passes[0].Name == "DirectionalShadows");
			CHECK(setup.GetStats().Passes[0].DrawCalls == 1);
			CHECK(setup.GetStats().Passes[0].Triangles == 2);
		}

		TEST_CASE("Shadows: masked mirrored and double-sided casters preserve M8 parity")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			ShadowSetup setup(gpu);
			MeshDrawItem item;
			item.Mesh = ShadowQuadHandle;
			for (AlphaMode mode : { AlphaMode::Opaque, AlphaMode::Mask })
				for (uint32_t variant = 0; variant < 3; ++variant)
				{
					CAPTURE(static_cast<uint32_t>(mode));
					CAPTURE(variant);
					MaterialData material;
					material.AlphaMode = mode;
					material.DoubleSided = variant == 2;
					setup.GetAssets().Publish(ShadowMaterialHandle, CreateRef<MaterialData>(material));
					item.World = glm::translate(glm::mat4(1), glm::vec3(0, 0, 0.5f));
					if (variant == 1)
						item.World[0][0] = -1;
					if (variant == 2)
						item.World[2][2] = item.World[0][0] = -1; // back-facing, positive determinant
					CHECK(setup.Render(std::span(&item, 1), IdentityShadowCascade()).DrawCalls == 1);
					CHECK(ShadowPixel(setup.Probe(0), 128, 128).x == doctest::Approx(0.5));
					if (mode == AlphaMode::Mask)
					{
						material.BaseColor.a = 0.25f;
						setup.GetAssets().Publish(ShadowMaterialHandle, CreateRef<MaterialData>(material));
						setup.Render(std::span(&item, 1), IdentityShadowCascade());
						CHECK(ShadowPixel(setup.Probe(0), 128, 128).x == 0);
					}
				}
			MaterialData material;
			setup.GetAssets().Publish(ShadowMaterialHandle, CreateRef<MaterialData>(material));
			setup.Render(std::span(&item, 1), IdentityShadowCascade());
			CHECK(ShadowPixel(setup.Probe(0), 128, 128).x == 0); // single-sided back face
			material.AlphaMode = AlphaMode::Blend;
			setup.GetAssets().Publish(ShadowMaterialHandle, CreateRef<MaterialData>(material));
			CHECK(setup.Render(std::span(&item, 1), IdentityShadowCascade()).DrawCalls == 0);
		}

		TEST_CASE("Shadows: both depth-clamp and extended-near fallback render off-screen casters")
		{
			for (const bool disableDepthClamp : { false, true })
			{
				CAPTURE(disableDepthClamp);
				Test::HeadlessGpuFixture gpu({ .DisableDepthClamp = disableDepthClamp });
				ENGINE_REQUIRE_GPU(gpu);
				if (disableDepthClamp)
					REQUIRE_FALSE(gpu.GetDevice().GetInfo().DepthClamp);
				ShadowSetup setup(gpu);
				CameraData camera;
				camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60, 10, 0.1f, 100, 256, 256);
				LightData light;
				light.CascadeCount = 1;
				auto cascades = BuildShadowCascades(camera, light, 0, 256);
				REQUIRE(cascades);
				const ShadowCascade& cascade = cascades->Cascades[0];
				ShadowConstants shadows{};
				shadows.Counts.x = 1;
				shadows.CascadeViewProjection[0] = cascade.ViewProjection;
				shadows.CascadeSplits.x = cascade.SplitFar;
				shadows.CascadeBlendStarts.x = cascade.BlendStart;
				shadows.CascadeDepthRanges[0] = glm::vec4(cascade.LightNear, cascade.LightFar, cascade.PenumbraUvPerMetre, 0.05f);
				shadows.CascadeTexelWorldSizes.x = cascade.TexelWorldSize;
				const glm::vec4 receiver(0, 20, -20, 0); // PCSS probe at world z=-20, view distance 20
				MeshDrawItem item;
				item.Mesh = ShadowQuadHandle;
				item.World = glm::translate(glm::mat4(1), glm::vec3(0, 0, 20)); // behind the camera, towards the light
				item.World = glm::scale(item.World, glm::vec3(20));
				CHECK(setup.Render(std::span(&item, 1), *cascades).DrawCalls == 1);
				CHECK(ShadowPixel(setup.Probe(0), 128, 128).x > 0);
				CHECK(ShadowPixel(setup.Probe(3, 256, 256, shadows, receiver), 128, 128).x == 0);
				// Beyond even the extended near plane: the capable path pancakes, while the forced fallback clips.
				// Check the actual receiver as well as depth, so a cleared/stale map cannot masquerade as coverage.
				item.World[3][2] = 10000;
				if (gpu.GetDevice().GetInfo().DepthClamp)
				{
					CHECK(setup.Render(std::span(&item, 1), *cascades).DrawCalls == 1);
					CHECK(ShadowPixel(setup.Probe(0), 128, 128).x == 1);
					CHECK(ShadowPixel(setup.Probe(3, 256, 256, shadows, receiver), 128, 128).x == 0);
				}
				else
				{
					CHECK(setup.Render(std::span(&item, 1), *cascades).DrawCalls == 0);
					CHECK(ShadowPixel(setup.Probe(0), 128, 128).x == 0);
					CHECK(ShadowPixel(setup.Probe(3, 256, 256, shadows, receiver), 128, 128).x == 1);
				}
				item.World[3][0] = 10000;
				CHECK(setup.Render(std::span(&item, 1), *cascades).DrawCalls == 0);
				CHECK(ShadowPixel(setup.Probe(3, 256, 256, shadows, receiver), 128, 128).x == 1);
			}
		}

		TEST_CASE("DebugViews: cascades show exact colors with the shadow blend and distance fade")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			ShadowSetup setup(gpu);
			ShadowConstants shadows{};
			shadows.Counts.x = 4;
			shadows.CascadeSplits = glm::vec4(10, 20, 30, 40);
			shadows.CascadeBlendStarts = glm::vec4(9, 19, 29, 39);
			const Image image = setup.Probe(2, 81, 1, shadows, glm::vec4(0.5f, 0, 0, 0));
			CHECK(ShadowPixel(image, 0, 0) == glm::vec4(0, 0, 0, 1));
			CHECK(ShadowPixel(image, 10, 0) == glm::vec4(1, 0, 0, 1));
			CHECK(ShadowPixel(image, 30, 0) == glm::vec4(0, 1, 0, 1));
			CHECK(ShadowPixel(image, 50, 0) == glm::vec4(0, 0, 1, 1));
			CHECK(ShadowPixel(image, 70, 0) == glm::vec4(1, 1, 0, 1));
			CHECK(ShadowPixel(image, 19, 0) == glm::vec4(0.5f, 0.5f, 0, 1));
			CHECK(ShadowPixel(image, 39, 0) == glm::vec4(0, 0.5f, 0.5f, 1));
			CHECK(ShadowPixel(image, 59, 0) == glm::vec4(0.5f, 0.5f, 0.5f, 1));
			CHECK(ShadowPixel(image, 79, 0) == glm::vec4(0.5f, 0.5f, 0, 1));
			CHECK(ShadowPixel(image, 80, 0) == glm::vec4(0, 0, 0, 1));
			shadows.Counts.x = 0;
			CHECK(ShadowPixel(setup.Probe(2, 1, 1, shadows, glm::vec4(0, 5, 0, 0)), 0, 0) == glm::vec4(0, 0, 0, 1));
		}

		TEST_CASE("Shadows: alpha texture and UV transform match caster coverage")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			ShadowSetup setup(gpu);
			auto texture = CreateRef<TextureData>();
			texture->Width = texture->Height = 2;
			texture->Mips = { { .Width = 2, .Height = 2, .Offset = 0, .Size = 16 } };
			texture->Pixels.assign(16, std::byte{ 255 });
			texture->Pixels[3] = texture->Pixels[11] = std::byte{ 0 };
			setup.GetAssets().Publish(ShadowTextureHandle, texture);
			MaterialData material;
			material.AlphaMode = AlphaMode::Mask;
			material.BaseColorMap = TypedAssetHandle<AssetType::Texture>(ShadowTextureHandle);
			setup.GetAssets().Publish(ShadowMaterialHandle, CreateRef<MaterialData>(material));
			MeshDrawItem item;
			item.Mesh = ShadowQuadHandle;
			item.World = glm::translate(glm::mat4(1), glm::vec3(0, 0, 0.5f));
			setup.Render(std::span(&item, 1), IdentityShadowCascade());
			const Image first = setup.Probe(0);
			CHECK(ShadowPixel(first, 64, 128).x == 0);
			CHECK(ShadowPixel(first, 192, 128).x == 0.5f);
			material.UVOffset.x = 0.5f;
			setup.GetAssets().Publish(ShadowMaterialHandle, CreateRef<MaterialData>(material));
			setup.Render(std::span(&item, 1), IdentityShadowCascade());
			const Image shifted = setup.Probe(0);
			CHECK(ShadowPixel(shifted, 64, 128).x == 0.5f);
			CHECK(ShadowPixel(shifted, 192, 128).x == 0);
		}

		TEST_CASE("Shadows: PCSS uses reverse depth and preserves contact equality with deterministic softness")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			ShadowSetup setup(gpu);
			MeshDrawItem item;
			item.Mesh = ShadowQuadHandle;
			item.World = glm::translate(glm::mat4(1), glm::vec3(-0.5f, 0, 0.7f));
			item.World = glm::scale(item.World, glm::vec3(0.5f, 1, 1));
			setup.Render(std::span(&item, 1), IdentityShadowCascade());
			ShadowConstants shadows{};
			shadows.Counts.x = 1;
			shadows.CascadeViewProjection[0] = glm::mat4(1);
			shadows.CascadeSplits.x = 100;
			shadows.CascadeBlendStarts.x = 90;
			shadows.CascadeDepthRanges[0] = glm::vec4(0, 100, 0, 0.15f);
			shadows.CascadeTexelWorldSizes.x = 2.0f / 256;
			const Image hard = setup.Probe(3, 256, 64, shadows, glm::vec4(0, 50, 0.5f, 0));
			CHECK(ShadowPixel(hard, 64, 32).x == 0);
			CHECK(ShadowPixel(hard, 192, 32).x == 1);
			const Image contact = setup.Probe(3, 256, 64, shadows, glm::vec4(0, 50, 0.7f, 0));
			CHECK(ShadowPixel(contact, 64, 32).x == 1); // equality must pass GreaterOrEqual
			shadows.CascadeDepthRanges[0].z = 0.005f;
			const Image soft = setup.Probe(3, 256, 64, shadows, glm::vec4(0, 50, 0.5f, 0));
			const Image repeat = setup.Probe(3, 256, 64, shadows, glm::vec4(0, 50, 0.5f, 0));
			CHECK(soft.Pixels == repeat.Pixels);
			uint32_t hardTransition = 0;
			uint32_t softTransition = 0;
			for (uint32_t x = 80; x < 176; ++x)
			{
				const float first = ShadowPixel(hard, x, 32).x;
				const float second = ShadowPixel(soft, x, 32).x;
				hardTransition += first > 0.02f && first < 0.98f ? 1U : 0U;
				softTransition += second > 0.02f && second < 0.98f ? 1U : 0U;
			}
			CHECK(hardTransition < 5);
			CHECK(softTransition > hardTransition + 10);
			const Image faded = setup.Probe(3, 256, 64, shadows, glm::vec4(0, 95, 0.5f, 0));
			CHECK(ShadowPixel(faded, 64, 32).x == doctest::Approx(0.5));
		}

		TEST_CASE("SpotShadowAtlas: recorded tiles have independent guarded depth and PCSS sampling" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			ShadowSetup setup(gpu);
			CameraData camera;
			camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60, 10, 0.1f, 100, 256, 256);
			std::array<LightData, 2> lights{};
			for (uint32_t index = 0; index < 2; ++index)
			{
				lights[index].Type = RenderLightType::Spot;
				lights[index].CastShadows = true;
				lights[index].InnerConeAngle = 30;
				lights[index].OuterConeAngle = 45;
				lights[index].Entity = UUID(index + 1);
			}
			lights[1].Position.x = 100;
			auto atlas = AllocateSpotShadowAtlas(lights, std::array<uint32_t, 2>{ 0, 1 }, camera);
			REQUIRE(atlas);
			MeshDrawItem item;
			item.Mesh = ShadowQuadHandle;
			item.World = glm::translate(glm::mat4(1), glm::vec3(0, 0, -2));
			const auto result = setup.Render(std::span(&item, 1), {}, &*atlas);
			CHECK(result.DrawCalls == 1);
			REQUIRE(setup.GetStats().Passes.size() == 1);
			CHECK(setup.GetStats().Passes[0].Name == "SpotShadows");
			const Image depth = setup.Probe(1, 2048, 1024);
			CHECK(ShadowPixel(depth, 512, 512).x > 0);
			CHECK(ShadowPixel(depth, 1536, 512).x == 0);
			CHECK(ShadowPixel(depth, 0, 512).x == 0);
			CHECK(ShadowPixel(depth, 1023, 512).x == 0);
			ShadowConstants shadows{};
			shadows.Counts.y = 2;
			for (uint32_t index = 0; index < 2; ++index)
			{
				const SpotShadowTile& tile = atlas->Tiles[index];
				shadows.Spots[index].ViewProjection = tile.ViewProjection;
				shadows.Spots[index].UvScaleBias = tile.UvScaleBias;
				shadows.Spots[index].DepthSoftness = glm::vec4(tile.NearClip, tile.FarClip, 0.05f, 0.02f);
				shadows.Spots[index].Indices.x = tile.LightIndex;
			}
			const Image visibility = setup.Probe(4, 128, 128, shadows, glm::vec4(0, 0, 3, 0), 0);
			CHECK(ShadowPixel(visibility, 64, 64).x == 0);
			CHECK(ShadowPixel(visibility, 3, 64).x == 1);
			shadows.Spots[0].DepthSoftness.z = 0.5f;
			const Image soft = setup.Probe(4, 128, 128, shadows, glm::vec4(0, 0, 3, 0), 0);
			uint32_t narrowTransition = 0;
			uint32_t wideTransition = 0;
			for (uint32_t x = 12; x < 116; ++x)
			{
				const float narrow = ShadowPixel(visibility, x, 64).x;
				const float wide = ShadowPixel(soft, x, 64).x;
				narrowTransition += narrow > 0.02f && narrow < 0.98f ? 1U : 0U;
				wideTransition += wide > 0.02f && wide < 0.98f ? 1U : 0U;
			}
			CHECK(wideTransition > narrowTransition + 3);
			// Cover tile 0 completely while tile 1 remains clear. Even a wide filter at their shared edge must stay dark.
			item.World = glm::scale(item.World, glm::vec3(10));
			setup.Render(std::span(&item, 1), {}, &*atlas);
			shadows.Spots[0].DepthSoftness.z = 2;
			shadows.Spots[0].DepthSoftness.w = 0.1f;
			const Image guarded = setup.Probe(4, 128, 128, shadows, glm::vec4(0, 0, 3, 0), 0);
			CHECK(ShadowPixel(guarded, 127, 64).x == 0);
			CHECK(ShadowPixel(guarded, 0, 64).x == 0);
			const Image unallocated = setup.Probe(4, 1, 1, shadows, glm::vec4(0, 0, 3, 0), 99);
			CHECK(ShadowPixel(unallocated, 0, 0).x == 1);
		}

		TEST_CASE("Shadows: bias changes depth and disabled work does not retain stale coverage")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			ShadowSetup setup(gpu);
			MeshDrawItem item;
			item.Mesh = ShadowQuadHandle;
			item.World = glm::translate(glm::mat4(1), glm::vec3(0, 0, 0.5f));
			setup.Render(std::span(&item, 1), IdentityShadowCascade());
			const float unbiased = ShadowPixel(setup.Probe(0), 128, 128).x;
			setup.Render(std::span(&item, 1), IdentityShadowCascade(), nullptr, 2);
			const float biased = ShadowPixel(setup.Probe(0), 128, 128).x;
			CHECK(biased < unbiased);
			CHECK(biased == doctest::Approx(unbiased - 4.0f / 256));
			item.CastShadows = false;
			CHECK(setup.Render(std::span(&item, 1), IdentityShadowCascade()).DrawCalls == 0);
			CHECK(ShadowPixel(setup.Probe(0), 128, 128).x == 0);
			CHECK(setup.Render({}, {}).DrawCalls == 0);
			CHECK(setup.GetStats().Passes.empty());
		}
	}

}
