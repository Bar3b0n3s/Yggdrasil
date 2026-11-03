#include "TestsPCH.h"

#include "Engine/Renderer/SkinningPass.h"

#include "Engine/Asset/MeshDeform.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderStats.h"
#include "Support/BufferReadback.h"
#include "Support/DeviceLossTest.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// The Skinning pass of the Deform stage (M23, C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md decision 27, stream
// C): the GPU deformation equals the CPU reference (Roadmap M23: "the deform equals the CPU reference"), keeps the previous
// positions, draws through the 24 mesh pipelines with each item's own vertex stream, resolves missing morph weights to the
// targets' defaults, and recovers from a device loss. A deformed item's GPU state is keyed by its Entity and Mesh, so each
// skinned item here has an entity of its own, as render extraction gives it. Skipped until stream C implements it.

namespace Engine {

	namespace {

		constexpr AssetHandle StripHandle(0x5c10000000000001);
		constexpr AssetHandle StripMaterial(0x5c10000000000002);
		constexpr AssetHandle DefaultedStripHandle(0x5c10000000000003);

		// A strip of two quads along +Y facing +Z, skinned to two joints (the bottom row to joint 0, the top to joint 1, the
		// middle half to each), with one morph target that pushes the top row 0.5 along +Z.
		Ref<MeshData> MakeStrip()
		{
			Ref<MeshData> mesh = CreateRef<MeshData>();
			for (int row = 0; row < 3; ++row)
			{
				for (int column = 0; column < 2; ++column)
				{
					const glm::vec3 position(static_cast<float>(column) - 0.5f, static_cast<float>(row) - 1.0f, 0.0f);
					mesh->Vertices.push_back({ .Position = position, .Normal = { 0.0f, 0.0f, 1.0f }, .Tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .TexCoord = { static_cast<float>(column), static_cast<float>(row) / 2.0f } });
					mesh->Bounds.Extend(position);
					const float top = static_cast<float>(row) / 2.0f;
					mesh->Skin.push_back({ .Joints = { 0, 1, 0, 0 }, .Weights = { 1.0f - top, top, 0.0f, 0.0f } });
				}
			}
			mesh->SkinJointCount = 2;
			mesh->Indices = { 0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4 };
			mesh->Submeshes.push_back({ .IndexOffset = 0, .IndexCount = 12, .MaterialSlot = 0, .Bounds = mesh->Bounds });
			mesh->Slots.push_back({ .Name = "Default" });
			MeshMorphTarget push{ .Name = "Push" };
			for (size_t index = 0; index < mesh->Vertices.size(); ++index)
				push.Deltas.push_back({ .Position = { 0.0f, 0.0f, index >= 4 ? 0.5f : 0.0f } });
			mesh->MorphTargets.push_back(push);
			return mesh;
		}

		// The strip of MakeStrip whose morph target pushes at the default weight 0.6 (glTF's mesh weights).
		Ref<MeshData> MakeDefaultedStrip()
		{
			Ref<MeshData> mesh = MakeStrip();
			mesh->MorphTargets[0].DefaultWeight = 0.6f;
			return mesh;
		}

		// Joint 1 turned 90 degrees about +X around (0, 0, 0) and moved 0.25 along +X.
		std::vector<glm::mat4> MakePalette(float shift)
		{
			const glm::mat4 turn(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
			return { glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(shift, 0.0f, 0.0f)) * turn };
		}

		bool Near(const glm::vec3& a, const glm::vec3& b, float tolerance)
		{
			return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance && std::abs(a.z - b.z) <= tolerance;
		}

	}

	TEST_SUITE(Test::GpuSuite)
	{
		// The deformed vertices the pass writes equal DeformMeshVertices within 1e-4 per component (the GPU's fused
		// multiply-adds and square roots), the previous positions of a first deformation are its new positions, and those of
		// the next deformation are the first one's.
		TEST_CASE("SkinningPass: the deform equals the CPU reference, and the previous positions hold the last deformation" * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				const Ref<MeshData> strip = MakeStrip();
				setup.Assets().Publish(StripHandle, strip);
				const GpuMesh& mesh = setup.Cache().GetMesh(StripHandle);
				REQUIRE_FALSE(mesh.IsPlaceholder);
				REQUIRE(mesh.SkinBuffer);
				REQUIRE(mesh.MorphBuffer);
				CHECK(mesh.SkinJointCount == 2);
				CHECK(mesh.MorphTargetCount == 1);

				Result<Scope<SkinningPass>> pass = SkinningPass::Create(device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				const uint32_t vertexCount = static_cast<uint32_t>(strip->Vertices.size());
				Result<nvrhi::BufferHandle> vertices = device.CreateBuffer(SkinningPass::GetVerticesDesc(vertexCount));
				Result<nvrhi::BufferHandle> previous = device.CreateBuffer(SkinningPass::GetPreviousPositionsDesc(vertexCount));
				Result<nvrhi::BufferHandle> palette = device.CreateBuffer(SkinningPass::GetPaletteDesc(2));
				Result<nvrhi::BufferHandle> weights = device.CreateBuffer(SkinningPass::GetMorphWeightsDesc());
				REQUIRE(vertices.has_value());
				REQUIRE(previous.has_value());
				REQUIRE(palette.has_value());
				REQUIRE(weights.has_value());
				PassBindingCache bindings;
				RenderStats stats;
				RenderRecordingContext recording(stats, []()
				{
					return 0.0;
				});
				const std::vector<float> morphWeights = { 1.0f };
				const auto deform = [&](const std::vector<glm::mat4>& joints, bool first)
				{
					Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
					REQUIRE(list.has_value());
					(*list)->open();
					const SkinningPassInputs inputs{ .SourceVertices = mesh.VertexBuffer, .Skin = mesh.SkinBuffer, .Morphs = mesh.MorphBuffer, .Palette = joints, .MorphWeights = morphWeights, .PaletteBuffer = *palette, .MorphWeightBuffer = *weights, .Vertices = *vertices, .PreviousPositions = *previous, .VertexCount = vertexCount, .SkinJointCount = 2, .MorphTargetCount = 1, .FirstDeform = first };
					const Status recorded = (*pass)->Record(**list, recording, bindings, inputs);
					(*list)->close();
					device.ExecuteCommandList(**list);
					device.WaitForIdle();
					REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
				};
				const auto readVertices = [&]()
				{
					const Result<std::vector<std::byte>> bytes = Test::ReadBufferBytes(device, **vertices, vertexCount * sizeof(MeshVertex));
					REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
					std::vector<MeshVertex> read(vertexCount);
					std::memcpy(read.data(), bytes->data(), bytes->size());
					return read;
				};
				const auto readPrevious = [&]()
				{
					const Result<std::vector<std::byte>> bytes = Test::ReadBufferBytes(device, **previous, vertexCount * sizeof(glm::vec4));
					REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
					std::vector<glm::vec4> read(vertexCount);
					std::memcpy(read.data(), bytes->data(), bytes->size());
					return read;
				};

				const std::vector<glm::mat4> firstPalette = MakePalette(0.0f);
				deform(firstPalette, true);
				const Result<std::vector<MeshVertex>> reference = DeformMeshVertices(*strip, firstPalette, morphWeights);
				REQUIRE(reference.has_value());
				const std::vector<MeshVertex> gpuVertices = readVertices();
				const std::vector<glm::vec4> firstPrevious = readPrevious();
				for (uint32_t index = 0; index < vertexCount; ++index)
				{
					CAPTURE(index);
					CHECK(Near(gpuVertices[index].Position, (*reference)[index].Position, 1e-4f));
					CHECK(Near(gpuVertices[index].Normal, (*reference)[index].Normal, 1e-4f));
					CHECK(Near(glm::vec3(gpuVertices[index].Tangent), glm::vec3((*reference)[index].Tangent), 1e-4f));
					CHECK(gpuVertices[index].Tangent.w == (*reference)[index].Tangent.w);
					CHECK(gpuVertices[index].TexCoord == (*reference)[index].TexCoord);
					CHECK(Near(glm::vec3(firstPrevious[index]), gpuVertices[index].Position, 0.0f));
				}

				deform(MakePalette(0.25f), false);
				const std::vector<glm::vec4> secondPrevious = readPrevious();
				for (uint32_t index = 0; index < vertexCount; ++index)
					CHECK(Near(glm::vec3(secondPrevious[index]), gpuVertices[index].Position, 0.0f));
				CHECK(setup.Renderer().GetWidth() == 64);
			}
			device.RunGarbageCollection();
		}

		// The renderer deforms a skinned item before its passes draw it: turning joint 1 by 90 degrees folds the strip's top
		// half towards the camera, so the pixels it covered at rest are cleared; each deformed item (its own entity, as render
		// extraction gives it) has its own vertex stream, so two skinned items of one mesh and material are never one
		// instanced draw: two forward draws where the two items at rest batch into one; and the startup pipelines stay the
		// declared ones (24 mesh pipelines).
		TEST_CASE("SceneRenderer: a skinned item renders deformed by its palette, each in an instanced draw of its own" * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(StripHandle, MakeStrip());
			setup.Assets().Publish(StripMaterial, CreateRef<MaterialData>());
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Lights = { LightData{ .Type = RenderLightType::Directional, .Intensity = 2.0f } };
			snapshot.Meshes = { MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(-0.75f, 0.0f, -4.0f)), .Mesh = StripHandle, .Materials = { StripMaterial }, .Entity = UUID(1) },
				MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.75f, 0.0f, -4.0f)), .Mesh = StripHandle, .Materials = { StripMaterial }, .Entity = UUID(2) } };
			const Image rest = setup.Render(snapshot);
			const RenderPassStats* restForward = Test::SceneRendererIntegrationFixture::FindPass(setup.Renderer().GetRenderStats(), "ForwardOpaque");
			REQUIRE(restForward != nullptr);
			CHECK(restForward->DrawCalls == 1);
			for (MeshDrawItem& item : snapshot.Meshes)
				item.JointPalette = MakePalette(0.0f);
			snapshot.FrameIndex += 1;
			const Image bent = setup.Render(snapshot);
			// The top row's pixels above the strip's centre were lit at rest and are background once folded.
			CHECK(Test::SceneRendererIntegrationFixture::Channel(rest, 20, 12) > 0);
			CHECK(Test::SceneRendererIntegrationFixture::Channel(bent, 20, 12) == 0);
			const RenderStats& stats = setup.Renderer().GetRenderStats();
			const RenderPassStats* skinning = Test::SceneRendererIntegrationFixture::FindPass(stats, "Skinning");
			REQUIRE(skinning != nullptr);
			CHECK(skinning->DrawCalls == 2);
			const RenderPassStats* forward = Test::SceneRendererIntegrationFixture::FindPass(stats, "ForwardOpaque");
			REQUIRE(forward != nullptr);
			CHECK(forward->DrawCalls == 2);
			CHECK(SceneRendererPipelines::MeshPipelineCount == 24);
		}

		// The deformed items' vertices, palettes, morph weights and previous positions are each renderer's (the placement
		// rule's place 2), the Skinning pipeline the pipeline set's (place 1) and the meshes' skin and morph buffers the
		// cache's (place 3), so a loss with two skinned items recovers byte-identically.
		TEST_CASE("SceneRenderer: a device loss recreates the deformed vertices, palettes and previous positions and renders the same image"
			* doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(StripHandle, MakeStrip());
			setup.Assets().Publish(StripMaterial, CreateRef<MaterialData>());
			GraphicsDevice& device = gpu.GetDevice();
			uint64_t frame = 30;
			Test::DeviceLossTest test;
			test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
			test.Use = [&device, &setup, &frame]() -> Result<std::vector<std::byte>>
			{
				RenderSnapshot snapshot = setup.Snapshot();
				snapshot.FrameIndex = ++frame;
				snapshot.Meshes = { MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(-0.75f, 0.0f, -4.0f)), .Mesh = StripHandle, .Materials = { StripMaterial }, .Entity = UUID(1), .JointPalette = MakePalette(0.0f), .MorphWeights = { 1.0f } },
					MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.75f, 0.0f, -4.0f)), .Mesh = StripHandle, .Materials = { StripMaterial }, .Entity = UUID(2), .JointPalette = MakePalette(0.25f) } };
				// Renders into a command list of its own and reads the image back, returning every error, so it also runs on the
				// lost device.
				ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
				list->open();
				const Status rendered = setup.Renderer().Render(*list, snapshot);
				list->close();
				setup.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(*list));
				ENGINE_TRY(rendered);
				nvrhi::ITexture* image = setup.Renderer().GetFinalTexture();
				if (image == nullptr)
					return MakeError(ErrorCode::InvalidState, "the renderer has no final texture");
				Readback readback(device);
				ENGINE_TRY_ASSIGN(const Image pixels, readback.ReadTexture(*image));
				return pixels.Pixels;
			};
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			const RenderStats& stats = setup.Renderer().GetRenderStats();
			CHECK(Test::SceneRendererIntegrationFixture::FindPass(stats, "Skinning") != nullptr);
		}

		// One rule for a missing morph weight everywhere (Asset/MeshDeform.h's ResolveMorphWeights): an item whose mesh's
		// target has the default weight 0.6 draws pushed by it with an empty MorphWeights (a SkinnedMesh added by hand) and
		// without a SkinnedMesh's weights at all, exactly as with the weight 0.6 given, and unlike the weight 0.
		TEST_CASE("SceneRenderer: a morphed item without weights draws at its targets' default weights" * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(DefaultedStripHandle, MakeDefaultedStrip());
			setup.Assets().Publish(StripMaterial, CreateRef<MaterialData>());
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Lights = { LightData{ .Type = RenderLightType::Directional, .Intensity = 2.0f } };
			const auto render = [&setup, &snapshot](std::vector<float> weights)
			{
				snapshot.FrameIndex += 1;
				snapshot.Meshes = { MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -4.0f)) * glm::rotate(glm::mat4(1.0f), 1.0f, glm::vec3(0.0f, 1.0f, 0.0f)),
					.Mesh = DefaultedStripHandle,
					.Materials = { StripMaterial },
					.Entity = UUID(1),
					.MorphWeights = std::move(weights) } };
				return setup.Render(snapshot);
			};
			const Image defaulted = render({});
			CHECK(Test::SceneRendererIntegrationFixture::FindPass(setup.Renderer().GetRenderStats(), "Skinning") != nullptr);
			const Image given = render({ 0.6f });
			const Image flat = render({ 0.0f });
			CHECK(defaulted.Pixels == given.Pixels);
			CHECK(defaulted.Pixels != flat.Pixels);
		}

		// The bind-pose warning of a palette shorter than its mesh's skin (RenderSkinMismatchCode) is once per renderer: a
		// device loss's release replaces the renderer's state but keeps SkinMismatchLogged (SceneRendererRecovery.cpp's kept
		// members), so the recovered renderer, still drawing the short palette, does not warn again.
		TEST_CASE("SceneRenderer: the skin mismatch warning stays once per renderer across a release and recreation" * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(StripHandle, MakeStrip());
			setup.Assets().Publish(StripMaterial, CreateRef<MaterialData>());
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Meshes = { MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -4.0f)), .Mesh = StripHandle, .Materials = { StripMaterial }, .Entity = UUID(1), .JointPalette = { glm::mat4(1.0f) } } };
			const Test::ExpectLog warning(LogLevel::Warn, std::string(RenderSkinMismatchCode));
			const Image before = setup.Render(snapshot);
			CHECK(warning.GetMatchCount() == 1);
			++snapshot.FrameIndex;
			static_cast<void>(setup.Render(snapshot));
			CHECK(warning.GetMatchCount() == 1);

			setup.Renderer().ReleaseGpuObjects();
			const Status recreated = setup.Renderer().RecreateGpuObjects();
			REQUIRE_MESSAGE(recreated.has_value(), recreated.error().ToString());
			++snapshot.FrameIndex;
			const Image after = setup.Render(snapshot);
			CHECK(warning.GetMatchCount() == 1);
			CHECK(after.Pixels == before.Pixels);
		}
	}

}
