#include "TestsPCH.h"
#include "Engine/Renderer/PlanarReflections.h"

#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Shared/ViewConstants.h"
#include "Support/BufferCreationFails.h"
#include "Support/DeviceLossTest.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/RenderReference.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

// The planar reflections' GPU path (M42, Docs/Decisions/0059-m42-planar-reflections.md decision 3; Renderer/PlanarReflections.h,
// SceneRendererReflections.cpp). The fixture's 64 x 64 view looks down -Z from the origin; its 2 x 2 quad, at z = -3 facing +Z,
// is a polished metal mirror (roughness clamped to 0.045) with a PlanarReflection on its own entity at full resolution, so
// each pixel on the glass samples the reflection at a texel centre. The scene has no ambient light, so the glass shows its
// reflection alone; two emitting markers, red at x = -0.8 and green at x = +0.8, stand behind the camera at z = +2, facing
// the mirror, and only the mirror shows them. The cases read SceneColor (linear HDR radiance) back.

namespace Engine {

	namespace {

		constexpr AssetHandle QuadMeshHandle{ 8910 }; // the fixture's quad
		constexpr AssetHandle MirrorMaterialHandle{ 0x5942000000000001ull };
		constexpr AssetHandle TwoSidedMirrorMaterialHandle{ 0x5942000000000002ull };
		constexpr AssetHandle RedMaterialHandle{ 0x5942000000000003ull };
		constexpr AssetHandle GreenMaterialHandle{ 0x5942000000000004ull };
		constexpr AssetHandle BlueMaterialHandle{ 0x5942000000000005ull };
		constexpr AssetHandle WallMaterialHandle{ 0x5942000000000006ull };
		constexpr AssetHandle StraddlerMeshHandle{ 0x5942000000000007ull };
		constexpr AssetHandle StripMeshHandle{ 0x5942000000000008ull };
		constexpr uint64_t MirrorEntity = 900;
		constexpr uint32_t Size = 64;
		constexpr glm::vec3 ClearColor(0.1f, 0.2f, 0.3f);

		AssetRef<Asset> MakeMaterial(const glm::vec3& baseColor, float metallic, float roughness, const glm::vec3& emissive, bool doubleSided)
		{
			MaterialData material;
			material.BaseColor = glm::vec4(baseColor, 1.0f);
			material.Metallic = metallic;
			material.Roughness = roughness;
			material.Emissive = emissive;
			material.DoubleSided = doubleSided;
			return CreateRef<MaterialData>(std::move(material));
		}

		// Two quads in one submesh: a 2 x 2 m quad at z = 0 and a tiny one at (30, 0, 2), so its bounds reach 2 m in front of
		// what lies 2 m behind it.
		Ref<MeshData> MakeStraddler()
		{
			Ref<MeshData> mesh = CreateRef<MeshData>();
			const auto quad = [&mesh](const glm::vec3& centre, float half)
			{
				const uint32_t first = static_cast<uint32_t>(mesh->Vertices.size());
				for (const glm::vec2 corner : { glm::vec2(-1.0f, -1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(-1.0f, 1.0f) })
				{
					const glm::vec3 position = centre + glm::vec3(corner * half, 0.0f);
					mesh->Vertices.push_back({ .Position = position, .Normal = { 0.0f, 0.0f, 1.0f }, .Tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .TexCoord = { 0.0f, 0.0f } });
					mesh->Bounds.Extend(position);
				}
				for (const uint32_t index : { 0U, 1U, 2U, 0U, 2U, 3U })
					mesh->Indices.push_back(first + index);
			};
			quad(glm::vec3(0.0f), 1.0f);
			quad(glm::vec3(30.0f, 0.0f, 2.0f), 0.05f);
			mesh->Submeshes = { { .IndexCount = static_cast<uint32_t>(mesh->Indices.size()), .Bounds = mesh->Bounds } };
			mesh->Slots = { {} };
			return mesh;
		}

		// SkinningPassTests.cpp's strip: two quads along +Y facing +Z, skinned to two joints.
		Ref<MeshData> MakeStrip()
		{
			Ref<MeshData> mesh = CreateRef<MeshData>();
			for (int row = 0; row < 3; ++row)
			{
				for (int column = 0; column < 2; ++column)
				{
					const glm::vec3 position(static_cast<float>(column) - 0.5f, static_cast<float>(row) - 1.0f, 0.0f);
					mesh->Vertices.push_back({ .Position = position, .Normal = { 0.0f, 0.0f, 1.0f }, .Tangent = { 1.0f, 0.0f, 0.0f, 1.0f }, .TexCoord = { 0.0f, 0.0f } });
					mesh->Bounds.Extend(position);
					const float top = static_cast<float>(row) / 2.0f;
					mesh->Skin.push_back({ .Joints = { 0, 1, 0, 0 }, .Weights = { 1.0f - top, top, 0.0f, 0.0f } });
				}
			}
			mesh->SkinJointCount = 2;
			mesh->Indices = { 0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4 };
			mesh->Submeshes.push_back({ .IndexOffset = 0, .IndexCount = 12, .MaterialSlot = 0, .Bounds = mesh->Bounds });
			mesh->Slots.push_back({ .Name = "Default" });
			return mesh;
		}

		void PublishAssets(Test::SceneRendererIntegrationFixture& fixture)
		{
			Test::InMemoryAssetManager& assets = fixture.Assets();
			assets.Publish(MirrorMaterialHandle, MakeMaterial(glm::vec3(1.0f), 1.0f, 0.0f, glm::vec3(0.0f), false));
			assets.Publish(TwoSidedMirrorMaterialHandle, MakeMaterial(glm::vec3(1.0f), 1.0f, 0.0f, glm::vec3(0.0f), true));
			assets.Publish(RedMaterialHandle, MakeMaterial(glm::vec3(0.0f), 0.0f, 1.0f, glm::vec3(2.0f, 0.0f, 0.0f), true));
			assets.Publish(GreenMaterialHandle, MakeMaterial(glm::vec3(0.0f), 0.0f, 1.0f, glm::vec3(0.0f, 2.0f, 0.0f), true));
			assets.Publish(BlueMaterialHandle, MakeMaterial(glm::vec3(0.0f), 0.0f, 1.0f, glm::vec3(0.0f, 0.0f, 2.0f), true));
			assets.Publish(WallMaterialHandle, MakeMaterial(glm::vec3(0.8f), 0.0f, 1.0f, glm::vec3(0.0f), true));
			assets.Publish(StraddlerMeshHandle, MakeStraddler());
			assets.Publish(StripMeshHandle, MakeStrip());
		}

		glm::mat4 MarkerWorld(float x)
		{
			return glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, 2.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.4f));
		}

		// A mirror quad (the fixture's) at `world` on entity `entity`, with its PlanarReflection on the quad's face.
		void AddMirror(RenderSnapshot& snapshot, const glm::mat4& world, uint64_t entity, float resolutionScale = 1.0f,
			AssetHandle material = MirrorMaterialHandle)
		{
			snapshot.Meshes.push_back(MeshDrawItem{ .World = world, .Mesh = QuadMeshHandle, .Materials = { material }, .CastShadows = false, .Entity = UUID(entity) });
			snapshot.PlanarReflections.push_back(
				PlanarReflectionItem{ .World = world, .Normal = glm::vec3(0.0f, 0.0f, 1.0f), .ResolutionScale = resolutionScale, .Entity = UUID(entity) });
		}

		// The fixture's view with no ambient light, the clear colour ClearColor, the Linear tonemapper and SSR off; the markers;
		// and, with `mirror`, the mirror at z = -3.
		RenderSnapshot MakeMirrorScene(Test::SceneRendererIntegrationFixture& fixture, bool mirror = true)
		{
			RenderSnapshot snapshot = fixture.Snapshot();
			snapshot.Meshes.clear();
			snapshot.Camera.ClearColor = ClearColor;
			snapshot.Environment = RenderEnvironment{ .Environment = {}, .Intensity = 1.0f, .ShowSkybox = false, .FallbackColor = glm::vec3(0.0f) };
			snapshot.Post.SsrEnabled = false;
			snapshot.Meshes.push_back(MeshDrawItem{ .World = MarkerWorld(-0.8f), .Mesh = QuadMeshHandle, .Materials = { RedMaterialHandle }, .Entity = UUID(1) });
			snapshot.Meshes.push_back(MeshDrawItem{ .World = MarkerWorld(0.8f), .Mesh = QuadMeshHandle, .Materials = { GreenMaterialHandle }, .Entity = UUID(2) });
			if (mirror)
				AddMirror(snapshot, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -3.0f)), MirrorEntity);
			return snapshot;
		}

		// The snapshot without its PlanarReflection items: the same meshes, every mirror drawn with its fallback.
		RenderSnapshot WithoutReflections(RenderSnapshot snapshot)
		{
			snapshot.PlanarReflections.clear();
			return snapshot;
		}

		// Renders `snapshot` into `fixture`'s renderer with a new frame index and returns Render's status.
		Status RenderStatus(GraphicsDevice& device, Test::SceneRendererIntegrationFixture& fixture, RenderSnapshot& snapshot)
		{
			snapshot.FrameIndex += 1;
			Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
			REQUIRE(list.has_value());
			(*list)->open();
			Status rendered = fixture.Renderer().Render(**list, snapshot);
			(*list)->close();
			fixture.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(**list));
			return rendered;
		}

		// SceneColor of the last render, as linear RGBA, row by row.
		std::vector<glm::dvec4> ReadSceneColor(GraphicsDevice& device, SceneRenderer& renderer)
		{
			Readback readback(device);
			const Result<Image> image = readback.ReadTexture(*renderer.GetSceneColorTexture());
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			std::vector<glm::dvec4> texels;
			for (uint32_t y = 0; y < image->Height; ++y)
			{
				const std::span<const std::byte> row = image->GetRow(y);
				for (uint32_t x = 0; x < image->Width; ++x)
				{
					std::array<uint16_t, 4> halves{};
					std::memcpy(halves.data(), row.data() + static_cast<size_t>(x) * sizeof(halves), sizeof(halves));
					texels.emplace_back(Test::HalfToDouble(halves[0]), Test::HalfToDouble(halves[1]), Test::HalfToDouble(halves[2]), Test::HalfToDouble(halves[3]));
				}
			}
			return texels;
		}

		// Renders `snapshot` (which must render without an error) and reads SceneColor back.
		std::vector<glm::dvec4> RenderSceneColor(GraphicsDevice& device, Test::SceneRendererIntegrationFixture& fixture, RenderSnapshot snapshot)
		{
			const Status rendered = RenderStatus(device, fixture, snapshot);
			REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
			return ReadSceneColor(device, fixture.Renderer());
		}

		glm::dvec3 Texel(const std::vector<glm::dvec4>& texels, uint32_t x, uint32_t y)
		{
			return glm::dvec3(texels[static_cast<size_t>(y) * Size + x]);
		}

		bool IsNear(const glm::dvec3& actual, const glm::dvec3& expected, double tolerance)
		{
			for (glm::length_t channel = 0; channel < 3; ++channel)
			{
				if (std::abs(actual[channel] - expected[channel]) > std::max(std::abs(expected[channel]) * tolerance, 2e-3))
					return false;
			}
			return true;
		}

		// The pixels of the glass at z = -3 (x and y in [-1, 1]: normalized device coordinates within 1 / (3 tan 30 degrees)),
		// two pixels in from its edges.
		constexpr uint32_t GlassMin = 16;
		constexpr uint32_t GlassMax = 48;

	}

	TEST_SUITE("Renderer")
	{
		// The mirror's glass shows what the camera mirrored in its plane sees: the same view as the main camera's of the scene
		// reflected through the plane (each marker's world matrix times the reflection, which renders mirrored), pixel for
		// pixel inside the glass except where a marker's edge falls on a pixel centre; the red marker on the left and the green
		// on the right, as a mirror shows them, and the clear colour around them.
		TEST_CASE("PlanarReflections: a mirror shows the view a camera mirrored in its plane sees" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				const std::vector<glm::dvec4> mirrored = RenderSceneColor(device, fixture, MakeMirrorScene(fixture));
				const RenderStats stats = fixture.Renderer().GetRenderStats();
				CHECK(stats.PlanarReflections == 1);
				CHECK(stats.DroppedPlanarReflections == 0);
				CHECK(stats.CulledPlanarReflections == 0);
				CHECK(stats.PlanarReflectionEntities == std::vector<UUID>{ UUID(MirrorEntity) });
				CHECK(Test::SceneRendererIntegrationFixture::FindPass(stats, "PlanarReflection0") != nullptr);

				RenderSnapshot reference = MakeMirrorScene(fixture, false);
				const glm::mat4 reflection = ComputePlanarReflectionMatrix(PlanarReflectionPlane{ .Normal = glm::vec3(0.0f, 0.0f, 1.0f), .Distance = 3.0f });
				for (MeshDrawItem& item : reference.Meshes)
					item.World = reflection * item.World;
				const std::vector<glm::dvec4> expected = RenderSceneColor(device, fixture, reference);

				uint32_t mismatched = 0;
				for (uint32_t y = GlassMin; y < GlassMax; ++y)
				{
					for (uint32_t x = GlassMin; x < GlassMax; ++x)
					{
						if (!IsNear(Texel(mirrored, x, y), Texel(expected, x, y), 1e-2))
							++mismatched;
					}
				}
				CHECK(mismatched <= 8);
				// The virtual red marker at (-0.8, 0, -8) is at pixel 26.5, the green at 37.5.
				const glm::dvec3 red = Texel(mirrored, 26, 31);
				const glm::dvec3 green = Texel(mirrored, 37, 31);
				CHECK(red.r > 1.5);
				CHECK(red.g < 0.1);
				CHECK(green.g > 1.5);
				CHECK(green.r < 0.1);
				CHECK(IsNear(Texel(mirrored, 32, 20), glm::dvec3(ClearColor), 2e-2));

				// Without the PlanarReflection the glass shows its fallback: the black ambient.
				const std::vector<glm::dvec4> fallback = RenderSceneColor(device, fixture, WithoutReflections(MakeMirrorScene(fixture)));
				CHECK(IsNear(Texel(fallback, 26, 31), glm::dvec3(0.0), 1e-2));
				CHECK(fixture.Renderer().GetRenderStats().PlanarReflections == 0);
			}
			device.RunGarbageCollection();
		}

		// A mesh whose bounds straddle the plane is drawn in the mirror view, and the clip at the plane removes its part behind
		// the glass: a 2 x 2 m blue quad 1 m behind the glass, which the glass hides from the main view and which would cover
		// the whole reflection, changes no pixel.
		TEST_CASE("PlanarReflections: nothing behind the plane appears in a mirror" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot plain = MakeMirrorScene(fixture);
				const Image without = fixture.Render(plain);
				const RenderStats withoutStats = fixture.Renderer().GetRenderStats();
				RenderSnapshot straddled = MakeMirrorScene(fixture);
				straddled.FrameIndex = plain.FrameIndex + 1;
				straddled.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -4.0f)),
					.Mesh = StraddlerMeshHandle,
					.Materials = { BlueMaterialHandle },
					.Entity = UUID(3) });
				const Image with = fixture.Render(straddled);
				const RenderStats withStats = fixture.Renderer().GetRenderStats();
				CHECK(with.Pixels == without.Pixels);
				// The mirror view drew it (its prepass and forward draws), so the clip, not the cull, removed it.
				const RenderPassStats* withRow = Test::SceneRendererIntegrationFixture::FindPass(withStats, "PlanarReflection0");
				const RenderPassStats* withoutRow = Test::SceneRendererIntegrationFixture::FindPass(withoutStats, "PlanarReflection0");
				REQUIRE(withRow != nullptr);
				REQUIRE(withoutRow != nullptr);
				CHECK(withRow->DrawCalls == withoutRow->DrawCalls + 2);
			}
			device.RunGarbageCollection();
		}

		// Five mirrors of five sizes in view: the four largest are drawn, largest first, and the fifth falls back, drawing
		// exactly as without its PlanarReflection (the same bytes), and the renderer logs the drop once.
		TEST_CASE("PlanarReflections: a view draws at most four reflections, and a mirror past the cap draws as without one" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeMirrorScene(fixture, false);
				// The smallest first in the snapshot, so the ranking, not the order, decides.
				const std::array<float, 5> scales = { 0.3f, 0.5f, 0.35f, 0.45f, 0.4f };
				for (size_t index = 0; index < scales.size(); ++index)
				{
					const float x = (static_cast<float>(index) - 2.0f) * 1.15f;
					AddMirror(snapshot, glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, -5.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(scales[index])),
						MirrorEntity + index, 0.25f);
				}
				const Image capped = fixture.Render(snapshot);
				const RenderStats stats = fixture.Renderer().GetRenderStats();
				CHECK(stats.PlanarReflections == 4);
				CHECK(stats.DroppedPlanarReflections == 1);
				CHECK(stats.CulledPlanarReflections == 0);
				CHECK(stats.PlanarReflectionEntities
					== std::vector<UUID>{ UUID(MirrorEntity + 1), UUID(MirrorEntity + 3), UUID(MirrorEntity + 4), UUID(MirrorEntity + 2) });
				for (const char* row : { "PlanarReflection0", "PlanarReflection1", "PlanarReflection2", "PlanarReflection3" })
					CHECK(Test::SceneRendererIntegrationFixture::FindPass(stats, row) != nullptr);

				RenderSnapshot withoutSmallest = snapshot;
				withoutSmallest.FrameIndex += 1;
				withoutSmallest.PlanarReflections.erase(withoutSmallest.PlanarReflections.begin());
				const Image uncapped = fixture.Render(withoutSmallest);
				CHECK(fixture.Renderer().GetRenderStats().DroppedPlanarReflections == 0);
				CHECK(capped.Pixels == uncapped.Pixels);
			}
			device.RunGarbageCollection();
		}

		// A mirror whose reflecting side faces away from the camera, and one outside the view, are culled: they cost no slot,
		// and the view draws exactly as without them.
		TEST_CASE("PlanarReflections: a mirror out of view or facing away renders no reflection" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot away = MakeMirrorScene(fixture);
				away.PlanarReflections.front().Normal = glm::vec3(0.0f, 0.0f, -1.0f);
				const Image awayImage = fixture.Render(away);
				const RenderStats awayStats = fixture.Renderer().GetRenderStats();
				CHECK(awayStats.PlanarReflections == 0);
				CHECK(awayStats.CulledPlanarReflections == 1);
				CHECK(Test::SceneRendererIntegrationFixture::FindPass(awayStats, "PlanarReflection0") == nullptr);
				RenderSnapshot plain = WithoutReflections(away);
				plain.FrameIndex += 1;
				CHECK(fixture.Render(plain).Pixels == awayImage.Pixels);

				RenderSnapshot outside = MakeMirrorScene(fixture);
				outside.FrameIndex = plain.FrameIndex + 1;
				const glm::mat4 distant = glm::translate(glm::mat4(1.0f), glm::vec3(50.0f, 0.0f, -3.0f));
				outside.Meshes.back().World = distant;
				outside.PlanarReflections.front().World = distant;
				const Image outsideImage = fixture.Render(outside);
				const RenderStats outsideStats = fixture.Renderer().GetRenderStats();
				CHECK(outsideStats.PlanarReflections == 0);
				CHECK(outsideStats.CulledPlanarReflections == 1);
				RenderSnapshot outsidePlain = WithoutReflections(outside);
				outsidePlain.FrameIndex += 1;
				CHECK(fixture.Render(outsidePlain).Pixels == outsideImage.Pixels);
			}
			device.RunGarbageCollection();
		}

		// Mirror B, small and two-sided, stands in front of the large mirror A; both are drawn. A's reflection shows B's back
		// with B's fallback (a mirror view's records carry no reflection), so A's pixels showing B are the same whether B has
		// its PlanarReflection or not.
		TEST_CASE("PlanarReflections: a mirror seen in a mirror shows its fallback" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeMirrorScene(fixture, false);
				snapshot.Environment.FallbackColor = glm::vec3(0.25f); // B's fallback: the ambient grey
				AddMirror(snapshot, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -4.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(3.0f)), MirrorEntity);
				AddMirror(snapshot, glm::translate(glm::mat4(1.0f), glm::vec3(-0.7f, 0.4f, -2.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.25f)),
					MirrorEntity + 1, 1.0f, TwoSidedMirrorMaterialHandle);
				const std::vector<glm::dvec4> both = RenderSceneColor(device, fixture, snapshot);
				CHECK(fixture.Renderer().GetRenderStats().PlanarReflections == 2);
				RenderSnapshot aOnly = snapshot;
				aOnly.PlanarReflections.pop_back();
				const std::vector<glm::dvec4> single = RenderSceneColor(device, fixture, aOnly);
				CHECK(fixture.Renderer().GetRenderStats().PlanarReflections == 1);
				// B's image in A: the virtual B at (-0.7, 0.4, -6) spans pixels 23 to 28 across and rows 26 to 30; B itself
				// covers pixels 6 to 19 across, apart from it.
				for (uint32_t y = 25; y <= 31; ++y)
				{
					for (uint32_t x = 22; x <= 29; ++x)
						CHECK(Texel(both, x, y) == Texel(single, x, y));
				}
				// B's back in A shows its fallback grey, not the clear colour around it.
				CHECK(Texel(both, 25, 28).r > 0.15);
				CHECK(std::abs(Texel(both, 25, 28).r - Texel(both, 25, 28).b) < 0.05);
			}
			device.RunGarbageCollection();
		}

		// SSR on: the prepass writes a zero SceneRoughness multiplier on a drawn reflection's glass, so SSR neither traces nor
		// composites there and the glass is the same with SSR on as off; without the PlanarReflection the glass reflects, so its
		// multiplier is not zero.
		TEST_CASE("PlanarReflections: SSR does not trace a planar mirror's pixels" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				const auto multiplierAt = [&device, &fixture](uint32_t x, uint32_t y)
				{
					Readback readback(device);
					const Result<Image> image = readback.ReadTexture(*fixture.Renderer().GetSceneRoughnessTexture());
					REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
					uint32_t word = 0;
					std::memcpy(&word, image->GetRow(y).data() + static_cast<size_t>(x) * 2 * sizeof(uint32_t), sizeof(word));
					return word & 0x00FFFFFFU;
				};
				RenderSnapshot snapshot = MakeMirrorScene(fixture);
				const std::vector<glm::dvec4> off = RenderSceneColor(device, fixture, snapshot);
				snapshot.Post.SsrEnabled = true;
				snapshot.Post.SsrMaxRoughness = 0.6f;
				const std::vector<glm::dvec4> on = RenderSceneColor(device, fixture, snapshot);
				CHECK(multiplierAt(32, 32) == 0);
				CHECK(multiplierAt(20, 40) == 0);
				for (uint32_t y = GlassMin; y < GlassMax; ++y)
				{
					for (uint32_t x = GlassMin; x < GlassMax; ++x)
						CHECK(Texel(on, x, y) == Texel(off, x, y));
				}
				static_cast<void>(RenderSceneColor(device, fixture, WithoutReflections(snapshot)));
				CHECK(multiplierAt(32, 32) != 0);
			}
			device.RunGarbageCollection();
		}

		// A skinned strip in front of the mirror, seen by the main view and by the mirror view: the Deform stage deforms it
		// once, and both views draw its deformed vertices.
		TEST_CASE("PlanarReflections: a skinned mesh deforms once for a view and its reflections" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeMirrorScene(fixture);
				static_cast<void>(fixture.Render(snapshot));
				const RenderStats plain = fixture.Renderer().GetRenderStats();
				const glm::mat4 turn(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.9f, 0.0f, -2.0f)),
					.Mesh = StripMeshHandle,
					.Materials = { RedMaterialHandle },
					.Entity = UUID(4),
					.JointPalette = { glm::mat4(1.0f), turn } });
				snapshot.FrameIndex += 1;
				static_cast<void>(fixture.Render(snapshot));
				const RenderStats stats = fixture.Renderer().GetRenderStats();
				CHECK(stats.SkinnedMeshes == 1);
				const RenderPassStats* skinning = Test::SceneRendererIntegrationFixture::FindPass(stats, "Skinning");
				REQUIRE(skinning != nullptr);
				CHECK(skinning->Dispatches == 1);
				// Both views drew it: the main view's forward pass and the mirror view's prepass and forward pass.
				const RenderPassStats* row = Test::SceneRendererIntegrationFixture::FindPass(stats, "PlanarReflection0");
				const RenderPassStats* plainRow = Test::SceneRendererIntegrationFixture::FindPass(plain, "PlanarReflection0");
				REQUIRE(row != nullptr);
				REQUIRE(plainRow != nullptr);
				CHECK(row->DrawCalls == plainRow->DrawCalls + 2);
				CHECK(stats.VisibleMeshes == plain.VisibleMeshes + 1);
			}
			device.RunGarbageCollection();
		}

		// A mirror on the left, facing +X, reflects a lit wall at z = -7 inside the main view's cascades, on which an occluder
		// off to the right casts the directional light's shadow: the reflection of the shadowed wall is dark, and bright when
		// the occluder casts none, since the mirror view samples the main view's cascades.
		TEST_CASE("PlanarReflections: the main view's shadows shade a reflection" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeMirrorScene(fixture, false);
				snapshot.Meshes.clear();
				snapshot.Lights = { LightData{ .Type = RenderLightType::Directional,
					.Intensity = 3.0f,
					.Direction = glm::normalize(glm::vec3(-0.6f, 0.0f, -0.8f)),
					.CastShadows = true,
					.ShadowDistance = 15.0f } };
				const glm::mat4 faceX = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -7.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(4.0f)),
					.Mesh = QuadMeshHandle,
					.Materials = { WallMaterialHandle },
					.CastShadows = false,
					.Entity = UUID(5) });
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.9f, 0.0f, -5.5f)) * faceX * glm::scale(glm::mat4(1.0f), glm::vec3(0.5f)),
					.Mesh = QuadMeshHandle,
					.Materials = { WallMaterialHandle },
					.Entity = UUID(6) });
				AddMirror(snapshot, glm::translate(glm::mat4(1.0f), glm::vec3(-1.2f, 0.0f, -4.0f)) * faceX, MirrorEntity);
				const std::vector<glm::dvec4> shadowed = RenderSceneColor(device, fixture, snapshot);
				CHECK(fixture.Renderer().GetRenderStats().PlanarReflections == 1);
				snapshot.Meshes[1].CastShadows = false;
				const std::vector<glm::dvec4> lit = RenderSceneColor(device, fixture, snapshot);
				// The wall's point (-0.2, 0, -7), in the occluder's shadow, seen in the mirror at pixel (14, 31).
				const double inShadow = Texel(shadowed, 14, 31).r;
				const double inLight = Texel(lit, 14, 31).r;
				CHECK(inLight > 0.2);
				CHECK(inShadow < 0.5 * inLight);
			}
			device.RunGarbageCollection();
		}

		// The mirror turned 15 degrees about Y between renders: each render reflects the pose its snapshot gives, so the
		// renderer's image of the turned mirror is the one it renders after rendering the first pose again, and differs from
		// the first pose's on the glass.
		TEST_CASE("PlanarReflections: a reflection of a moving plane follows its rendered pose" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot first = MakeMirrorScene(fixture);
				RenderSnapshot turned = first;
				const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -3.0f))
					* glm::rotate(glm::mat4(1.0f), glm::radians(15.0f), glm::vec3(0.0f, 1.0f, 0.0f));
				turned.Meshes.back().World = world;
				turned.PlanarReflections.front().World = world;
				const std::vector<glm::dvec4> turnedFirst = RenderSceneColor(device, fixture, turned);
				const std::vector<glm::dvec4> unturned = RenderSceneColor(device, fixture, first);
				const std::vector<glm::dvec4> turnedAgain = RenderSceneColor(device, fixture, turned);
				CHECK(turnedAgain == turnedFirst);
				uint32_t differing = 0;
				for (uint32_t y = GlassMin; y < GlassMax; ++y)
				{
					for (uint32_t x = GlassMin; x < GlassMax; ++x)
					{
						if (!IsNear(Texel(turnedFirst, x, y), Texel(unturned, x, y), 1e-2))
							++differing;
					}
				}
				CHECK(differing > 20);
			}
			device.RunGarbageCollection();
		}

		// The per-slot targets, buffers and terrain state live in the renderer's state (the placement rule's place 2) and the
		// mip chain's pipeline and the neutral reflection in the pipeline set (place 1): the recovery releases and creates them
		// again, and the mirror scene renders the same bytes.
		TEST_CASE("PlanarReflections: a mirror scene renders the same bytes after a device loss" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
			PublishAssets(fixture);
			RenderSnapshot snapshot = MakeMirrorScene(fixture);
			GraphicsDevice& device = gpu.GetDevice();
			Test::DeviceLossTest test;
			test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
			test.Use = [&device, &fixture, &snapshot]() -> Result<std::vector<std::byte>>
			{
				std::vector<std::byte> pixels;
				for (uint32_t slot = 0; slot < device.GetFramesInFlight(); ++slot)
				{
					snapshot.FrameIndex += 1;
					ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
					list->open();
					const Status rendered = fixture.Renderer().Render(*list, snapshot);
					list->close();
					fixture.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(*list));
					ENGINE_TRY(rendered);
					if (fixture.Renderer().GetRenderStats().PlanarReflections != 1)
						return MakeError(ErrorCode::InvalidState, "the mirror was not drawn");
					nvrhi::ITexture* image = fixture.Renderer().GetFinalTexture();
					if (image == nullptr)
						return MakeError(ErrorCode::InvalidState, "the renderer has no final texture");
					Readback readback(device);
					ENGINE_TRY_ASSIGN(Image read, readback.ReadTexture(*image));
					pixels = std::move(read.Pixels);
				}
				return pixels;
			};
			// The view's targets reach their steady state in the pool first, as the other renderer loss cases do.
			for (uint32_t warmUp = 0; warmUp <= RenderTargetPool::DefaultKeepFrames; ++warmUp)
			{
				const Result<std::vector<std::byte>> steady = test.Use();
				REQUIRE_MESSAGE(steady.has_value(), steady.error().ToString());
			}
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
		}

		// A reflection whose objects cannot be created (its mirror view's constants run out of memory) falls back for that
		// render, which draws exactly as without the PlanarReflection and returns the error (the host trims after the frame,
		// Architecture §8.14 item 7), counted as dropped; the next render draws it.
		TEST_CASE("PlanarReflections: a reflection that runs out of memory falls back for the render, which returns the error" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeMirrorScene(fixture);
				const Image fallback = fixture.Render(WithoutReflections(snapshot));
				Status rendered;
				{
					// The mirror view's ViewConstants are the only constant buffers of their size this render creates.
					const Test::BufferCreationFails fails([](const VkBufferCreateInfo& info)
					{
						return info.size == sizeof(ViewConstants) && (info.usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) != 0;
					});
					rendered = RenderStatus(device, fixture, snapshot);
					CHECK(fails.GetFailedCount() == 1);
				}
				CHECK_FALSE(rendered.has_value());
				const RenderStats failed = fixture.Renderer().GetRenderStats();
				CHECK(failed.PlanarReflections == 0);
				CHECK(failed.DroppedPlanarReflections == 1);
				CHECK(fixture.Read().Pixels == fallback.Pixels);

				const Status recovered = RenderStatus(device, fixture, snapshot);
				CHECK(recovered.has_value());
				CHECK(fixture.Renderer().GetRenderStats().PlanarReflections == 1);
				CHECK(fixture.Read().Pixels != fallback.Pixels);
			}
			device.RunGarbageCollection();
		}
	}

}
