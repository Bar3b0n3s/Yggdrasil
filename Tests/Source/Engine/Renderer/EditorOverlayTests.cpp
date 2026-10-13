#include "TestsPCH.h"
#include "Engine/Renderer/EditorOverlay.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/DebugRenderer.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Shared/ViewConstants.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

		static RenderSnapshot OverlayTestSnapshot(RenderProjection projection)
		{
			RenderSnapshot snapshot;
			snapshot.HasCamera = true;
			snapshot.Camera.Position = { 0.0f, 3.0f, 5.0f };
			snapshot.Camera.View = glm::lookAt(snapshot.Camera.Position, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			snapshot.Camera.ProjectionKind = projection;
			snapshot.Camera.ViewportWidth = snapshot.Camera.ViewportHeight = 64;
			snapshot.Camera.OrthographicSize = 5.0f;
			snapshot.Camera.FarClip = 100.0f;
			snapshot.Camera.Projection = ComputeReverseZProjection(projection, 60.0f, 5.0f, 0.1f, 100.0f, 64, 64);
			snapshot.Flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Grid | RenderViewFlags::Icons;
			return snapshot;
		}

		static double OverlayProjectedWidth(const DebugDrawList& list, const CameraData& camera)
		{
			double minimum = 1.0e30, maximum = -1.0e30;
			for (const auto& command : list.GetCommands())
			{
				const auto& line = std::get<DebugLine>(command.Shape);
				for (const glm::vec3 point : { line.From, line.To })
				{
					const glm::vec4 clip = camera.Projection * camera.View * glm::vec4(point, 1.0f);
					const double pixel = clip.x / clip.w * camera.ViewportWidth * 0.5;
					minimum = std::min(minimum, pixel);
					maximum = std::max(maximum, pixel);
				}
			}
			return maximum - minimum;
		}

		// The editor's default scene camera (ExplicitRenderCamera: from (0, 3, 10) at the origin, 60 degrees, far 1000) in the
		// headless 640x360 viewport, or `position` looking at `target` with `up` and a vertical field of view of `verticalFov`.
		static CameraData EditorPoseCamera(RenderProjection projection, float orthographicSize = 10.0f, float farClip = 1000.0f,
			const glm::vec3& position = glm::vec3(0.0f, 3.0f, 10.0f), const glm::vec3& target = glm::vec3(0.0f),
			const glm::vec3& up = glm::vec3(0.0f, 1.0f, 0.0f), float verticalFov = 60.0f)
		{
			CameraData camera;
			camera.Position = position;
			camera.View = glm::lookAt(position, target, up);
			camera.ProjectionKind = projection;
			camera.OrthographicSize = orthographicSize;
			camera.NearClip = 0.1f;
			camera.FarClip = farClip;
			camera.ViewportWidth = 640;
			camera.ViewportHeight = 360;
			camera.Projection = ComputeReverseZProjection(projection, verticalFov, orthographicSize, 0.1f, farClip, 640, 360);
			return camera;
		}

		static DebugDrawList GridOf(const CameraData& camera)
		{
			RenderSnapshot snapshot;
			snapshot.HasCamera = true;
			snapshot.Camera = camera;
			snapshot.Flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Grid;
			DebugDrawList grid;
			REQUIRE(AppendEditorOverlay(snapshot, grid).has_value());
			REQUIRE_FALSE(grid.IsEmpty());
			return grid;
		}

		// The spacing of the grid's lines of constant x: the smallest gap between their distinct coordinates.
		static double GridSpacing(const DebugDrawList& grid)
		{
			std::set<float> coordinates;
			for (const auto& command : grid.GetCommands())
			{
				const auto& line = std::get<DebugLine>(command.Shape);
				if (line.From.x == line.To.x && line.From.z != line.To.z)
					coordinates.insert(line.From.x);
			}
			double spacing = std::numeric_limits<double>::max();
			for (auto next = std::next(coordinates.begin()); next != coordinates.end(); ++next)
				spacing = std::min(spacing, static_cast<double>(*next) - static_cast<double>(*std::prev(next)));
			return spacing;
		}

		// The largest alpha of a regular (non-axis) grid segment whose midpoint the camera sees.
		static float VisibleGridAlpha(const DebugDrawList& grid, const CameraData& camera)
		{
			float alpha = 0.0f;
			for (const auto& command : grid.GetCommands())
			{
				const auto& line = std::get<DebugLine>(command.Shape);
				if (command.Color.r != command.Color.g)
					continue; // a world axis
				const glm::vec4 clip = camera.Projection * camera.View * glm::vec4((line.From + line.To) * 0.5f, 1.0f);
				if (clip.w > 0.0f && std::abs(clip.x) <= clip.w && std::abs(clip.y) <= clip.w && clip.z >= 0.0f && clip.z <= clip.w)
					alpha = std::max(alpha, command.Color.a);
			}
			return alpha;
		}

		// Whether the camera sees `point` more than `margin` (a fraction of the half extent) inside its view's edges, and not on
		// its near or far plane (where the clipping, not the grid, ends a line).
		static bool GridPointInsideView(const glm::vec3& point, const CameraData& camera, float margin)
		{
			const float depth = -(camera.View * glm::vec4(point, 1.0f)).z;
			const glm::vec4 clip = camera.Projection * camera.View * glm::vec4(point, 1.0f);
			const float inner = (1.0f - margin) * clip.w;
			return depth > camera.NearClip * 1.01f && depth < camera.FarClip * 0.99f && clip.w > 0.0f && std::abs(clip.x) < inner && std::abs(clip.y) < inner;
		}

		// The largest alpha of a grid line's end segment whose end the camera sees well inside its view: a grid whose region
		// ends in the middle of the view shows an edge there unless its fade has already reached zero.
		static float GridInteriorEndAlpha(const DebugDrawList& grid, const CameraData& camera)
		{
			struct LineEnds
			{
				float Low = std::numeric_limits<float>::max();
				float High = std::numeric_limits<float>::lowest();
				glm::vec3 LowPoint{ 0.0f };
				glm::vec3 HighPoint{ 0.0f };
				float LowAlpha = 0.0f;
				float HighAlpha = 0.0f;
			};
			// Lines of constant x run along z and lines of constant z along x; each is keyed by its axis and coordinate.
			std::map<std::pair<int, float>, LineEnds> lines;
			for (const auto& command : grid.GetCommands())
			{
				const auto& line = std::get<DebugLine>(command.Shape);
				const bool alongZ = line.From.x == line.To.x;
				LineEnds& ends = lines[alongZ ? std::pair{ 0, line.From.x } : std::pair{ 2, line.From.z }];
				for (const glm::vec3& point : { line.From, line.To })
				{
					const float along = alongZ ? point.z : point.x;
					if (along < ends.Low)
					{
						ends.Low = along;
						ends.LowPoint = point;
						ends.LowAlpha = command.Color.a;
					}
					if (along > ends.High)
					{
						ends.High = along;
						ends.HighPoint = point;
						ends.HighAlpha = command.Color.a;
					}
				}
			}
			float alpha = 0.0f;
			for (const auto& [key, ends] : lines)
			{
				if (GridPointInsideView(ends.LowPoint, camera, 0.02f))
					alpha = std::max(alpha, ends.LowAlpha);
				if (GridPointInsideView(ends.HighPoint, camera, 0.02f))
					alpha = std::max(alpha, ends.HighAlpha);
			}
			return alpha;
		}

		// The largest alpha of a regular grid segment whose midpoint the camera sees in the top part of its view (normalized
		// device y above `fromY`).
		static float GridAlphaNearTop(const DebugDrawList& grid, const CameraData& camera, float fromY)
		{
			float alpha = 0.0f;
			for (const auto& command : grid.GetCommands())
			{
				const auto& line = std::get<DebugLine>(command.Shape);
				if (command.Color.r != command.Color.g)
					continue; // a world axis
				const glm::vec4 clip = camera.Projection * camera.View * glm::vec4((line.From + line.To) * 0.5f, 1.0f);
				if (GridPointInsideView((line.From + line.To) * 0.5f, camera, 0.0f) && clip.y > fromY * clip.w)
					alpha = std::max(alpha, command.Color.a);
			}
			return alpha;
		}

		static RenderSnapshot WireframeTestSnapshot(Test::InMemoryAssetManager& assets, bool mirrored, bool backwards, bool doubleSided, uint32_t triangles = 1)
		{
			auto mesh = CreateRef<MeshData>();
			mesh->Vertices = { { .Position = { -0.5f, -0.5f, -2.0f } }, { .Position = { 0.5f, -0.5f, -2.0f } }, { .Position = { 0.0f, 0.5f, -2.0f } } };
			for (uint32_t index = 0; index < triangles; ++index)
				mesh->Indices.insert(mesh->Indices.end(), backwards ? std::initializer_list<uint32_t>{ 0, 2, 1 } : std::initializer_list<uint32_t>{ 0, 1, 2 });
			mesh->Bounds = { { -0.5f, -0.5f, -2.0f }, { 0.5f, 0.5f, -2.0f } };
			mesh->Submeshes = { { .IndexCount = triangles * 3, .Bounds = mesh->Bounds } };
			mesh->Slots = { {} };
			assets.Publish(UUID(911), mesh);
			auto material = CreateRef<MaterialData>();
			material->DoubleSided = doubleSided;
			assets.Publish(UUID(912), material);
			auto snapshot = OverlayTestSnapshot(RenderProjection::Perspective);
			snapshot.Camera.Position = glm::vec3(0.0f);
			snapshot.Camera.View = glm::mat4(1.0f);
			snapshot.Flags = RenderViewFlags::Wireframe;
			snapshot.Meshes = { { .World = glm::scale(glm::mat4(1.0f), glm::vec3(mirrored ? -1.0f : 1.0f, 1.0f, 1.0f)), .Mesh = UUID(911), .Materials = { UUID(912) }, .Entity = UUID(42), .PickId = 1 } };
			snapshot.PickTable = { UUID(42) };
			return snapshot;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("EditorOverlay: grid and procedural icons support both camera projections")
		{
			for (const auto projection : { RenderProjection::Perspective, RenderProjection::Orthographic })
			{
				auto snapshot = Utils::OverlayTestSnapshot(projection);
				DebugDrawList grid;
				REQUIRE(AppendEditorOverlay(snapshot, grid).has_value());
				CHECK_FALSE(grid.IsEmpty());
				CHECK(grid.GetDroppedCount() == 0);
				bool axisFound = false;
				float faintest = 1.0f;
				for (const auto& command : grid.GetCommands())
				{
					const auto& line = std::get<DebugLine>(command.Shape);
					CHECK(line.From.y == 0.0f);
					CHECK(line.To.y == 0.0f);
					CHECK(command.Depth == DebugDepthMode::Tested);
					CHECK(command.Color.a >= 0.0f);
					CHECK(command.Color.a <= 0.8f);
					axisFound |= command.Color.r != command.Color.g;
					if (command.Color.r == command.Color.g)
						faintest = std::min(faintest, command.Color.a);
				}
				CHECK(axisFound);
				CHECK(faintest < 0.2f); // regular lines (alpha 0.4 at the centre) fade with distance
				snapshot.Flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Icons;
				const glm::vec3 forward = -glm::vec3(glm::inverse(snapshot.Camera.View)[2]);
				for (uint32_t kind = 0; kind <= static_cast<uint32_t>(RenderIconKind::AudioListener); ++kind)
				{
					std::array<double, 2> widths{};
					for (size_t distance = 0; distance < widths.size(); ++distance)
					{
						DebugDrawList icons;
						snapshot.Icons = { { .Kind = static_cast<RenderIconKind>(kind), .Position = snapshot.Camera.Position + forward * (distance == 0 ? 2.0f : 20.0f), .Size = 20.0f } };
						REQUIRE(AppendEditorOverlay(snapshot, icons).has_value());
						CHECK_FALSE(icons.IsEmpty());
						widths[distance] = Utils::OverlayProjectedWidth(icons, snapshot.Camera);
					}
					CHECK(widths[0] == doctest::Approx(widths[1]).epsilon(1.0e-4));
					CHECK(widths[0] > 10.0);
					CHECK(widths[0] <= 20.0);
				}
			}
		}

		TEST_CASE("EditorOverlay: grid spacing follows the camera height and orthographic size, never the far plane")
		{
			// The default editor pose 3 m above the ground: 1 m cells, whatever FarClip is (M9-F19 measured 100 m cells from
			// the far plane's footprint).
			for (const float farClip : { 1000.0f, 100.0f })
			{
				CAPTURE(farClip);
				CHECK(Utils::GridSpacing(Utils::GridOf(Utils::EditorPoseCamera(RenderProjection::Perspective, 10.0f, farClip))) == doctest::Approx(1.0));
				// Orthographic: the default size 10 (a view 20 m tall).
				CHECK(Utils::GridSpacing(Utils::GridOf(Utils::EditorPoseCamera(RenderProjection::Orthographic, 10.0f, farClip))) == doctest::Approx(1.0));
			}
			CHECK(Utils::GridSpacing(Utils::GridOf(Utils::EditorPoseCamera(RenderProjection::Orthographic, 0.5f))) == doctest::Approx(0.1));
			CHECK(Utils::GridSpacing(Utils::GridOf(Utils::EditorPoseCamera(RenderProjection::Orthographic, 50.0f))) == doctest::Approx(10.0));
			CHECK(Utils::GridSpacing(Utils::GridOf(Utils::EditorPoseCamera(RenderProjection::Perspective, 10.0f, 1000.0f, glm::vec3(0.0f, 30.0f, 10.0f))))
				== doctest::Approx(10.0));
			// Orbiting close to the ground keeps the cells near the camera small.
			CHECK(Utils::GridSpacing(Utils::GridOf(Utils::EditorPoseCamera(RenderProjection::Perspective, 10.0f, 1000.0f, glm::vec3(0.0f, 0.15f, 0.5f))))
				== doctest::Approx(0.1));
		}

		TEST_CASE("EditorOverlay: an orthographic grid stays visible zoomed in and seen from far above")
		{
			// The fade ignores depth along the orthographic view axis (M9-F20: at size 0.5 the brightest line had alpha 0.03,
			// and from 50 m above 3e-4).
			const CameraData zoomed = Utils::EditorPoseCamera(RenderProjection::Orthographic, 0.5f);
			CHECK(Utils::VisibleGridAlpha(Utils::GridOf(zoomed), zoomed) > 0.3f);
			const CameraData above = Utils::EditorPoseCamera(RenderProjection::Orthographic, 5.0f, 1000.0f, glm::vec3(0.0f, 50.0f, 0.0f), glm::vec3(0.0f),
				glm::vec3(0.0f, 0.0f, -1.0f));
			CHECK(Utils::VisibleGridAlpha(Utils::GridOf(above), above) > 0.3f);
			const CameraData perspective = Utils::EditorPoseCamera(RenderProjection::Perspective);
			CHECK(Utils::VisibleGridAlpha(Utils::GridOf(perspective), perspective) > 0.3f);
		}

		TEST_CASE("EditorOverlay: an orthographic grid at a shallow pitch fades out before its region ends")
		{
			// The default orthographic size (1 m cells) orbited down around the origin at the default distance, also turned
			// aside so both axes' lines run obliquely (fix review R02: at 10 degrees the grid stopped 85% of the way up the view
			// with alpha 0.275 on its last segments, cut at 40 cells while its fade reached zero only far beyond).
			constexpr float Distance = 10.44f;
			for (const float yaw : { 0.0f, 30.0f })
			{
				for (const float pitch : { 10.0f, 3.0f, 0.5f })
				{
					CAPTURE(yaw);
					CAPTURE(pitch);
					const float pitchRadians = glm::radians(pitch);
					const float yawRadians = glm::radians(yaw);
					const glm::vec3 position = Distance * glm::vec3(std::cos(pitchRadians) * std::sin(yawRadians), std::sin(pitchRadians), std::cos(pitchRadians) * std::cos(yawRadians));
					const CameraData camera = Utils::EditorPoseCamera(RenderProjection::Orthographic, 10.0f, 1000.0f, position);
					const DebugDrawList grid = Utils::GridOf(camera);
					CHECK(grid.GetDroppedCount() == 0);
					// No line ends inside the view with any alpha left, and the line count stays within 100 cells on each side.
					CHECK(Utils::GridInteriorEndAlpha(grid, camera) < 0.02f);
					CHECK(grid.GetCommands().size() <= 2u * 202u * 16u);
					// While the footprint fits those 100 cells, the grid covers the whole view up to its top edge.
					if (pitch == 10.0f)
						CHECK(Utils::GridAlphaNearTop(grid, camera, 0.85f) > 0.1f);
				}
			}
		}

		TEST_CASE("EditorOverlay: a narrow perspective view of distant ground still shows the grid")
		{
			// A 10 degree field of view from 10 m up, aimed just below the horizon: it sees the ground only from 53 m to the far
			// plane, beyond the 40 cells a 1 m grid around the camera's ground point covers (fix review R02: no grid at all).
			const CameraData narrow = Utils::EditorPoseCamera(RenderProjection::Perspective, 10.0f, 1000.0f, glm::vec3(0.0f, 10.0f, 10.0f), glm::vec3(0.0f, 9.3f, 0.0f),
				glm::vec3(0.0f, 1.0f, 0.0f), 10.0f);
			const DebugDrawList grid = Utils::GridOf(narrow);
			CHECK(Utils::GridSpacing(grid) == doctest::Approx(10.0));
			CHECK(Utils::VisibleGridAlpha(grid, narrow) > 0.1f);
			CHECK(Utils::GridInteriorEndAlpha(grid, narrow) < 0.02f);
			// A view that sees the ground near the camera keeps the camera height's spacing.
			const CameraData wide = Utils::EditorPoseCamera(RenderProjection::Perspective, 10.0f, 1000.0f, glm::vec3(0.0f, 10.0f, 10.0f), glm::vec3(0.0f, 9.3f, 0.0f));
			CHECK(Utils::GridSpacing(Utils::GridOf(wide)) == doctest::Approx(1.0));
		}

		TEST_CASE("EditorOverlay: no camera or disabled view flags produce no overlay")
		{
			auto snapshot = Utils::OverlayTestSnapshot(RenderProjection::Perspective);
			DebugDrawList output;
			snapshot.HasCamera = false;
			REQUIRE(AppendEditorOverlay(snapshot, output).has_value());
			CHECK(output.IsEmpty());
			snapshot.HasCamera = true;
			snapshot.Flags = RenderViewFlags::Grid | RenderViewFlags::Icons;
			REQUIRE(AppendEditorOverlay(snapshot, output).has_value());
			CHECK(output.IsEmpty());
			snapshot.Flags |= RenderViewFlags::EditorOverlays;
			snapshot.Flags &= ~RenderViewFlags::Grid;
			const glm::vec3 forward = -glm::vec3(glm::inverse(snapshot.Camera.View)[2]);
			snapshot.Icons = { { .Position = snapshot.Camera.Position - forward } };
			REQUIRE(AppendEditorOverlay(snapshot, output).has_value());
			CHECK(output.IsEmpty());
			output.AddLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec4(1.0f));
			const DebugDrawList original = output;
			snapshot.Icons = { { .Position = glm::vec3(0.0f) }, { .Position = glm::vec3(0.0f), .Size = std::numeric_limits<float>::infinity() } };
			CHECK_FALSE(AppendEditorOverlay(snapshot, output).has_value());
			CHECK(output == original);
			snapshot.Icons.clear();
			snapshot.Camera.Projection = glm::mat4(0.0f);
			CHECK_FALSE(AppendEditorOverlay(snapshot, output).has_value());
			CHECK(output == original);
		}

		TEST_CASE("Wireframe: portable triangle edges respect mirrored double-sided and budget rules")
		{
			Test::InMemoryAssetManager assets;
			for (bool mirrored : { false, true })
			{
				for (bool backwards : { false, true })
				{
					for (bool doubleSided : { false, true })
					{
						const auto snapshot = Utils::WireframeTestSnapshot(assets, mirrored, backwards, doubleSided);
						DebugDrawList output;
						REQUIRE(AppendWireframeOverlay(snapshot, assets, output).has_value());
						CHECK(output.GetSize() == (backwards && !doubleSided ? 0 : 3));
						for (const auto& command : output.GetCommands())
						{
							CHECK(command.Color == glm::vec4(1.0f));
							CHECK(command.Depth == DebugDepthMode::Tested);
						}
					}
				}
			}
			auto snapshot = Utils::WireframeTestSnapshot(assets, false, false, false, 22000);
			DebugDrawList budget;
			REQUIRE(AppendWireframeOverlay(snapshot, assets, budget).has_value());
			CHECK(budget.GetSize() == DebugDrawList::MaxCommands);
			CHECK(budget.GetDroppedCount() == 66000 - DebugDrawList::MaxCommands);
			DebugDrawList copy = budget;
			snapshot.Meshes[0].World[0][0] = std::numeric_limits<float>::quiet_NaN();
			CHECK_FALSE(AppendWireframeOverlay(snapshot, assets, budget).has_value());
			CHECK(budget == copy);
			snapshot.DebugView = RenderDebugView::Normals;
			CHECK(AppendWireframeOverlay(snapshot, assets, budget).has_value());
			CHECK(budget == copy);
			// Submeshes the solid passes cull draw no edges: behind the camera, and beyond FarClip (M9-F14).
			snapshot = Utils::WireframeTestSnapshot(assets, false, false, false);
			snapshot.Meshes[0].World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 4.0f));
			DebugDrawList behind;
			REQUIRE(AppendWireframeOverlay(snapshot, assets, behind).has_value());
			CHECK(behind.IsEmpty());
			snapshot.Camera.FarClip = 50.0f;
			snapshot.Meshes[0].World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -78.0f));
			DebugDrawList beyond;
			REQUIRE(AppendWireframeOverlay(snapshot, assets, beyond).has_value());
			CHECK(beyond.IsEmpty());
			snapshot.Meshes[0].World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -38.0f));
			DebugDrawList within;
			REQUIRE(AppendWireframeOverlay(snapshot, assets, within).has_value());
			CHECK(within.GetSize() == 3);
			snapshot = Utils::WireframeTestSnapshot(assets, false, false, false);
			snapshot.Meshes[0].World = glm::scale(glm::mat4(1.0f), glm::vec3(0.0f));
			DebugDrawList degenerate;
			REQUIRE(AppendWireframeOverlay(snapshot, assets, degenerate).has_value());
			CHECK(degenerate.IsEmpty());
			// A tiny triangle inside the frustum (the mesh's z = -2 shrinks toward the camera, so it is moved out past the near
			// plane).
			snapshot.Meshes[0].World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -1.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(1.0e-7f));
			DebugDrawList small;
			REQUIRE(AppendWireframeOverlay(snapshot, assets, small).has_value());
			CHECK(small.GetSize() == 3); // Nonzero triangles retain edges regardless of world scale.
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("EditorOverlay: grid and icons render through portable depth-tested lines")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			auto renderer = DebugRenderer::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE(renderer.has_value());
			nvrhi::TextureDesc desc;
			desc.width = desc.height = 64;
			desc.format = nvrhi::Format::RGBA8_UNORM;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			auto color = gpu.GetDevice().CreateTexture(desc);
			desc.format = nvrhi::Format::D32;
			desc.initialState = nvrhi::ResourceStates::DepthWrite;
			auto depth = gpu.GetDevice().CreateTexture(desc);
			REQUIRE(color.has_value());
			REQUIRE(depth.has_value());
			auto framebuffer = gpu.GetDevice().CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(*color).setDepthAttachment(*depth));
			REQUIRE(framebuffer.has_value());
			nvrhi::BufferDesc bufferDesc;
			bufferDesc.byteSize = sizeof(ViewConstants);
			bufferDesc.isConstantBuffer = true;
			bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
			bufferDesc.keepInitialState = true;
			auto buffer = gpu.GetDevice().CreateBuffer(bufferDesc);
			REQUIRE(buffer.has_value());
			PassBindingCache bindings;
			for (auto projection : { RenderProjection::Perspective, RenderProjection::Orthographic })
			{
				const auto snapshot = Utils::OverlayTestSnapshot(projection);
				auto withIcon = snapshot;
				withIcon.Icons = { { .Position = glm::vec3(0.0f), .Size = 20.0f } };
				DebugDrawList list;
				REQUIRE(AppendEditorOverlay(withIcon, list).has_value());
				for (float obstruction : { 0.0f, 1.0f })
				{
					auto commands = gpu.GetDevice().CreateCommandList();
					REQUIRE(commands.has_value());
					(*commands)->open();
					ViewConstants view{};
					view.View = snapshot.Camera.View;
					view.Projection = snapshot.Camera.Projection;
					view.ViewProjection = view.Projection * view.View;
					view.ViewportSize = glm::vec2(64.0f);
					(*commands)->writeBuffer(*buffer, &view, sizeof(view));
					(*commands)->clearTextureFloat(*color, nvrhi::AllSubresources, nvrhi::Color(0.0f));
					(*commands)->clearDepthStencilTexture(*depth, nvrhi::AllSubresources, true, obstruction, false, 0);
					auto recorded = (*renderer)->Record(**commands, bindings, { .DebugDraw = &list, .Framebuffer = *framebuffer, .ViewConstants = *buffer });
					(*commands)->close();
					gpu.GetDevice().ExecuteCommandList(**commands);
					REQUIRE(recorded.has_value());
					CHECK(*recorded > 0);
					Readback readback(gpu.GetDevice());
					auto image = readback.ReadTexture(**color);
					REQUIRE(image.has_value());
					size_t lit = 0;
					for (size_t pixel = 0; pixel < image->Pixels.size(); pixel += 4)
						lit += image->Pixels[pixel] != std::byte{ 0 } || image->Pixels[pixel + 1] != std::byte{ 0 } || image->Pixels[pixel + 2] != std::byte{ 0 };
					if (obstruction == 0.0f)
						CHECK(lit > 40);
					else
						CHECK(lit == 0);
				}
			}
		}

		TEST_CASE("Wireframe: masked blended mirrored and double-sided meshes keep geometric edges and solid picking")
		{
			// ADR 0017: wire edges at geometric coverage for Mask (no sampled alpha) and Blend, tested against the opaque depth
			// without writing it; mirrored winding and DoubleSided as the solid passes; EntityId unchanged, Mask holes included;
			// no skybox, bloom or FXAA (M9-F13, M9-F38).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, 160, 96);
			MaterialData masked;
			masked.AlphaMode = AlphaMode::Mask;
			masked.BaseColor.a = 0.25f; // below the 0.5 cutoff everywhere: the solid passes cut the whole quad
			MaterialData blended;
			blended.AlphaMode = AlphaMode::Blend;
			blended.BaseColor.a = 0.5f;
			MaterialData twoSided;
			twoSided.DoubleSided = true;
			fixture.Assets().Publish(UUID(8930), CreateRef<MaterialData>(masked));
			fixture.Assets().Publish(UUID(8931), CreateRef<MaterialData>(blended));
			fixture.Assets().Publish(UUID(8932), CreateRef<MaterialData>(twoSided));
			struct WireCase
			{
				const char* Name = "";
				AssetHandle Material{};
				glm::mat4 Transform{ 1.0f };
				bool Edges = true;
				bool Picked = true;
			};
			const glm::mat4 base = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -3.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.5f));
			const glm::mat4 turned = base * glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
			const std::array<WireCase, 6> cases = { {
				{ .Name = "opaque", .Material = UUID(8911), .Transform = base },
				{ .Name = "mask", .Material = UUID(8930), .Transform = base, .Picked = false },
				{ .Name = "blend", .Material = UUID(8931), .Transform = base, .Picked = false },
				{ .Name = "mirrored", .Material = UUID(8911), .Transform = base * glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f)) },
				{ .Name = "double-sided back", .Material = UUID(8932), .Transform = turned },
				{ .Name = "single-sided back", .Material = UUID(8911), .Transform = turned, .Edges = false, .Picked = false },
			} };
			// The quad spans 0.5 m at z = -3: about 13.9 pixels each side of the centre (80, 48). Its left edge crosses row 48
			// near column 66; local (0.5, 0), pixel (87, 48), lies inside, off both diagonals (mirroring flips the diagonal).
			const auto edgeLit = [](const Image& image)
			{
				for (uint32_t x = 63; x <= 69; ++x)
					if (Test::SceneRendererIntegrationFixture::Channel(image, x, 48) == 255)
						return true;
				return false;
			};
			for (const WireCase& wire : cases)
			{
				CAPTURE(wire.Name);
				RenderSnapshot snapshot = fixture.Snapshot();
				snapshot.Meshes[0].Materials = { wire.Material };
				snapshot.Meshes[0].World = wire.Transform;
				snapshot.Post.BloomEnabled = true;
				snapshot.Post.FxaaEnabled = true;
				snapshot.Flags = RenderViewFlags::Picking;
				fixture.Submit(snapshot);
				Readback readback(gpu.GetDevice());
				const auto solidIds = readback.ReadTexture(*fixture.Renderer().GetEntityIdTexture());
				REQUIRE(solidIds.has_value());
				snapshot.Flags |= RenderViewFlags::Wireframe;
				++snapshot.FrameIndex;
				const Image image = fixture.Render(snapshot);
				const auto wireIds = readback.ReadTexture(*fixture.Renderer().GetEntityIdTexture());
				REQUIRE(wireIds.has_value());
				CHECK(wireIds->Pixels == solidIds->Pixels);
				uint32_t centreId = 0;
				std::memcpy(&centreId, wireIds->Pixels.data() + (48 * 160 + 87) * sizeof(uint32_t), sizeof(centreId));
				CHECK(centreId == (wire.Picked ? 1U : 0U));
				CHECK(edgeLit(image) == wire.Edges);
				for (size_t channel = 0; channel < 3; ++channel)
					CHECK(Test::SceneRendererIntegrationFixture::Channel(image, 87, 48, channel) == 0);
				const RenderStats stats = fixture.Renderer().GetRenderStats();
				for (const char* suppressed : { "ForwardOpaque", "ForwardTransparent", "Skybox", "Bloom", "FXAA", "GTAO" })
					CHECK(Test::SceneRendererIntegrationFixture::FindPass(stats, suppressed) == nullptr);
				REQUIRE((Test::SceneRendererIntegrationFixture::FindPass(stats, "Wireframe") != nullptr) == wire.Edges);
			}

			// A Blend quad's edge hidden behind an opaque quad is rejected by the opaque depth; its other edges stay.
			RenderSnapshot occluded = fixture.Snapshot();
			occluded.Meshes[0].Materials = { UUID(8931) };
			occluded.Meshes[0].World = base;
			MeshDrawItem cover = occluded.Meshes[0];
			cover.Materials = { UUID(8911) };
			cover.Entity = UUID(40);
			cover.PickId = 2;
			// Half extent 0.3 m at z = -2.5 over the Blend quad's left edge (x = -0.5 at z = -3), raised 0.2 m: it covers columns
			// 56 to 76 and rows 31 to 51, while its own edges and diagonal (column 59.5 on row 48) stay clear of columns 63 to 69.
			cover.World = glm::translate(glm::mat4(1.0f), glm::vec3(-0.5f * 2.5f / 3.0f, 0.2f, -2.5f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.3f));
			occluded.Meshes.push_back(cover);
			occluded.Flags = RenderViewFlags::Wireframe;
			const Image hidden = fixture.Render(occluded);
			CHECK_FALSE(edgeLit(hidden));
			// The Blend quad's right edge (column 94) is not behind anything.
			bool right = false;
			for (uint32_t x = 91; x <= 97; ++x)
				right = right || Test::SceneRendererIntegrationFixture::Channel(hidden, x, 48) == 255;
			CHECK(right);
		}

		TEST_CASE("Wireframe: edges beyond the debug draw budget warn once per view")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			GpuResourceCache cache(gpu.GetDevice(), assets);
			auto pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE(pipelines.has_value());
			auto renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets, { 64, 64 });
			REQUIRE(renderer.has_value());
			// 22,000 visible triangles need 66,000 edge commands, more than DebugDrawList::MaxCommands.
			auto snapshot = Utils::WireframeTestSnapshot(assets, false, false, false, 22000);
			snapshot.Camera.ClearToSkybox = false;
			Test::ExpectLog warning(LogLevel::Warn, "beyond the debug draw budget");
			for (uint64_t frame = 1; frame <= 2; ++frame)
			{
				snapshot.FrameIndex = frame;
				auto commands = gpu.GetDevice().CreateCommandList();
				REQUIRE(commands.has_value());
				(*commands)->open();
				const auto recorded = (*renderer)->Render(**commands, snapshot);
				(*commands)->close();
				(*renderer)->OnSubmitted(snapshot.FrameIndex, gpu.GetDevice().ExecuteCommandList(**commands));
				REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
			}
			CHECK(warning.GetMatchCount() == 1);
			gpu.GetDevice().WaitForIdle();
		}

		TEST_CASE("Wireframe: solid picking remains unchanged while mesh interiors become background")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			GpuResourceCache cache(gpu.GetDevice(), assets);
			auto pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE(pipelines.has_value());
			auto renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets, { 64, 64 });
			REQUIRE(renderer.has_value());
			auto snapshot = Utils::WireframeTestSnapshot(assets, false, false, false);
			snapshot.Flags = RenderViewFlags::Picking;
			snapshot.Post.BloomEnabled = false;
			snapshot.Post.FxaaEnabled = false;
			snapshot.Camera.ClearColor = glm::vec3(0.0f);
			snapshot.Camera.ClearToSkybox = false;
			std::array<Image, 2> ids;
			for (size_t frame = 0; frame < 2; ++frame)
			{
				snapshot.FrameIndex = frame + 1;
				if (frame == 1)
					snapshot.Flags |= RenderViewFlags::Wireframe;
				auto commands = gpu.GetDevice().CreateCommandList();
				REQUIRE(commands.has_value());
				(*commands)->open();
				const auto recorded = (*renderer)->Render(**commands, snapshot);
				(*commands)->close();
				(*renderer)->OnSubmitted(snapshot.FrameIndex, gpu.GetDevice().ExecuteCommandList(**commands));
				REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
				REQUIRE((*renderer)->GetEntityIdTexture() != nullptr);
				Readback readback(gpu.GetDevice());
				const auto pixels = readback.ReadTexture(*(*renderer)->GetEntityIdTexture());
				REQUIRE(pixels.has_value());
				ids[frame] = *pixels;
				uint32_t centerId = 0;
				std::memcpy(&centerId, pixels->Pixels.data() + (32 * 64 + 32) * sizeof(uint32_t), sizeof(centerId));
				CHECK(centerId == 1); // Equal all-background images must not satisfy the solid-picking oracle.
				if (frame == 1)
				{
					const auto color = readback.ReadTexture(*(*renderer)->GetFinalTexture());
					REQUIRE(color.has_value());
					const size_t center = (32 * 64 + 32) * 4;
					CHECK(color->Pixels[center] == std::byte{ 0 });
					CHECK(color->Pixels[center + 1] == std::byte{ 0 });
					CHECK(color->Pixels[center + 2] == std::byte{ 0 });
					uint32_t whitePixels = 0;
					for (size_t pixel = 0; pixel < color->Pixels.size(); pixel += 4)
						if (color->Pixels[pixel] == std::byte{ 255 } && color->Pixels[pixel + 1] == std::byte{ 255 } && color->Pixels[pixel + 2] == std::byte{ 255 })
							++whitePixels;
					CHECK(whitePixels > 20);
				}
			}
			CHECK(ids[0].Pixels == ids[1].Pixels);
			gpu.GetDevice().WaitForIdle();
		}
	}

}
