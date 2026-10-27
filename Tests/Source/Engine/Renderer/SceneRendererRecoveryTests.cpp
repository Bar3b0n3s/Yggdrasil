#include "TestsPCH.h"

#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/FrameLoop.h"
#include "Engine/App/VulkanErrorHandler.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Automation/Protocol/Dispatcher.h"
#include "Engine/Automation/Protocol/MethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/RingBufferSink.h"
#include "Engine/Graphics/CommandListRecording.h"
#include "Engine/Graphics/GpuDiagnostics.h"
#include "Engine/Graphics/GpuResourceTracker.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Renderer/DebugRenderer.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderPassDeclaration.h"
#include "Support/DeviceLossTest.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/ProtocolTestTypes.h"
#include "Support/TestOptions.h"
#include "Support/TimelineReportsDeviceLost.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A scene renderer's recovery from a device loss (M18, C18.3; Docs/Decisions/0029-m18-contract.md decisions 26 and 27):
// the placement rule's SceneRenderer::State, with its RenderTargetPool, through the shared loss test.

namespace Engine {

	namespace Test {

		// Renders `snapshot` with `renderer` into a command list of its own and reads the final image back, returning every
		// error instead of failing the test, so it also runs on a lost device.
		[[nodiscard]] static Result<std::vector<std::byte>> RenderAndRead(GraphicsDevice& device, SceneRenderer& renderer, const RenderSnapshot& snapshot)
		{
			ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
			list->open();
			const Status rendered = renderer.Render(*list, snapshot);
			list->close();
			renderer.OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(*list));
			ENGINE_TRY(rendered);
			nvrhi::ITexture* image = renderer.GetFinalTexture();
			if (image == nullptr)
				return MakeError(ErrorCode::InvalidState, "the renderer has no final texture");
			Readback readback(device);
			ENGINE_TRY_ASSIGN(const Image pixels, readback.ReadTexture(*image));
			return pixels.Pixels;
		}

		// RenderAndRead with `setup`'s renderer.
		[[nodiscard]] static Result<std::vector<std::byte>> RenderAndRead(GraphicsDevice& device, SceneRendererIntegrationFixture& setup,
			const RenderSnapshot& snapshot)
		{
			return RenderAndRead(device, setup.Renderer(), snapshot);
		}

		// The fixture's lit quad with every per-view object a render at the view's resolution creates besides the targets of
		// Create: picking's EntityId, the selection outline's mask, GTAO's targets, the shadow cascades of a shadow-casting
		// directional light, bloom and FXAA.
		[[nodiscard]] static RenderSnapshot MakeFullSnapshot(const SceneRendererIntegrationFixture& setup, uint64_t frameIndex)
		{
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.FrameIndex = frameIndex;
			snapshot.Flags = RenderViewFlags::Picking | RenderViewFlags::EditorOverlays | RenderViewFlags::Selection;
			snapshot.SelectedEntities = { UUID(900) };
			snapshot.Post.SsaoEnabled = true;
			snapshot.Post.BloomEnabled = true;
			snapshot.Post.FxaaEnabled = true;
			snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Directional,
				.Intensity = 2.0f,
				.Direction = glm::normalize(glm::vec3(0.0f, -1.0f, -1.0f)),
				.CastShadows = true });
			return snapshot;
		}

		// MakeFullSnapshot at a 20 x 20 scene resolution, upscaled 3 times into the 64 x 64 view with a letterbox: the
		// output stage's targets as well.
		[[nodiscard]] static RenderSnapshot MakeScaledSnapshot(const SceneRendererIntegrationFixture& setup, uint64_t frameIndex)
		{
			RenderSnapshot snapshot = MakeFullSnapshot(setup, frameIndex);
			snapshot.Output = RenderOutputSettings{ .SceneWidth = 20, .SceneHeight = 20 };
			snapshot.Camera.ViewportWidth = 20;
			snapshot.Camera.ViewportHeight = 20;
			snapshot.Camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 2.0f, 0.1f, 20.0f, 20, 20);
			return snapshot;
		}

		// Submits `list`, closed with a render the renderer released before its submission (a loss in the middle of a frame),
		// and waits for it: NVRHI's command buffer references its command list until the list is executed, so a closed list
		// that is never executed keeps itself and everything it recorded alive. Nothing collected garbage since the release, so
		// every object the list uses still exists (the device's tracker references even those that it only cleared), and the
		// renderer awaits no OnSubmitted for it.
		static void RetireUnsubmittedList(GraphicsDevice& device, nvrhi::ICommandList& list)
		{
			device.ExecuteCommandList(list);
			device.WaitForIdle();
		}

		namespace {

			// The tracker's live counts once the device is idle and its garbage collected, with their description for messages.
			struct LiveCounts
			{
				std::array<uint64_t, GpuResourceTypeCount> ByType{};
				uint64_t Total = 0;
				std::string Description{};
			};

			// M18's milestone review: a really lost device met in the middle of a render. In a render armed with `lose` (Render
			// below), the queue's timeline reports the device lost (TimelineReportsDeviceLost) from the moment the
			// GpuResourceCache loads `mesh`, inside the Prepare stage's "Prepare" pass and after Render's own timeline query,
			// until Render returns or unwinds: the next NVRHI call that asks the timeline what completed (an upload manager's
			// first write in a list) throws vk::DeviceLostError out of the recording. The timeline answers again before the throw
			// reaches the boundary that reports the loss: the Vulkan device is healthy, and a timeline that reports it lost sends
			// the report to query the fault (GraphicsDevice::DescribeDeviceFault), which validation refuses on a device that
			// Vulkan never lost. Main thread; not copyable.
			class MeshLoadLoss
			{
			public:
				// `assets` is a documented back-reference: the test's manager outlives the loss, which owns its load observer.
				MeshLoadLoss(InMemoryAssetManager& assets, AssetHandle mesh)
					: m_Assets(&assets), m_Mesh(mesh)
				{
					m_Assets->SetLoadObserver([this](AssetHandle handle)
					{
						if (m_IsArmed && !m_Lost.has_value() && handle == m_Mesh)
						{
							m_Lost.emplace();
							++m_LossCount;
						}
					});
				}

				~MeshLoadLoss() { m_Assets->SetLoadObserver({}); }

				MeshLoadLoss(const MeshLoadLoss&) = delete;
				MeshLoadLoss& operator=(const MeshLoadLoss&) = delete;

				// The renders armed with `lose` that met the loss: each one that reached the mesh's load in Render's Prepare stage.
				[[nodiscard]] uint32_t GetLossCount() const { return m_LossCount; }

				// `renderer`'s Render of `snapshot` into `list`; with `lose`, that render meets the loss (see the class comment).
				[[nodiscard]] Status Render(SceneRenderer& renderer, nvrhi::ICommandList& list, const RenderSnapshot& snapshot, bool lose)
				{
					struct EndLossOnExit
					{
						MeshLoadLoss* Loss = nullptr;

						~EndLossOnExit()
						{
							Loss->m_IsArmed = false;
							Loss->m_Lost.reset();
						}
					};
					m_IsArmed = lose;
					const EndLossOnExit endLoss{ .Loss = this };
					return renderer.Render(list, snapshot);
				}
			private:
				InMemoryAssetManager* m_Assets = nullptr;
				AssetHandle m_Mesh{};
				std::optional<TimelineReportsDeviceLost> m_Lost{};
				uint32_t m_LossCount = 0;
				bool m_IsArmed = false;
			};

			// The loss cases' scene over `context`'s device, created as an application creates its view: a GpuResourceCache, the
			// pipeline set and a 64 x 64 SceneRenderer, with an unlit snapshot of a quad (no post-processing, no light). The quad's
			// mesh has 96 KiB of vertices, past vkCmdUpdateBuffer's 64 KiB, so NVRHI writes them through its upload manager,
			// whose first write in a list asks the timeline what completed, under both API caps (a smaller mesh is written
			// inline, and a loss at its load would surface at a later upload that host image copy skips under API 1.4); the copies
			// of the first corner are never indexed. Not copyable or movable.
			struct LossQuadScene
			{
				explicit LossQuadScene(EngineContext& context)
					: Cache(*context.GetGraphicsDevice(), Assets)
				{
					auto mesh = CreateRef<MeshData>();
					mesh->Vertices = {
						{ .Position = { -1, -1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 0, 1 } },
						{ .Position = { 1, -1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 1, 1 } },
						{ .Position = { 1, 1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 1, 0 } },
						{ .Position = { -1, 1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 0, 0 } },
					};
					mesh->Vertices.resize(2048, mesh->Vertices.front());
					mesh->Indices = { 0, 1, 2, 0, 2, 3 };
					mesh->Bounds = { { -1, -1, 0 }, { 1, 1, 0 } };
					mesh->Submeshes = { { .IndexCount = 6, .Bounds = mesh->Bounds } };
					mesh->Slots = { {} };
					Assets.Publish(UUID(8910), mesh);
					Assets.Publish(UUID(8911), CreateRef<MaterialData>());
					GraphicsDevice& device = *context.GetGraphicsDevice();
					Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(device, *context.GetPipelineFactory());
					REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
					Pipelines = std::move(*pipelines);
					Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(device, *Pipelines, Cache, Assets, { 64, 64 });
					REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
					Renderer = std::move(*renderer);

					Snapshot.HasCamera = true;
					Snapshot.Camera.ViewportWidth = 64;
					Snapshot.Camera.ViewportHeight = 64;
					Snapshot.Camera.NearClip = 0.1f;
					Snapshot.Camera.FarClip = 20;
					Snapshot.Camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60, 2, 0.1f, 20, 64, 64);
					Snapshot.Camera.ClearColor = glm::vec3(0);
					Snapshot.Camera.ClearToSkybox = false;
					Snapshot.Post.BloomEnabled = false;
					Snapshot.Post.FxaaEnabled = false;
					Snapshot.Post.SsaoEnabled = false;
					Snapshot.Post.Tonemap = RenderTonemapper::Linear;
					Snapshot.Quality.ShadowMapSize = 256;
					Snapshot.Meshes = { { .World = glm::translate(glm::mat4(1), glm::vec3(0, 0, -3)), .Mesh = UUID(8910), .Materials = { UUID(8911) }, .Entity = UUID(900), .PickId = 1 } };
				}

				LossQuadScene(const LossQuadScene&) = delete;
				LossQuadScene& operator=(const LossQuadScene&) = delete;

				InMemoryAssetManager Assets{};
				GpuResourceCache Cache; // after Assets, which it reads
				Scope<SceneRendererPipelines> Pipelines{};
				Scope<SceneRenderer> Renderer{};
				RenderSnapshot Snapshot{};
			};

			// M18's milestone review: renders the scene's snapshot with its renderer in every frame's OnFrameRender, into a
			// command list of its own, as an application's frame does. Frame 0's render meets the loss at the mesh's load
			// (MeshLoadLoss): the mesh's first upload write throws vk::DeviceLostError out of NVRHI with the Prepare stage and its
			// pass open, which unwinds Render to the frame-boundary catch. The client recovers the device when the loop asks, as
			// Application does, and records each frame's render result.
			class StageLosingClient final : public IFrameLoopClient
			{
			public:
				StageLosingClient(EngineContext& context, LossQuadScene& scene)
					: m_Context(&context), m_Scene(&scene), m_Loss(scene.Assets, scene.Snapshot.Meshes.front().Mesh), m_Snapshot(scene.Snapshot)
				{
				}

				StageLosingClient(const StageLosingClient&) = delete;
				StageLosingClient& operator=(const StageLosingClient&) = delete;

				void OnFrameEvent(Event& /*event*/) override {}

				void OnFrameFixedStep(const SimStep& /*step*/) override {}

				void OnFrameUpdate(const FrameTime& /*frame*/) override {}

				void OnFrameRender(const FrameTime& frame) override
				{
					GraphicsDevice& device = *m_Context->GetGraphicsDevice();
					Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
					if (!list.has_value())
					{
						Rendered.push_back(list.error().ToString());
						return;
					}
					m_Snapshot.FrameIndex = frame.FrameIndex + 1;
					CommandListRecording recording(device, **list);
					const Status rendered = m_Loss.Render(*m_Scene->Renderer, **list, m_Snapshot, frame.FrameIndex == 0);
					m_Scene->Renderer->OnSubmitted(m_Snapshot.FrameIndex, recording.Execute());
					Rendered.push_back(rendered.has_value() ? std::format("frame {} rendered", frame.FrameIndex) : rendered.error().ToString());
				}

				void OnFrameDeviceLost() override
				{
					++Recoveries;
					Recovery = m_Context->RecoverGraphics();
				}
			public:
				std::vector<std::string> Rendered{};
				uint32_t Recoveries = 0;
				Status Recovery{};
			private:
				EngineContext* m_Context = nullptr; // documented back-references: the test's objects outlive the client
				LossQuadScene* m_Scene = nullptr;
				MeshLoadLoss m_Loss;
				RenderSnapshot m_Snapshot{};
			};

			// The debug lines of the second-render case: 1,500 segments, whose 3,000 vertices exceed vkCmdUpdateBuffer's 64 KiB,
			// so the EditorOverlay stage writes them through NVRHI's upload manager.
			constexpr uint32_t OverlayLineCount = 1500;
			static_assert(OverlayLineCount * 2 * sizeof(DebugLineVertex) > 65536, "the overlay's vertices must exceed vkCmdUpdateBuffer's limit");

			// test.render's params (none) and result: the frame index of the render it submitted.
			struct LossRenderParams
			{
			};

			struct LossRenderResult
			{
				uint32_t Frame = 0;
			};

			// The host of test.render, an automation method that renders a view as viewport.screenshot's capture does (registered
			// again as test.renderAgain, so the loss report tells the requests apart): the scene's renderer records into the one
			// command list made for it before any loss (a lost device creates none, so ViewportCapture keeps its own), through a
			// CommandListRecording executed after Render returns, which a loss thrown out of NVRHI retires instead. While
			// LoseDevice is set, every render meets the loss (MeshLoadLoss), as every render on a really lost device does. The
			// other host calls admit everything and record nothing. Not copyable or movable.
			class LossRenderHost final : public IMethodHost
			{
			public:
				// `scene` and `device` are documented back-references that outlive the host; `snapshot` is rendered with frame
				// indices after its own.
				LossRenderHost(GraphicsDevice& device, LossQuadScene& scene, RenderSnapshot snapshot, nvrhi::CommandListHandle list)
					: m_Device(&device), m_Scene(&scene), m_Loss(scene.Assets, scene.Snapshot.Meshes.front().Mesh), m_Snapshot(std::move(snapshot)), m_List(std::move(list))
				{
				}

				LossRenderHost(const LossRenderHost&) = delete;
				LossRenderHost& operator=(const LossRenderHost&) = delete;

				// test.render's work: renders the snapshot at its next frame index and returns that index, or Render's error.
				[[nodiscard]] Result<uint32_t> RenderView()
				{
					++m_Snapshot.FrameIndex;
					CommandListRecording recording(*m_Device, *m_List);
					const Status rendered = m_Loss.Render(*m_Scene->Renderer, *m_List, m_Snapshot, LoseDevice);
					m_Scene->Renderer->OnSubmitted(m_Snapshot.FrameIndex, recording.Execute());
					++ReturnedRenders;
					ENGINE_TRY(rendered);
					return static_cast<uint32_t>(m_Snapshot.FrameIndex);
				}

				// Drops the command list before the device's recovery, which finds no GPU object alive once the owners released
				// theirs, as ViewportCapture's release drops its own.
				void ReleaseCommandList() { m_List = nullptr; }

				// The renders that reached Render and met the loss there (MeshLoadLoss::GetLossCount), rather than failing before it.
				[[nodiscard]] uint32_t GetLostRenderCount() const { return m_Loss.GetLossCount(); }

				[[nodiscard]] Status CheckAvailability(const MethodDescriptor& /*method*/) const override { return {}; }
				[[nodiscard]] Scope<MethodContext> CreateContext(MethodRequest request) override;
				[[nodiscard]] Status AdmitRequest(MethodContext& /*context*/) override { return {}; }
				void EnterInvocation(MethodContext& /*context*/) override {}
				void LeaveInvocation(MethodContext& /*context*/) override {}
				void FinishRequest(MethodContext& /*context*/) override {}
				[[nodiscard]] MetaState GetMetaState() const override { return {}; }

				[[nodiscard]] Result<std::string> WriteOffloadedResult(std::string_view /*text*/) override
				{
					return MakeError(ErrorCode::Unsupported, "test.render's results are never offloaded");
				}
			public:
				bool LoseDevice = false;
				uint32_t ReturnedRenders = 0; // the renders whose Render returned, rather than a loss unwinding it
			private:
				GraphicsDevice* m_Device = nullptr;
				LossQuadScene* m_Scene = nullptr;
				MeshLoadLoss m_Loss;
				RenderSnapshot m_Snapshot{};
				nvrhi::CommandListHandle m_List{};
			};

			// The request context of test.render.
			class LossRenderContext final : public MethodContext
			{
			public:
				// `host` is a documented back-reference that outlives the request.
				LossRenderContext(LossRenderHost& host, MethodRequest request)
					: MethodContext(TypeKeyOf<LossRenderContext>(), std::move(request)), m_Host(&host)
				{
				}

				[[nodiscard]] LossRenderHost& GetHost() const { return *m_Host; }

				[[nodiscard]] Scope<MethodContext> CreateNested(MethodRequest request) const override
				{
					return CreateScope<LossRenderContext>(*m_Host, std::move(request));
				}
			private:
				LossRenderHost* m_Host = nullptr;
			};

			Scope<MethodContext> LossRenderHost::CreateContext(MethodRequest request)
			{
				return CreateScope<LossRenderContext>(*this, std::move(request));
			}

		}

		[[nodiscard]] static Result<LossRenderResult> RenderLossView(LossRenderContext& context, const LossRenderParams& /*params*/)
		{
			ENGINE_TRY_ASSIGN(const uint32_t frame, context.GetHost().RenderView());
			return LossRenderResult{ .Frame = frame };
		}

		// The types of test.render, frozen.
		[[nodiscard]] static Scope<TypeRegistry> CreateLossRenderTypes()
		{
			Scope<TypeRegistry> types = CreateScope<TypeRegistry>();
			// A struct without fields has no field to chain to the builder, so the builder is discarded explicitly.
			static_cast<void>(types->Struct<LossRenderParams>("TestLossRenderParams", "No params."));
			types->Struct<LossRenderResult>("TestLossRenderResult", "The render test.render submitted.")
				.Field("frame", &LossRenderResult::Frame, "The rendered snapshot's frame index.");
			types->Freeze();
			return types;
		}

		[[nodiscard]] static LiveCounts ReadLiveCounts(GraphicsDevice& device)
		{
			device.WaitForIdle();
			device.RunGarbageCollection();
			const GpuResourceTracker& tracker = device.GetResourceTracker();
			LiveCounts counts;
			for (size_t type = 0; type < GpuResourceTypeCount; ++type)
				counts.ByType[type] = tracker.GetLiveCount(static_cast<GpuResourceType>(type));
			counts.Total = tracker.GetTotalLiveCount();
			counts.Description = tracker.DescribeLiveCounts();
			return counts;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("SceneRenderer: a device loss recreates its per-view objects and renders the same image"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.3, decisions 27 and 32): the renderer, its pipeline set and its cache recover, the image is
			// byte-identical, and the tracker's counts return to their pre-loss values.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			uint64_t frame = 30;
			Test::DeviceLossTest test;
			test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
			test.Use = [&device, &setup, &frame]()
			{
				RenderSnapshot snapshot = setup.Snapshot();
				snapshot.FrameIndex = ++frame;
				return Test::RenderAndRead(device, setup, snapshot);
			};
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(setup.Renderer().GetWidth() == 64);
			CHECK(setup.Pipelines().GetGeneration() == 2); // Create, then CreatePipelines in the recovery
		}

		TEST_CASE("SceneRenderer: Release forgets a pending render and its picks, and Recreate restores the final texture"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.3, decision 27): a loss in the middle of a frame leaves no render awaiting OnSubmitted and no
			// pick that could read a target of the lost device.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
			REQUIRE_MESSAGE(list.has_value(), list.error().ToString());
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Flags = RenderViewFlags::Picking;
			(*list)->open();
			REQUIRE(setup.Renderer().Render(**list, snapshot).has_value());
			(*list)->close();
			const uint64_t generation = setup.Renderer().GetViewGeneration();

			setup.Renderer().ReleaseGpuObjects();
			CHECK(setup.Renderer().GetFinalTexture() == nullptr);
			CHECK(setup.Renderer().GetEntityIdTexture() == nullptr);
			CHECK(setup.Renderer().GetViewGeneration() > generation);
			CHECK_FALSE(setup.Renderer().RequestPick({ .X = 1, .Y = 1 }).has_value());
			const Status recreated = setup.Renderer().RecreateGpuObjects();
			REQUIRE_MESSAGE(recreated.has_value(), recreated.error().ToString());
			CHECK(setup.Renderer().GetFinalTexture() != nullptr);
			Test::RetireUnsubmittedList(device, **list);
			list->Reset();
		}

		TEST_CASE("SceneRenderer: Release leaves none of the renderer's GPU objects alive" * doctest::test_suite(Test::GpuSuite))
		{
			// The placement rule's place 2 (C18.3, decision 27): once its renderers released, the recovery finds no tracked
			// object alive, so a released renderer holds nothing its destruction would free: not its targets, its pool's free
			// targets, the output stage's targets, its binding and material sets, its per-view buffers, its picker's staging
			// copies or its profiler's queries.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Result<Scope<SceneRenderer>> created = SceneRenderer::Create(device, setup.Pipelines(), setup.Cache(), setup.Assets(), { .Width = 64, .Height = 64 });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			Scope<SceneRenderer> renderer = std::move(*created);

			const RenderSnapshot full = Test::MakeFullSnapshot(setup, 1);
			const Result<std::vector<std::byte>> fullImage = Test::RenderAndRead(device, *renderer, full);
			REQUIRE_MESSAGE(fullImage.has_value(), fullImage.error().ToString());
			// A pick in flight: its staging copy, command list and event query belong to the renderer's picker.
			const Result<PickTicket> ticket = renderer->RequestPick({ .X = 32,
				.Y = 32,
				.FrameIndex = full.FrameIndex,
				.SceneRevision = full.SceneRevision,
				.ViewGeneration = renderer->GetViewGeneration() });
			REQUIRE_MESSAGE(ticket.has_value(), ticket.error().ToString());
			// The scaled render leaves the view-size targets free in the pool and adds the output stage's.
			const Result<std::vector<std::byte>> scaledImage = Test::RenderAndRead(device, *renderer, Test::MakeScaledSnapshot(setup, 2));
			REQUIRE_MESSAGE(scaledImage.has_value(), scaledImage.error().ToString());
			const Test::LiveCounts rendered = Test::ReadLiveCounts(device);

			renderer->ReleaseGpuObjects();
			const Test::LiveCounts released = Test::ReadLiveCounts(device);
			INFO("live after the render: ", rendered.Description);
			INFO("live after the release: ", released.Description);
			CHECK(released.Total < rendered.Total);
			// The released picker's ticket resolves without a result.
			CHECK_FALSE(renderer->PollPick(*ticket, 10).has_value());

			renderer.reset();
			const Test::LiveCounts destroyed = Test::ReadLiveCounts(device);
			INFO("live after the destruction: ", destroyed.Description);
			CHECK(released.ByType == destroyed.ByType);
		}

		TEST_CASE("SceneRenderer: a pick pending at the release resolves as cancelled, and no ticket is reused after the recovery"
			* doctest::test_suite(Test::GpuSuite))
		{
			// C18.3, decision 27 (SceneRenderer.h, ReleaseGpuObjects: "every pending pick resolves as cancelled"): the release
			// drops the picker's GPU objects but keeps its tickets, so a host's pending ticket answers Cancelled, and a ticket
			// after the recovery never equals one from before, whose holder would otherwise read the new pick's result.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			SceneRenderer& renderer = setup.Renderer();
			const RenderSnapshot before = Test::MakeFullSnapshot(setup, 1);
			const Result<std::vector<std::byte>> beforeImage = Test::RenderAndRead(device, renderer, before);
			REQUIRE_MESSAGE(beforeImage.has_value(), beforeImage.error().ToString());
			const Result<PickTicket> pending = renderer.RequestPick({ .X = 32,
				.Y = 32,
				.FrameIndex = before.FrameIndex,
				.SceneRevision = before.SceneRevision,
				.ViewGeneration = renderer.GetViewGeneration() });
			REQUIRE_MESSAGE(pending.has_value(), pending.error().ToString());

			device.WaitForIdle();
			renderer.ReleaseGpuObjects();
			const Result<std::optional<PickResult>> cancelled = renderer.PollPick(*pending, before.FrameIndex + 10);
			REQUIRE_FALSE(cancelled.has_value());
			CHECK(cancelled.error().GetCode() == ErrorCode::Cancelled);
			const Status recreated = renderer.RecreateGpuObjects();
			REQUIRE_MESSAGE(recreated.has_value(), recreated.error().ToString());

			const RenderSnapshot after = Test::MakeFullSnapshot(setup, 20);
			const Result<std::vector<std::byte>> afterImage = Test::RenderAndRead(device, renderer, after);
			REQUIRE_MESSAGE(afterImage.has_value(), afterImage.error().ToString());
			const Result<PickTicket> next = renderer.RequestPick({ .X = 32,
				.Y = 32,
				.FrameIndex = after.FrameIndex,
				.SceneRevision = after.SceneRevision,
				.ViewGeneration = renderer.GetViewGeneration() });
			REQUIRE_MESSAGE(next.has_value(), next.error().ToString());
			CHECK(next->Value > pending->Value);
			// The picker created its GPU objects again for the new pick, which resolves.
			device.WaitForIdle();
			const Result<std::optional<PickResult>> resolved = renderer.PollPick(*next, after.FrameIndex + 2);
			REQUIRE_MESSAGE(resolved.has_value(), resolved.error().ToString());
			CHECK(resolved->has_value());
		}

		TEST_CASE("SceneRenderer: after Release and Recreate it renders the same images, and its live counts return"
			* doctest::test_suite(Test::GpuSuite))
		{
			// The renderer's half of M18's acceptance (C18.3, decision 27): with its set and its cache kept, Release and Recreate
			// leave the renderer as Create made it, so the same renders produce byte-identical images and the same live GPU
			// objects; a render recorded but never submitted before the release (a loss in the middle of a frame) awaits no
			// OnSubmitted.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			SceneRenderer& renderer = setup.Renderer();
			uint64_t frame = 0;
			const auto renderBoth = [&device, &setup, &renderer, &frame]() -> Result<std::array<std::vector<std::byte>, 2>>
			{
				std::array<std::vector<std::byte>, 2> images{};
				ENGINE_TRY_ASSIGN(images[0], Test::RenderAndRead(device, renderer, Test::MakeFullSnapshot(setup, ++frame)));
				ENGINE_TRY_ASSIGN(images[1], Test::RenderAndRead(device, renderer, Test::MakeScaledSnapshot(setup, ++frame)));
				return images;
			};
			const Result<std::array<std::vector<std::byte>, 2>> before = renderBoth();
			REQUIRE_MESSAGE(before.has_value(), before.error().ToString());
			const Test::LiveCounts countsBefore = Test::ReadLiveCounts(device);

			Result<nvrhi::CommandListHandle> pending = device.CreateCommandList();
			REQUIRE_MESSAGE(pending.has_value(), pending.error().ToString());
			(*pending)->open();
			REQUIRE(renderer.Render(**pending, Test::MakeFullSnapshot(setup, ++frame)).has_value());
			(*pending)->close();
			renderer.ReleaseGpuObjects();
			Test::RetireUnsubmittedList(device, **pending);
			pending->Reset();
			const Status recreated = renderer.RecreateGpuObjects();
			REQUIRE_MESSAGE(recreated.has_value(), recreated.error().ToString());
			CHECK(renderer.GetWidth() == 64);
			CHECK(renderer.GetHeight() == 64);

			const Result<std::array<std::vector<std::byte>, 2>> after = renderBoth();
			REQUIRE_MESSAGE(after.has_value(), after.error().ToString());
			CHECK((*after)[0] == (*before)[0]);
			CHECK((*after)[1] == (*before)[1]);
			const Test::LiveCounts countsAfter = Test::ReadLiveCounts(device);
			INFO("live before the release: ", countsBefore.Description);
			INFO("live after the recreation and the same renders: ", countsAfter.Description);
			CHECK(countsAfter.ByType == countsBefore.ByType);
		}

		TEST_CASE("SceneRenderer: a debug view first asked for on a lost device returns the loss and is created after the recovery"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3, decision 28): "a Gpu error on a lost device is the loss, never out-of-memory". The Albedo view's
			// pipelines cannot be created on the lost device, so Render returns the creation's Gpu error instead of ending the
			// process with FatalError(OutOfMemory) (§8.14 item 7), and the first Albedo render after the recovery creates them.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			RenderSnapshot lit = setup.Snapshot();
			setup.Submit(lit);
			REQUIRE(setup.Pipelines().GetPipelineCount() == GetDeclaredPipelineCount());

			RenderSnapshot albedo = lit;
			albedo.FrameIndex = lit.FrameIndex + 1;
			albedo.DebugView = RenderDebugView::Albedo;
			{
				// The command list exists before the loss (a lost device creates none), as a frame's list does.
				Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
				REQUIRE_MESSAGE(list.has_value(), list.error().ToString());
				device.GetDiagnostics().SetDeviceLost(true);
				device.ReportDeviceLost("SceneRendererRecoveryTests");
				(*list)->open();
				const Status lost = setup.Renderer().Render(**list, albedo);
				(*list)->close();
				setup.Renderer().OnSubmitted(albedo.FrameIndex, device.ExecuteCommandList(**list));
				REQUIRE_FALSE(lost.has_value());
				CHECK(lost.error().GetCode() == ErrorCode::Gpu);
				CHECK(setup.Pipelines().GetPipelineCount() == GetDeclaredPipelineCount());
			}

			const Status recovered = device.Recover(nullptr);
			REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
			albedo.FrameIndex = lit.FrameIndex + 2;
			const Result<std::vector<std::byte>> image = Test::RenderAndRead(device, setup, albedo);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			CHECK(setup.Pipelines().GetPipelineCount() == GetDeclaredPipelineCount() + SceneRendererPipelines::DebugViewPipelineCount);
		}

		TEST_CASE("SceneRenderer: a device loss thrown out of NVRHI in the middle of a stage is recovered at the frame boundary"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18's milestone review (decisions 25, 29 and 39): a real loss that Render's own timeline query has not seen yet
			// surfaces as a vk::DeviceLostError out of the Prepare stage's first upload. The recording context ends with its
			// stage and pass open while the error unwinds Render, and the renderer abandons the render, so the frame-boundary
			// catch reports the loss, the recovery releases and recreates the renderer, and the next frame renders. Before the
			// fix the recording context's destructor verified that no stage or pass was open and ended the process with
			// FatalError(Assert).
			if (!Test::ProbeGpuForProcess())
				return;
			GraphicsSpecification graphics;
			graphics.Validation = true;
			graphics.SynchronizationValidation = true;
			graphics.MaxApiVersion = Test::GetTestOptions().VulkanApi;
			Result<Scope<EngineContext>> created = EngineContext::Create({ .WorkerCount = 0, .Graphics = graphics });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			EngineContext& context = **created;
			GraphicsDevice& device = *context.GetGraphicsDevice();
			{
				Test::LossQuadScene scene(context);
				{
					Test::StageLosingClient client(context, scene);
					FrameLoop loop(context, client, CreateScope<ManualClock>(1.0 / 60.0), {});
					loop.RunFrame();
					loop.RunFrame();
					CHECK(client.Recoveries == 1);
					CHECK_MESSAGE(client.Recovery.has_value(), (client.Recovery.has_value() ? std::string() : client.Recovery.error().ToString()));
					CHECK_FALSE(device.IsLost());
					CHECK(device.GetRecoveryCount() == 1);
					CHECK(client.Rendered == std::vector<std::string>{ "frame 1 rendered" });
					CHECK(scene.Renderer->GetRenderStats().FrameIndex == 2);
				}
				device.WaitForIdle();
			}
			const GpuMessageCounts counts = context.DestroyGraphics();
			CHECK(counts.Errors == 0);
			CHECK(counts.Warnings == 0);
		}

		TEST_CASE("SceneRenderer: a render before the recovery of a device loss thrown out of NVRHI in the middle of a stage meets the loss as the first did"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18's milestone review (decision 39; SceneRenderer.h, Render): a render that a loss thrown out of NVRHI unwinds
			// after it began awaiting OnSubmitted, with its stage's GPU timer scopes open, is abandoned, so a second render
			// before the frame boundary's recovery (an automation method's: two viewport captures in one safe point) records
			// again and meets the same loss. A test.render and a test.renderAgain request (one handler under two names) run in
			// one Pump of a Dispatcher with the application's SystemErrorHandler (MakeVulkanSystemErrorHandler) and render the
			// quad with the debug lines of OverlayLineCount: after a first render uploaded everything, the overlay's vertex
			// write in the EditorOverlay stage's "Overlays" pass is the list's first through NVRHI's upload manager, where each
			// render meets the loss (MeshLoadLoss). Both requests fail with the Gpu error of a loss, both renders reach Render
			// and neither returns, the first request's loss report stays, and the recovery then renders the image of before
			// the loss. Without the abandonment the second render ended the process with FatalError(Assert) ("Render requires
			// OnSubmitted for the prior Render", or GpuProfiler::BeginFrame's open scopes).
			if (!Test::ProbeGpuForProcess())
				return;
			GraphicsSpecification graphics;
			graphics.Validation = true;
			graphics.SynchronizationValidation = true;
			graphics.MaxApiVersion = Test::GetTestOptions().VulkanApi;
			Result<Scope<EngineContext>> created = EngineContext::Create({ .WorkerCount = 0, .Graphics = graphics });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			EngineContext& context = **created;
			GraphicsDevice& device = *context.GetGraphicsDevice();
			{
				Test::LossQuadScene scene(context);
				for (uint32_t line = 0; line < Test::OverlayLineCount; ++line)
				{
					const float x = -1.0f + 2.0f * static_cast<float>(line) / static_cast<float>(Test::OverlayLineCount);
					scene.Snapshot.DebugDraw.AddLine({ x, -1.0f, -2.5f }, { x, 1.0f, -2.5f }, glm::vec4(1.0f, 0.5f, 0.0f, 1.0f));
				}
				RenderSnapshot first = scene.Snapshot;
				first.FrameIndex = 1;
				const Result<std::vector<std::byte>> before = Test::RenderAndRead(device, *scene.Renderer, first);
				REQUIRE_MESSAGE(before.has_value(), before.error().ToString());

				Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
				REQUIRE_MESSAGE(list.has_value(), list.error().ToString());
				const Scope<TypeRegistry> types = Test::CreateLossRenderTypes();
				MethodRegistry methods(*types);
				// The second request names the same handler differently, so the loss report shows which request it kept.
				for (const std::string_view name : { std::string_view("test.render"), std::string_view("test.renderAgain") })
				{
					methods.Add<Test::LossRenderContext, Test::LossRenderParams, Test::LossRenderResult>(
						{ .Name = std::string(name),
							.Description = "Renders the loss test's view.",
							.Examples = { MethodExample{ .Description = "Render.", .Params = Json::object() } } },
						&Test::RenderLossView);
				}
				methods.Freeze();
				{
					Test::LossRenderHost host(device, scene, first, std::move(*list));
					const RingBufferSink log(64);
					Dispatcher dispatcher(methods, host, log, nullptr, { .SystemErrors = MakeVulkanSystemErrorHandler(context) });
					dispatcher.AddClient(1, "test");
					host.LoseDevice = true;
					dispatcher.Enqueue(1, Test::MakeTestRequest(1, "test.render", Json::object()));
					dispatcher.Enqueue(1, Test::MakeTestRequest(2, "test.renderAgain", Json::object()));
					std::vector<OutboundMessage> messages;
					for (int pump = 0; pump < 10 && dispatcher.HasWork(); ++pump)
					{
						for (OutboundMessage& message : dispatcher.Pump(std::chrono::seconds(10)))
							messages.push_back(std::move(message));
					}
					REQUIRE(messages.size() == 2);
					for (OutboundMessage& message : messages)
					{
						INFO("response: ", message.Message.dump());
						const std::string_view method = message.Message["id"] == Json(1) ? "test.render" : "test.renderAgain";
						CHECK(message.Message["error"]["data"]["errorCode"] == Json("Gpu"));
						CHECK(message.Message["error"]["data"]["detail"].dump().contains(std::format("the GPU device was lost during '{}'", method)));
					}
					// Both renders reached Render and met the loss there, and neither returned.
					CHECK(host.GetLostRenderCount() == 2);
					CHECK(host.ReturnedRenders == 0);
					CHECK(device.IsLost());
					// The first loss's report stays: the second request's loss changes nothing.
					REQUIRE(device.GetLossReport().has_value());
					INFO("loss report: ", device.GetLossReport()->Context);
					CHECK(device.GetLossReport()->Context.contains("Vulkan error in automation method 'test.render'"));
					CHECK_FALSE(device.GetLossReport()->Context.contains("'test.renderAgain'"));
					CHECK(device.GetRecoveryCount() == 0);

					// The frame boundary's recovery (Application::OnFrameDeviceLost), the request's list released first.
					host.LoseDevice = false;
					host.ReleaseCommandList();
				}
				const Status recovered = context.RecoverGraphics();
				REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
				CHECK_FALSE(device.IsLost());
				CHECK(device.GetRecoveryCount() == 1);
				RenderSnapshot after = scene.Snapshot;
				after.FrameIndex = 4;
				const Result<std::vector<std::byte>> recoveredImage = Test::RenderAndRead(device, *scene.Renderer, after);
				REQUIRE_MESSAGE(recoveredImage.has_value(), recoveredImage.error().ToString());
				CHECK(*recoveredImage == *before);
				device.WaitForIdle();
			}
			const GpuMessageCounts counts = context.DestroyGraphics();
			CHECK(counts.Errors == 0);
			CHECK(counts.Warnings == 0);
		}
	}

}
