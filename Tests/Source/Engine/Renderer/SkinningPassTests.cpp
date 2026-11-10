#include "TestsPCH.h"

#include "Engine/Renderer/SkinningPass.h"

#include "Engine/Asset/MeshDeform.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderStats.h"
#include "Support/BufferCreationFails.h"
#include "Support/BufferReadback.h"
#include "Support/DeviceLossTest.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Skinning pass of the Deform stage (M23, C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md decision 27, stream
// C): the GPU deformation equals the CPU reference (Roadmap M23: "the deform equals the CPU reference"), keeps the previous
// positions, draws through the 24 mesh pipelines with each item's own vertex stream, resolves missing morph weights to the
// targets' defaults, and recovers from a device loss. A deformed item's GPU state is keyed by its Entity and Mesh, so each
// skinned item here has an entity of its own, as render extraction gives it.

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
		TEST_CASE("SkinningPass: the deform equals the CPU reference, and the previous positions hold the last deformation")
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
					const std::vector<Status> recorded = (*pass)->Record(**list, recording, bindings, std::span(&inputs, 1));
					(*list)->close();
					device.ExecuteCommandList(**list);
					device.WaitForIdle();
					REQUIRE(recorded.size() == 1);
					REQUIRE_MESSAGE(recorded[0].has_value(), recorded[0].error().ToString());
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

		// W23.3's review: Record's per-item rules (SkinningPass.h). Of {valid, a vertex count of 0, valid}, the two valid
		// items are recorded in the one pass (the Skinning row counts two dispatches, each output equals the CPU reference)
		// and the invalid one is reported, recording nothing of it; a Record of invalid items only begins no pass scope (the
		// recording context never reads its clock) and adds no Skinning row.
		TEST_CASE("SkinningPass: Record records every valid item and reports each invalid one, and invalid items alone record no pass")
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
				Result<Scope<SkinningPass>> pass = SkinningPass::Create(device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				const uint32_t vertexCount = static_cast<uint32_t>(strip->Vertices.size());

				// The buffers of one item.
				struct ItemBuffers
				{
					nvrhi::BufferHandle Vertices{};
					nvrhi::BufferHandle Previous{};
					nvrhi::BufferHandle Palette{};
					nvrhi::BufferHandle Weights{};
				};
				const auto createItem = [&device, vertexCount]()
				{
					ItemBuffers buffers;
					Result<nvrhi::BufferHandle> vertices = device.CreateBuffer(SkinningPass::GetVerticesDesc(vertexCount));
					Result<nvrhi::BufferHandle> previous = device.CreateBuffer(SkinningPass::GetPreviousPositionsDesc(vertexCount));
					Result<nvrhi::BufferHandle> palette = device.CreateBuffer(SkinningPass::GetPaletteDesc(2));
					Result<nvrhi::BufferHandle> weights = device.CreateBuffer(SkinningPass::GetMorphWeightsDesc());
					REQUIRE(vertices.has_value());
					REQUIRE(previous.has_value());
					REQUIRE(palette.has_value());
					REQUIRE(weights.has_value());
					buffers.Vertices = *vertices;
					buffers.Previous = *previous;
					buffers.Palette = *palette;
					buffers.Weights = *weights;
					return buffers;
				};
				const std::array<ItemBuffers, 3> buffers = { createItem(), createItem(), createItem() };
				const std::array<std::vector<glm::mat4>, 3> palettes = { MakePalette(0.0f), MakePalette(0.1f), MakePalette(0.25f) };
				const std::vector<float> morphWeights = { 1.0f };
				std::vector<SkinningPassInputs> items;
				for (size_t index = 0; index < buffers.size(); ++index)
				{
					items.push_back({ .SourceVertices = mesh.VertexBuffer, .Skin = mesh.SkinBuffer, .Morphs = mesh.MorphBuffer, .Palette = palettes[index], .MorphWeights = morphWeights, .PaletteBuffer = buffers[index].Palette, .MorphWeightBuffer = buffers[index].Weights, .Vertices = buffers[index].Vertices, .PreviousPositions = buffers[index].Previous, .VertexCount = vertexCount, .SkinJointCount = 2, .MorphTargetCount = 1, .FirstDeform = true });
				}
				items[1].VertexCount = 0;

				PassBindingCache bindings;
				RenderStats stats;
				uint32_t clockReads = 0;
				const auto record = [&](std::span<const SkinningPassInputs> recorded, RenderStats& into)
				{
					RenderRecordingContext recording(into, [&clockReads]()
					{
						++clockReads;
						return 0.0;
					});
					Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
					REQUIRE(list.has_value());
					(*list)->open();
					std::vector<Status> results = (*pass)->Record(**list, recording, bindings, recorded);
					(*list)->close();
					device.ExecuteCommandList(**list);
					device.WaitForIdle();
					return results;
				};
				const std::vector<Status> results = record(items, stats);
				REQUIRE(results.size() == 3);
				CHECK(results[0].has_value());
				REQUIRE_FALSE(results[1].has_value());
				CHECK(results[1].error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(results[2].has_value());
				const RenderPassStats* skinning = Test::SceneRendererIntegrationFixture::FindPass(stats, "Skinning");
				REQUIRE(skinning != nullptr);
				CHECK(skinning->Dispatches == 2);
				for (const size_t index : { size_t{ 0 }, size_t{ 2 } })
				{
					CAPTURE(index);
					const Result<std::vector<std::byte>> bytes = Test::ReadBufferBytes(device, *buffers[index].Vertices, vertexCount * sizeof(MeshVertex));
					REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
					std::vector<MeshVertex> read(vertexCount);
					std::memcpy(read.data(), bytes->data(), bytes->size());
					const Result<std::vector<MeshVertex>> reference = DeformMeshVertices(*strip, palettes[index], morphWeights);
					REQUIRE(reference.has_value());
					for (uint32_t vertex = 0; vertex < vertexCount; ++vertex)
						CHECK(Near(read[vertex].Position, (*reference)[vertex].Position, 1e-4f));
				}

				// Invalid items alone: a vertex count of 0, and a palette shorter than the skin's joints.
				std::vector<SkinningPassInputs> invalid = { items[1], items[0] };
				invalid[1].Palette = std::span<const glm::mat4>(palettes[0]).first(1);
				RenderStats invalidStats;
				clockReads = 0;
				const std::vector<Status> refused = record(invalid, invalidStats);
				REQUIRE(refused.size() == 2);
				CHECK_FALSE(refused[0].has_value());
				CHECK_FALSE(refused[1].has_value());
				CHECK(Test::SceneRendererIntegrationFixture::FindPass(invalidStats, "Skinning") == nullptr);
				CHECK(clockReads == 0);
			}
			device.RunGarbageCollection();
		}

		// W23.3's review: an item whose joint palette or morph weights are not finite is refused by the Prepare stage
		// (ResolveItemDeform), which skips it in every pass that would draw it, the camera's, the shadow casters' and the
		// selection mask's, while the other items deform and draw as they would alone; the render returns the error naming
		// the item's entity.
		TEST_CASE("SceneRenderer: a deformed item whose palette or morph weights are not finite is drawn in no pass, and the others are")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(StripHandle, MakeStrip());
			setup.Assets().Publish(StripMaterial, CreateRef<MaterialData>());
			GraphicsDevice& device = gpu.GetDevice();
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Lights = { LightData{ .Type = RenderLightType::Directional, .Intensity = 2.0f, .Direction = glm::normalize(glm::vec3(0.0f, -1.0f, -1.0f)), .CastShadows = true } };
			snapshot.Flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Selection;
			snapshot.SelectedEntities = { UUID(1), UUID(2) };
			const MeshDrawItem healthy{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(-0.75f, 0.0f, -4.0f)), .Mesh = StripHandle, .Materials = { StripMaterial }, .Entity = UUID(1), .JointPalette = MakePalette(0.0f) };
			// The healthy item alone: what it draws in each pass, and its image.
			snapshot.Meshes = { healthy };
			const Image alone = setup.Render(snapshot);
			const RenderStats aloneStats = setup.Renderer().GetRenderStats();
			const auto draws = [](const RenderStats& stats, std::string_view name)
			{
				const RenderPassStats* row = Test::SceneRendererIntegrationFixture::FindPass(stats, name);
				return row != nullptr ? row->DrawCalls : 0U;
			};
			REQUIRE(draws(aloneStats, "DirectionalShadows") > 0);
			REQUIRE(draws(aloneStats, "SelectionMask") > 0);

			MeshDrawItem nanPalette = healthy;
			nanPalette.World = glm::translate(glm::mat4(1.0f), glm::vec3(0.75f, 0.0f, -4.0f));
			nanPalette.Entity = UUID(2);
			nanPalette.JointPalette[1][3][0] = std::numeric_limits<float>::quiet_NaN();
			MeshDrawItem nanWeight = nanPalette;
			nanWeight.JointPalette = MakePalette(0.0f);
			nanWeight.MorphWeights = { std::numeric_limits<float>::infinity() };
			for (const MeshDrawItem& broken : { nanPalette, nanWeight })
			{
				snapshot.Meshes = { healthy, broken };
				++snapshot.FrameIndex;
				Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
				REQUIRE(list.has_value());
				(*list)->open();
				const Status rendered = setup.Renderer().Render(**list, snapshot);
				(*list)->close();
				setup.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(**list));
				REQUIRE_FALSE(rendered.has_value());
				CHECK(rendered.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(rendered.error().GetMessageText().contains(UUID(2).ToString()));
				const RenderStats stats = setup.Renderer().GetRenderStats();
				const RenderPassStats* skinning = Test::SceneRendererIntegrationFixture::FindPass(stats, "Skinning");
				REQUIRE(skinning != nullptr);
				CHECK(skinning->Dispatches == 1);
				for (const std::string_view name : { "ForwardOpaque", "DirectionalShadows", "SelectionMask" })
				{
					CAPTURE(name);
					CHECK(draws(stats, name) == draws(aloneStats, name));
				}
				CHECK(setup.Read().Pixels == alone.Pixels);
			}
		}

		// The renderer deforms a skinned item before its passes draw it: turning joint 1 by 90 degrees folds the strip's top
		// half towards the camera, so the pixels it covered at rest are cleared; each deformed item (its own entity, as render
		// extraction gives it) has its own vertex stream, so two skinned items of one mesh and material are never one
		// instanced draw: two forward draws where the two items at rest batch into one; and the startup pipelines stay the
		// declared ones (24 mesh pipelines).
		TEST_CASE("SceneRenderer: a skinned item renders deformed by its palette, each in an instanced draw of its own")
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
			const RenderStats restStats = setup.Renderer().GetRenderStats();
			const RenderPassStats* restForward = Test::SceneRendererIntegrationFixture::FindPass(restStats, "ForwardOpaque");
			REQUIRE(restForward != nullptr);
			CHECK(restForward->DrawCalls == 1);
			for (MeshDrawItem& item : snapshot.Meshes)
				item.JointPalette = MakePalette(0.0f);
			snapshot.FrameIndex += 1;
			const Image bent = setup.Render(snapshot);
			// The top row's pixels above the strip's centre were lit at rest and are background once folded (row 24 is half a
			// unit above the first strip's centre, whose top half spans rows 18 to 32).
			CHECK(Test::SceneRendererIntegrationFixture::Channel(rest, 20, 24) > 0);
			CHECK(Test::SceneRendererIntegrationFixture::Channel(bent, 20, 24) == 0);
			const RenderStats& stats = setup.Renderer().GetRenderStats();
			const RenderPassStats* skinning = Test::SceneRendererIntegrationFixture::FindPass(stats, "Skinning");
			REQUIRE(skinning != nullptr);
			CHECK(skinning->Dispatches == 2);
			const RenderPassStats* forward = Test::SceneRendererIntegrationFixture::FindPass(stats, "ForwardOpaque");
			REQUIRE(forward != nullptr);
			CHECK(forward->DrawCalls == 2);
			CHECK(SceneRendererPipelines::MeshPipelineCount == 24);
		}

		// The Skinning pass is one GPU profiler scope a frame, however many items it deforms (W23.3's integration): a scope per
		// dispatch would spend GpuProfiler's MaxScopesPerFrame on the deformed items and leave a frame's later passes untimed.
		// Three skinned items rendered until their frames retire: each retired frame's Deform stage holds exactly one Skinning
		// zone on the profiler's GPU timeline, and its stats row still counts the three dispatches.
		TEST_CASE("SceneRenderer: the Skinning pass is one profiler scope however many items it deforms")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			REQUIRE(Profiler::IsInitialized());
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(StripHandle, MakeStrip());
			setup.Assets().Publish(StripMaterial, CreateRef<MaterialData>());
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Lights = { LightData{ .Type = RenderLightType::Directional, .Intensity = 2.0f } };
			for (uint64_t index = 0; index < 3; ++index)
			{
				const float x = (static_cast<float>(index) - 1.0f) * 1.25f;
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, -5.0f)), .Mesh = StripHandle, .Materials = { StripMaterial }, .Entity = UUID(index + 1), .JointPalette = MakePalette(0.0f) });
			}
			const uint64_t since = Profiler::GetTimeNs();
			// The renderer collects a frame's timings when its timing slot comes round again.
			for (uint32_t frame = 0; frame <= gpu.GetDevice().GetFramesInFlight(); ++frame)
			{
				snapshot.FrameIndex = 200 + frame;
				setup.Submit(snapshot);
				gpu.GetDevice().WaitForIdle();
			}
			const RenderStats stats = setup.Renderer().GetRenderStats();
			const RenderPassStats* skinning = Test::SceneRendererIntegrationFixture::FindPass(stats, "Skinning");
			REQUIRE(skinning != nullptr);
			CHECK(skinning->Dispatches == 3);
			const std::vector<ProfileZone> zones = Profiler::CollectZones(since);
			const auto countGpuZones = [&zones](std::string_view name)
			{
				return std::count_if(zones.begin(), zones.end(), [name](const ProfileZone& zone)
				{
					return zone.ThreadIndex == Profiler::GpuThreadIndex && zone.Name == name;
				});
			};
			const auto deformStages = countGpuZones("Stage:Deform");
			CHECK(deformStages > 0);
			CHECK(countGpuZones("Skinning") == deformStages);
		}

		// The Deform stage deforms each deformed item one of whose draws survived culling once a render, whatever the passes
		// that draw it, the shadow casters' included: a skinned shadow caster outside the camera's view is deformed for its
		// shadow, and a selected skinned item in view once for its forward, shadow and selection-mask draws.
		TEST_CASE("SceneRenderer: a deformed item is deformed once for all its passes, an off-screen shadow caster's included")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(StripHandle, MakeStrip());
			setup.Assets().Publish(StripMaterial, CreateRef<MaterialData>());
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Lights = { LightData{ .Type = RenderLightType::Directional, .Intensity = 2.0f, .Direction = glm::normalize(glm::vec3(0.0f, -1.0f, -1.0f)), .CastShadows = true } };
			snapshot.Flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Selection;
			snapshot.SelectedEntities = { UUID(1) };
			snapshot.Meshes = { MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -4.0f)), .Mesh = StripHandle, .Materials = { StripMaterial }, .Entity = UUID(1), .JointPalette = MakePalette(0.0f) },
				MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(40.0f, 0.0f, -4.0f)), .Mesh = StripHandle, .Materials = { StripMaterial }, .Entity = UUID(2), .JointPalette = MakePalette(0.0f) } };
			static_cast<void>(setup.Render(snapshot));
			const RenderStats& stats = setup.Renderer().GetRenderStats();
			const RenderPassStats* skinning = Test::SceneRendererIntegrationFixture::FindPass(stats, "Skinning");
			REQUIRE(skinning != nullptr);
			CHECK(skinning->Dispatches == 2);
			const RenderPassStats* forward = Test::SceneRendererIntegrationFixture::FindPass(stats, "ForwardOpaque");
			REQUIRE(forward != nullptr);
			CHECK(forward->DrawCalls == 1);
			const RenderPassStats* mask = Test::SceneRendererIntegrationFixture::FindPass(stats, "SelectionMask");
			REQUIRE(mask != nullptr);
			CHECK(mask->DrawCalls == 1);

			// Without the shadow, the off-screen item is not deformed.
			snapshot.Lights[0].CastShadows = false;
			snapshot.FrameIndex += 1;
			static_cast<void>(setup.Render(snapshot));
			const RenderStats unshadowedStats = setup.Renderer().GetRenderStats();
			const RenderPassStats* unshadowed = Test::SceneRendererIntegrationFixture::FindPass(unshadowedStats, "Skinning");
			REQUIRE(unshadowed != nullptr);
			CHECK(unshadowed->Dispatches == 1);
		}

		// A deformed item whose buffers cannot be created (an out-of-memory request) is not drawn by that render, which draws
		// the rest and returns the error (the host trims after the frame, Architecture §8.14 item 7), and the next render
		// deforms and draws it.
		TEST_CASE("SceneRenderer: a deformed item whose buffers cannot be created is skipped, and the next render draws it")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(StripHandle, MakeStrip());
			setup.Assets().Publish(StripMaterial, CreateRef<MaterialData>());
			GraphicsDevice& device = gpu.GetDevice();
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Lights = { LightData{ .Type = RenderLightType::Directional, .Intensity = 2.0f } };
			snapshot.Meshes = { MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -4.0f)), .Mesh = StripHandle, .Materials = { StripMaterial }, .Entity = UUID(1) } };
			// At rest, which uploads the mesh: the strip's bottom half, which the palette keeps in place, is lit.
			const Image rest = setup.Render(snapshot);
			CHECK(Test::SceneRendererIntegrationFixture::Channel(rest, 32, 40) > 0);

			snapshot.Meshes[0].JointPalette = MakePalette(0.0f);
			snapshot.FrameIndex += 1;
			Status rendered;
			{
				// The deformed vertices are the only vertex buffers with storage views created by this render.
				const Test::BufferCreationFails fails([](const VkBufferCreateInfo& info)
				{
					return (info.usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) != 0 && (info.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0;
				});
				Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
				REQUIRE(list.has_value());
				(*list)->open();
				rendered = setup.Renderer().Render(**list, snapshot);
				(*list)->close();
				setup.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(**list));
				CHECK(fails.GetFailedCount() == 1);
			}
			REQUIRE_FALSE(rendered.has_value());
			CHECK(rendered.error().GetCode() == ErrorCode::Gpu);
			const Image skipped = setup.Read();
			CHECK(Test::SceneRendererIntegrationFixture::Channel(skipped, 32, 40) == 0);
			const RenderStats skippedStats = setup.Renderer().GetRenderStats();
			CHECK(Test::SceneRendererIntegrationFixture::FindPass(skippedStats, "Skinning") == nullptr);

			snapshot.FrameIndex += 1;
			const Image drawn = setup.Render(snapshot);
			CHECK(Test::SceneRendererIntegrationFixture::Channel(drawn, 32, 40) > 0);
			const RenderStats drawnStats = setup.Renderer().GetRenderStats();
			const RenderPassStats* skinning = Test::SceneRendererIntegrationFixture::FindPass(drawnStats, "Skinning");
			REQUIRE(skinning != nullptr);
			CHECK(skinning->Dispatches == 1);
		}

		// The deformed items' vertices, palettes, morph weights and previous positions are each renderer's (the placement
		// rule's place 2), the Skinning pipeline the pipeline set's (place 1) and the meshes' skin and morph buffers the
		// cache's (place 3), so a loss with two skinned items recovers byte-identically.
		TEST_CASE("SceneRenderer: a device loss recreates the deformed vertices, palettes and previous positions and renders the same image")
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
		TEST_CASE("SceneRenderer: a morphed item without weights draws at its targets' default weights")
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
		TEST_CASE("SceneRenderer: the skin mismatch warning stays once per renderer across a release and recreation")
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
