#include "TestsPCH.h"
#include "Engine/Renderer/PlanarReflections.h"

#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/PlanarReflectionPass.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Shared/ProbeConstants.h"
#include "Shared/ViewConstants.h"
#include "Support/BindingSetCreationFails.h"
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
		constexpr AssetHandle RedGlassMaterialHandle{ 0x5942000000000009ull };
		constexpr AssetHandle BlueGlassMaterialHandle{ 0x594200000000000aull };
		constexpr AssetHandle WhiteMaterialHandle{ 0x594200000000000bull };
		constexpr AssetHandle SkyEnvironmentHandle{ 0x594200000000000cull };
		constexpr AssetHandle HeightfieldHandle{ 0x594200000000000dull };
		constexpr AssetHandle ParticleTextureHandle{ 0x594200000000000eull };
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

		// A double-sided transparent (Blend) material at alpha 0.5 that emits `emissive`.
		AssetRef<Asset> MakeGlassMaterial(const glm::vec3& emissive)
		{
			MaterialData material;
			material.BaseColor = glm::vec4(0.0f, 0.0f, 0.0f, 0.5f);
			material.Metallic = 0.0f;
			material.Roughness = 1.0f;
			material.Emissive = emissive;
			material.AlphaMode = AlphaMode::Blend;
			material.DoubleSided = true;
			return CreateRef<MaterialData>(std::move(material));
		}

		// An environment whose skybox has one texel a face (binary16), each face its own colour, with a black prefiltered cube
		// and no irradiance, so the sky a view shows depends on where it looks.
		AssetRef<Asset> MakeSkyEnvironment()
		{
			constexpr uint32_t FaceSize = 8;
			constexpr uint32_t MipCount = 4;
			constexpr std::array<glm::dvec3, 6> Faces = { glm::dvec3(0.6, 0.2, 0.2), glm::dvec3(0.2, 0.6, 0.2), glm::dvec3(0.5, 0.5, 0.5),
				glm::dvec3(0.1, 0.1, 0.1), glm::dvec3(0.9, 0.5, 0.1), glm::dvec3(0.1, 0.3, 0.9) };
			EnvironmentData environment;
			environment.Skybox = { .FaceSize = 1, .MipCount = 1, .Texels = Buffer(ComputeCubeMapByteSize(1, 1)) };
			for (size_t face = 0; face < Faces.size(); ++face)
			{
				const std::array<uint16_t, 4> texel = { Test::DoubleToHalf(Faces[face].x), Test::DoubleToHalf(Faces[face].y), Test::DoubleToHalf(Faces[face].z),
					Test::DoubleToHalf(1.0) };
				std::memcpy(environment.Skybox.Texels.data() + face * sizeof(texel), texel.data(), sizeof(texel));
			}
			environment.Specular = { .FaceSize = FaceSize, .MipCount = MipCount, .Texels = Buffer(ComputeCubeMapByteSize(FaceSize, MipCount), std::byte{ 0 }) };
			return CreateRef<EnvironmentData>(std::move(environment));
		}

		// A heightfield of 33 samples a side in gentle diagonal waves (up to 0.92 of its height scale).
		AssetRef<Asset> MakeWavyHeightfield()
		{
			constexpr uint32_t Resolution = 33;
			HeightfieldData heightfield = MakeFlatHeightfield(Resolution);
			for (uint32_t z = 0; z < Resolution; ++z)
			{
				for (uint32_t x = 0; x < Resolution; ++x)
					heightfield.Heights[x + static_cast<size_t>(Resolution) * z] = static_cast<uint16_t>(((x * 7 + z * 5) % 16) * 4000);
			}
			return CreateRef<HeightfieldData>(std::move(heightfield));
		}

		// 8 x 8 opaque white texels, which a particle's Color tints.
		AssetRef<Asset> MakeParticleTexture()
		{
			constexpr uint32_t Texels = 8;
			Ref<TextureData> texture = CreateRef<TextureData>();
			texture->Format = TextureFormat::RGBA8Srgb;
			texture->Width = Texels;
			texture->Height = Texels;
			texture->Mips = { TextureMip{ .Width = Texels, .Height = Texels, .Offset = 0, .Size = Texels * Texels * 4 } };
			for (uint32_t texel = 0; texel < Texels * Texels * 4; ++texel)
				texture->Pixels.push_back(std::byte{ 255 });
			return texture;
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
			assets.Publish(RedGlassMaterialHandle, MakeGlassMaterial(glm::vec3(2.0f, 0.0f, 0.0f)));
			assets.Publish(BlueGlassMaterialHandle, MakeGlassMaterial(glm::vec3(0.0f, 0.0f, 2.0f)));
			assets.Publish(WhiteMaterialHandle, MakeMaterial(glm::vec3(0.0f), 0.0f, 1.0f, glm::vec3(1.0f), true));
			assets.Publish(SkyEnvironmentHandle, MakeSkyEnvironment());
			assets.Publish(HeightfieldHandle, MakeWavyHeightfield());
			assets.Publish(ParticleTextureHandle, MakeParticleTexture());
		}

		// The built-in quad's world matrix turned to face +Y (up), scaled `size` along X and Z, centred on `centre`.
		glm::mat4 FloorWorld(const glm::vec3& centre, const glm::vec2& size)
		{
			return glm::translate(glm::mat4(1.0f), centre) * glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f))
				* glm::scale(glm::mat4(1.0f), glm::vec3(size.x, size.y, 1.0f));
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

		// The glass's plane in MakeMirrorScene: z = -3, facing +Z.
		constexpr PlanarReflectionPlane MirrorPlane{ .Normal = glm::vec3(0.0f, 0.0f, 1.0f), .Distance = 3.0f };

		// `snapshot` rendered from the camera mirrored in MirrorPlane (MakePlanarReflectionCamera at the view's size), without
		// the mirror's draw and without any PlanarReflection: what a full-resolution mirror view renders before its mip chain,
		// for a fogless scene with nothing between the mirror camera and the glass. The main view's glass pixel (x, y) samples
		// its texel (Size - 1 - x, y).
		std::vector<glm::dvec4> RenderMirrorCameraView(GraphicsDevice& device, Test::SceneRendererIntegrationFixture& fixture, RenderSnapshot snapshot)
		{
			snapshot.Camera = MakePlanarReflectionCamera(snapshot.Camera, MirrorPlane, Size, Size);
			std::erase_if(snapshot.Meshes, [](const MeshDrawItem& item)
			{
				return item.Entity == UUID(MirrorEntity);
			});
			snapshot.PlanarReflections.clear();
			return RenderSceneColor(device, fixture, std::move(snapshot));
		}

		// The glass's pixels of `view` that differ by more than `tolerance` from `mirrorView`'s (RenderMirrorCameraView's)
		// mirrored left for right.
		uint32_t CountGlassMismatches(const std::vector<glm::dvec4>& view, const std::vector<glm::dvec4>& mirrorView, double tolerance)
		{
			uint32_t mismatched = 0;
			for (uint32_t y = GlassMin; y < GlassMax; ++y)
			{
				for (uint32_t x = GlassMin; x < GlassMax; ++x)
				{
					if (!IsNear(Texel(view, x, y), Texel(mirrorView, Size - 1 - x, y), tolerance))
						++mismatched;
				}
			}
			return mismatched;
		}

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

		// A 6 x 6 m mirror at z = -6 facing the camera and a floor at y = -1 before it (z from -6 to -2), on which a 2 x 2 m
		// occluder above and to the left casts the directional light's shadow (x from -1 to 1, z from -5.5 to -3.5); the main
		// view's cascades end at 6.5 m. The floor's point (0, -1, -4.6), seen in the glass at pixel (32, 39), lies 4.6 m from
		// the main camera, inside the cascades, and its mirror image 7.4 m, beyond them: the mirror view chooses the cascade by
		// the main camera's depth (ViewConstants::CascadeDepthPlane), so the reflection is shadowed, and bright when the
		// occluder casts none. A mirror view that chose it by its own view depth would leave the point lit.
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
					.Direction = glm::normalize(glm::vec3(1.0f, -1.0f, 0.0f)),
					.CastShadows = true,
					.ShadowDistance = 6.5f } };
				snapshot.Meshes.push_back(MeshDrawItem{ .World = FloorWorld(glm::vec3(0.0f, -1.0f, -4.0f), glm::vec2(3.0f, 2.0f)),
					.Mesh = QuadMeshHandle,
					.Materials = { WallMaterialHandle },
					.CastShadows = false,
					.Entity = UUID(5) });
				snapshot.Meshes.push_back(MeshDrawItem{ .World = FloorWorld(glm::vec3(-2.0f, 1.0f, -4.5f), glm::vec2(1.0f)),
					.Mesh = QuadMeshHandle,
					.Materials = { WallMaterialHandle },
					.Entity = UUID(6) });
				AddMirror(snapshot, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -6.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(3.0f)), MirrorEntity);
				const std::vector<glm::dvec4> shadowed = RenderSceneColor(device, fixture, snapshot);
				CHECK(fixture.Renderer().GetRenderStats().PlanarReflections == 1);
				snapshot.Meshes[1].CastShadows = false;
				const std::vector<glm::dvec4> lit = RenderSceneColor(device, fixture, snapshot);
				const double inShadow = Texel(shadowed, 32, 39).r;
				const double inLight = Texel(lit, 32, 39).r;
				CHECK(inLight > 0.2);
				CHECK(inShadow < 0.25 * inLight);
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

		// The view's probe constants, which every forward set binds at b4, fail to create on the renderer's first render: the
		// main view's forward passes are skipped (ProbeShading.h), and the reflection falls back with them, counted as dropped
		// with the render returning the error, rather than drawing with b4 unwritten; the next render creates them and draws
		// it.
		TEST_CASE("PlanarReflections: a view whose probe constants cannot be created draws no reflection, and the next render draws it"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeMirrorScene(fixture);
				Status rendered;
				{
					// The probe constants are the only constant buffers of their size the first render creates.
					const Test::BufferCreationFails fails([](const VkBufferCreateInfo& info)
					{
						return info.size == sizeof(ProbeConstants) && (info.usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) != 0;
					});
					rendered = RenderStatus(device, fixture, snapshot);
					CHECK(fails.GetFailedCount() == 1);
				}
				REQUIRE_FALSE(rendered.has_value());
				CHECK(rendered.error().GetCode() == ErrorCode::Gpu);
				const RenderStats failed = fixture.Renderer().GetRenderStats();
				CHECK(failed.PlanarReflections == 0);
				CHECK(failed.DroppedPlanarReflections == 1);
				CHECK(failed.PlanarReflectionEntities.empty());
				CHECK(Test::SceneRendererIntegrationFixture::FindPass(failed, "PlanarReflection0") == nullptr);

				const Status recovered = RenderStatus(device, fixture, snapshot);
				CHECK(recovered.has_value());
				const RenderStats drawn = fixture.Renderer().GetRenderStats();
				CHECK(drawn.PlanarReflections == 1);
				CHECK(Test::SceneRendererIntegrationFixture::FindPass(drawn, "PlanarReflection0") != nullptr);
			}
			device.RunGarbageCollection();
		}

		// The skinned strip in front of the mirror, seen by both views, whose deformed vertices cannot be created (an
		// out-of-memory request): neither the main view nor the mirror view draws it, and the render returns the error; the
		// next render deforms it once and draws it in both.
		TEST_CASE("PlanarReflections: a deformed item whose deformation cannot be created is drawn by neither the view nor its reflection"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeMirrorScene(fixture);
				const Status plainRendered = RenderStatus(device, fixture, snapshot);
				REQUIRE(plainRendered.has_value());
				const RenderStats plain = fixture.Renderer().GetRenderStats();
				const RenderPassStats* plainRow = Test::SceneRendererIntegrationFixture::FindPass(plain, "PlanarReflection0");
				const RenderPassStats* plainForward = Test::SceneRendererIntegrationFixture::FindPass(plain, "ForwardOpaque");
				REQUIRE(plainRow != nullptr);
				REQUIRE(plainForward != nullptr);

				// At rest first (no palette, so nothing deforms), which uploads the strip's mesh.
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.9f, 0.0f, -2.0f)),
					.Mesh = StripMeshHandle,
					.Materials = { RedMaterialHandle },
					.Entity = UUID(4) });
				const Status rest = RenderStatus(device, fixture, snapshot);
				REQUIRE(rest.has_value());
				const glm::mat4 turn(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
				snapshot.Meshes.back().JointPalette = { glm::mat4(1.0f), turn };
				Status rendered;
				{
					// The deformed vertices are the only vertex buffers with storage views this render creates.
					const Test::BufferCreationFails fails([](const VkBufferCreateInfo& info)
					{
						return (info.usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) != 0 && (info.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0;
					});
					rendered = RenderStatus(device, fixture, snapshot);
					CHECK(fails.GetFailedCount() == 1);
				}
				REQUIRE_FALSE(rendered.has_value());
				CHECK(rendered.error().GetCode() == ErrorCode::Gpu);
				const RenderStats skipped = fixture.Renderer().GetRenderStats();
				CHECK(Test::SceneRendererIntegrationFixture::FindPass(skipped, "Skinning") == nullptr);
				CHECK(skipped.PlanarReflections == 1);
				const RenderPassStats* skippedRow = Test::SceneRendererIntegrationFixture::FindPass(skipped, "PlanarReflection0");
				const RenderPassStats* skippedForward = Test::SceneRendererIntegrationFixture::FindPass(skipped, "ForwardOpaque");
				REQUIRE(skippedRow != nullptr);
				REQUIRE(skippedForward != nullptr);
				CHECK(skippedRow->DrawCalls == plainRow->DrawCalls);
				CHECK(skippedForward->DrawCalls == plainForward->DrawCalls);

				const Status recovered = RenderStatus(device, fixture, snapshot);
				CHECK(recovered.has_value());
				const RenderStats drawn = fixture.Renderer().GetRenderStats();
				const RenderPassStats* drawnRow = Test::SceneRendererIntegrationFixture::FindPass(drawn, "PlanarReflection0");
				const RenderPassStats* skinning = Test::SceneRendererIntegrationFixture::FindPass(drawn, "Skinning");
				REQUIRE(drawnRow != nullptr);
				REQUIRE(skinning != nullptr);
				CHECK(drawnRow->DrawCalls == plainRow->DrawCalls + 2);
				CHECK(skinning->Dispatches == 1);
			}
			device.RunGarbageCollection();
		}

		// The skinned strip in front of the mirror, seen by both views, whose Skinning dispatch cannot be recorded (its binding
		// set runs out of memory, through the engine's fault before NVRHI): its vertices hold no deformation, so the Deform
		// stage skips its draws in the main view and in the mirror view (SkipDeformedDraws), and the render returns the error;
		// the next render deforms it and draws it in both.
		TEST_CASE("PlanarReflections: a deformed item whose skinning cannot be recorded is drawn by neither the view nor its reflection"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeMirrorScene(fixture);
				const Status plainRendered = RenderStatus(device, fixture, snapshot);
				REQUIRE(plainRendered.has_value());
				const RenderStats plain = fixture.Renderer().GetRenderStats();
				const RenderPassStats* plainRow = Test::SceneRendererIntegrationFixture::FindPass(plain, "PlanarReflection0");
				const RenderPassStats* plainForward = Test::SceneRendererIntegrationFixture::FindPass(plain, "ForwardOpaque");
				REQUIRE(plainRow != nullptr);
				REQUIRE(plainForward != nullptr);

				// At rest first (no palette, so nothing deforms), which uploads the strip's mesh.
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.9f, 0.0f, -2.0f)),
					.Mesh = StripMeshHandle,
					.Materials = { RedMaterialHandle },
					.Entity = UUID(4) });
				const Status rest = RenderStatus(device, fixture, snapshot);
				REQUIRE(rest.has_value());
				const glm::mat4 turn(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
				snapshot.Meshes.back().JointPalette = { glm::mat4(1.0f), turn };
				Status rendered;
				{
					// The Skinning pass's set is the only one with three raw-buffer reads and a raw-buffer write.
					const Test::BindingSetCreationFails fails(device, [](const nvrhi::BindingSetDesc& desc, const nvrhi::IBindingLayout& /*layout*/)
					{
						const auto count = [&desc](nvrhi::ResourceType type)
						{
							return std::ranges::count_if(desc.bindings, [type](const nvrhi::BindingSetItem& item)
							{
								return item.type == type;
							});
						};
						return count(nvrhi::ResourceType::RawBuffer_SRV) == 3 && count(nvrhi::ResourceType::RawBuffer_UAV) == 1;
					});
					rendered = RenderStatus(device, fixture, snapshot);
					CHECK(fails.GetFailedCount() == 1);
				}
				REQUIRE_FALSE(rendered.has_value());
				CHECK(rendered.error().GetCode() == ErrorCode::Gpu);
				const RenderStats skipped = fixture.Renderer().GetRenderStats();
				CHECK(Test::SceneRendererIntegrationFixture::FindPass(skipped, "Skinning") == nullptr);
				CHECK(skipped.PlanarReflections == 1);
				const RenderPassStats* skippedRow = Test::SceneRendererIntegrationFixture::FindPass(skipped, "PlanarReflection0");
				const RenderPassStats* skippedForward = Test::SceneRendererIntegrationFixture::FindPass(skipped, "ForwardOpaque");
				REQUIRE(skippedRow != nullptr);
				REQUIRE(skippedForward != nullptr);
				CHECK(skippedRow->DrawCalls == plainRow->DrawCalls);
				CHECK(skippedForward->DrawCalls == plainForward->DrawCalls);

				const Status recovered = RenderStatus(device, fixture, snapshot);
				CHECK(recovered.has_value());
				const RenderStats drawn = fixture.Renderer().GetRenderStats();
				const RenderPassStats* drawnRow = Test::SceneRendererIntegrationFixture::FindPass(drawn, "PlanarReflection0");
				const RenderPassStats* skinning = Test::SceneRendererIntegrationFixture::FindPass(drawn, "Skinning");
				REQUIRE(drawnRow != nullptr);
				REQUIRE(skinning != nullptr);
				CHECK(drawnRow->DrawCalls == plainRow->DrawCalls + 2);
				CHECK(skinning->Dispatches == 1);
			}
			device.RunGarbageCollection();
		}

		// A rough mirror blurs its reflection by the level curve (Common/PlanarReflections.slang: the level log2(alpha * height *
		// 0.5) of the GGX alpha = roughness^2) and fades it into the probe term over the upper half of the roughnesses up to
		// SsrMaxRoughness.
		//   - The blur: an emitting red panel behind the camera fills the left half of the mirror camera's view, its edge on the
		//     64 x 64 reflection's centre line (a texel boundary at every level), and the glass's middle row ramps from red to
		//     black over about 2^level pixels: none at roughness 0.05 (level 0), about 4 at 0.354 (level 2) and 8 at 0.5
		//     (level 3), with SsrMaxRoughness 1 giving all three the whole weight.
		//   - The fade: the panel fills the view and the fallback ambient (the probe term without an environment) is blue, so
		//     the glass's blue over its blue without the PlanarReflection is the fallback's share, 1 - weight, with the weight
		//     saturate((SsrMaxRoughness - roughness) / (SsrMaxRoughness / 2)): none at roughness 0.25, half at 0.45 and all at
		//     0.6, with SsrMaxRoughness 0.6.
		TEST_CASE("PlanarReflections: a rough mirror blurs its reflection by the level curve and fades into the probe term by SsrMaxRoughness"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				uint64_t nextMaterial = 0x5942000000000100ull;
				// The mirror scene with a glass of `roughness`, the red panel behind the camera from x = -6 to `panelRight`, the
				// fallback ambient `fallback` and SsrMaxRoughness `ssrMaxRoughness`, on a black background; without its
				// PlanarReflection unless `reflected`.
				const auto makeScene = [&fixture, &nextMaterial](float roughness, float ssrMaxRoughness, float panelRight, const glm::vec3& fallback, bool reflected)
				{
					const AssetHandle material(nextMaterial++);
					fixture.Assets().Publish(material, MakeMaterial(glm::vec3(1.0f), 1.0f, roughness, glm::vec3(0.0f), false));
					RenderSnapshot snapshot = MakeMirrorScene(fixture, false);
					snapshot.Meshes.clear();
					snapshot.Camera.ClearColor = glm::vec3(0.0f);
					snapshot.Environment.FallbackColor = fallback;
					snapshot.Post.SsrMaxRoughness = ssrMaxRoughness;
					const float halfWidth = (panelRight + 6.0f) * 0.5f;
					snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(panelRight - halfWidth, 0.0f, 2.0f))
							* glm::scale(glm::mat4(1.0f), glm::vec3(halfWidth, 6.0f, 1.0f)),
						.Mesh = QuadMeshHandle,
						.Materials = { RedMaterialHandle },
						.CastShadows = false,
						.Entity = UUID(5) });
					AddMirror(snapshot, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -3.0f)), MirrorEntity, 1.0f, material);
					if (!reflected)
						snapshot.PlanarReflections.clear();
					return snapshot;
				};

				// The blur: the pixels of the glass's middle row between 5% and 95% of the red plateau left of the edge.
				const auto rampWidth = [&device, &fixture, &makeScene](float roughness)
				{
					const std::vector<glm::dvec4> view = RenderSceneColor(device, fixture, makeScene(roughness, 1.0f, 0.0f, glm::vec3(0.0f), true));
					CHECK(fixture.Renderer().GetRenderStats().PlanarReflections == 1);
					const double plateau = Texel(view, GlassMin + 2, 31).r;
					CHECK(plateau > 0.5);
					uint32_t ramp = 0;
					for (uint32_t x = GlassMin; x < GlassMax; ++x)
					{
						const double red = Texel(view, x, 31).r;
						if (red > 0.05 * plateau && red < 0.95 * plateau)
							++ramp;
					}
					return ramp;
				};
				const uint32_t sharp = rampWidth(0.05f);
				const uint32_t level2 = rampWidth(0.35355f);
				const uint32_t level3 = rampWidth(0.5f);
				CAPTURE(sharp);
				CAPTURE(level2);
				CAPTURE(level3);
				CHECK(sharp <= 1);
				CHECK(level2 >= 3);
				CHECK(level2 <= 6);
				CHECK(level3 >= 6);
				CHECK(level3 <= 11);
				CHECK(level3 > level2);

				// The fade: the fallback's share of the glass's blue at its centre.
				const glm::vec3 blue(0.0f, 0.0f, 0.5f);
				const auto fallbackShare = [&device, &fixture, &makeScene, &blue](float roughness, double& red)
				{
					const std::vector<glm::dvec4> reflected = RenderSceneColor(device, fixture, makeScene(roughness, 0.6f, 6.0f, blue, true));
					const std::vector<glm::dvec4> fallback = RenderSceneColor(device, fixture, makeScene(roughness, 0.6f, 6.0f, blue, false));
					red = Texel(reflected, 32, 31).r;
					const double without = Texel(fallback, 32, 31).b;
					CHECK(without > 0.05);
					return Texel(reflected, 32, 31).b / without;
				};
				double wholeRed = 0.0;
				double halfRed = 0.0;
				double goneRed = 0.0;
				const double whole = fallbackShare(0.25f, wholeRed);
				const double half = fallbackShare(0.45f, halfRed);
				const double gone = fallbackShare(0.6f, goneRed);
				CAPTURE(whole);
				CAPTURE(half);
				CAPTURE(gone);
				CHECK(whole < 0.05);
				CHECK(half == doctest::Approx(0.5).epsilon(0.16));
				CHECK(gone > 0.95);
				CHECK(wholeRed > 0.5);
				CHECK(halfRed > 0.2 * wholeRed);
				CHECK(halfRed < 0.8 * wholeRed);
				CHECK(goneRed < 0.02);
			}
			device.RunGarbageCollection();
		}

		// A 7 x 5 reflection's whole chain (7 x 5, 3 x 2, 1 x 1), level 0 uploaded: each texel of a level is the mean of the
		// level above over its span, weighted by the area each texel of it covers, so the last column and row of an odd level
		// count. The reference is computed in double from the level read back above it; the levels are binary16.
		TEST_CASE("PlanarReflections: each level of a reflection's mip chain is the area mean of the whole level above, odd sizes included"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<Scope<PlanarReflectionPass>> pass = PlanarReflectionPass::Create(device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				constexpr glm::uvec2 Extent(7, 5);
				constexpr uint32_t Levels = 3;
				Result<nvrhi::TextureHandle> color = device.CreateTexture(PlanarReflectionPass::GetColorDesc(Extent.x, Extent.y, Levels));
				REQUIRE_MESSAGE(color.has_value(), color.error().ToString());
				// Red rises along x, green along y, and blue lights only the last column and the last row.
				std::vector<uint16_t> halves;
				for (uint32_t y = 0; y < Extent.y; ++y)
				{
					for (uint32_t x = 0; x < Extent.x; ++x)
					{
						const double blue = x == Extent.x - 1 || y == Extent.y - 1 ? 8.0 : 0.0;
						for (const double value : { static_cast<double>(x + 1), static_cast<double>(y + 1), blue, 1.0 })
							halves.push_back(Test::DoubleToHalf(value));
					}
				}
				Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
				REQUIRE(list.has_value());
				PassBindingCache bindings;
				RenderPassCounters counters;
				(*list)->open();
				(*list)->writeTexture(*color, 0, 0, halves.data(), static_cast<size_t>(Extent.x) * 4 * sizeof(uint16_t));
				const Status recorded = (*pass)->Record(**list, bindings, **color, counters);
				(*list)->close();
				REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
				device.ExecuteCommandList(**list);
				CHECK(counters.Dispatches == Levels - 1);

				Readback readback(device);
				const auto read = [&readback, &color](uint32_t mip, const glm::uvec2& size)
				{
					const Result<Image> image = readback.ReadTexture(**color, mip);
					REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
					REQUIRE(image->Width == size.x);
					REQUIRE(image->Height == size.y);
					std::vector<glm::dvec4> texels;
					for (uint32_t y = 0; y < size.y; ++y)
					{
						const std::span<const std::byte> row = image->GetRow(y);
						for (uint32_t x = 0; x < size.x; ++x)
						{
							std::array<uint16_t, 4> texel{};
							std::memcpy(texel.data(), row.data() + static_cast<size_t>(x) * sizeof(texel), sizeof(texel));
							texels.emplace_back(Test::HalfToDouble(texel[0]), Test::HalfToDouble(texel[1]), Test::HalfToDouble(texel[2]), Test::HalfToDouble(texel[3]));
						}
					}
					return texels;
				};
				// The share of destination texel i that source texel s covers on an axis of `source` texels reduced to
				// `destination`: their overlap over the span's length source / destination.
				const auto weight = [](uint32_t s, uint32_t i, uint32_t source, uint32_t destination)
				{
					const double span = static_cast<double>(source) / destination;
					const double overlap = std::min(s + 1.0, (i + 1.0) * span) - std::max(static_cast<double>(s), i * span);
					return std::max(overlap, 0.0) / span;
				};
				glm::uvec2 size = Extent;
				std::vector<glm::dvec4> above = read(0, size);
				for (uint32_t mip = 1; mip < Levels; ++mip)
				{
					const glm::uvec2 next(std::max(1U, size.x / 2), std::max(1U, size.y / 2));
					const std::vector<glm::dvec4> level = read(mip, next);
					for (uint32_t y = 0; y < next.y; ++y)
					{
						for (uint32_t x = 0; x < next.x; ++x)
						{
							glm::dvec4 expected(0.0);
							for (uint32_t sy = 0; sy < size.y; ++sy)
							{
								for (uint32_t sx = 0; sx < size.x; ++sx)
									expected += weight(sx, x, size.x, next.x) * weight(sy, y, size.y, next.y) * above[static_cast<size_t>(sy) * size.x + sx];
							}
							CAPTURE(mip);
							CAPTURE(x);
							CAPTURE(y);
							CHECK(IsNear(glm::dvec3(level[static_cast<size_t>(y) * next.x + x]), glm::dvec3(expected), 2e-3));
						}
					}
					above = level;
					size = next;
				}
			}
			device.RunGarbageCollection();
		}

		// A blue marker whose mirror image lies beside the glass (its centre 0.75 of the half-view left of the centre, the glass
		// reaching 0.58), inside the blur's margin: the mirror view culls to the glass's rectangle widened by the margin of its
		// chain (5 levels of the 64 x 64 reflection at SsrMaxRoughness 0.6: two 16-pixel texels), so it draws the marker (its
		// prepass and forward draws), whose pixels the glass's edge texels average at the blurred levels.
		TEST_CASE("PlanarReflections: a mirror view draws what its blur margin shows beside the glass" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeMirrorScene(fixture);
				snapshot.Post.SsrMaxRoughness = 0.6f;
				static_cast<void>(RenderSceneColor(device, fixture, snapshot));
				const RenderStats plainStats = fixture.Renderer().GetRenderStats();
				const RenderPassStats* plain = Test::SceneRendererIntegrationFixture::FindPass(plainStats, "PlanarReflection0");
				REQUIRE(plain != nullptr);
				CHECK(plain->Dispatches == 4);

				const float x = -0.75f * 8.0f * std::tan(glm::radians(30.0f));
				snapshot.Meshes.push_back(MeshDrawItem{ .World = MarkerWorld(x), .Mesh = QuadMeshHandle, .Materials = { BlueMaterialHandle }, .Entity = UUID(7) });
				static_cast<void>(RenderSceneColor(device, fixture, snapshot));
				const RenderStats besideStats = fixture.Renderer().GetRenderStats();
				const RenderPassStats* beside = Test::SceneRendererIntegrationFixture::FindPass(besideStats, "PlanarReflection0");
				REQUIRE(beside != nullptr);
				CHECK(beside->DrawCalls == plain->DrawCalls + 2);
				CHECK(beside->Triangles == plain->Triangles + 4);
			}
			device.RunGarbageCollection();
		}

		// A grey wall behind the camera (z = +2, facing the mirror) lit only by a point light 0.5 m in front of it, behind the
		// camera and outside the main view's frustum: the main view culls the light, and the mirror view adds it to its own
		// list, unshadowed, so the wall's reflection is lit, the same where a small occluder would shadow it; without the light
		// it is black.
		TEST_CASE("PlanarReflections: a light only the mirror camera sees lights the reflection, unshadowed" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeMirrorScene(fixture, false);
				snapshot.Meshes.clear();
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 2.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(3.0f)),
					.Mesh = QuadMeshHandle,
					.Materials = { WallMaterialHandle },
					.Entity = UUID(5) });
				AddMirror(snapshot, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -3.0f)), MirrorEntity);
				const std::vector<glm::dvec4> dark = RenderSceneColor(device, fixture, snapshot);
				snapshot.Lights = { LightData{ .Type = RenderLightType::Point,
					.Intensity = 4.0f,
					.Position = glm::vec3(0.0f, 0.0f, 1.5f),
					.Range = 1.5f,
					.CastShadows = true } };
				const std::vector<glm::dvec4> lit = RenderSceneColor(device, fixture, snapshot);
				const RenderStats stats = fixture.Renderer().GetRenderStats();
				CHECK(stats.PlanarReflections == 1);
				CHECK(stats.VisibleLights == 0);
				// An occluder between the light and the wall, whose shadow would fall on the wall at x = 0.8 to 1.2, seen in the
				// glass at pixel (38, 31); the occluder itself shows at pixels 35 and 36.
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, 0.0f, 1.75f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.1f)),
					.Mesh = QuadMeshHandle,
					.Materials = { WallMaterialHandle },
					.Entity = UUID(6) });
				const std::vector<glm::dvec4> occluded = RenderSceneColor(device, fixture, snapshot);
				CHECK(Texel(dark, 32, 31).r < 1e-3);
				CHECK(Texel(dark, 38, 31).r < 1e-3);
				CHECK(Texel(lit, 32, 31).r > 0.1);
				CHECK(Texel(lit, 38, 31).r > 0.02);
				CHECK(IsNear(Texel(occluded, 38, 31), Texel(lit, 38, 31), 1e-2));
			}
			device.RunGarbageCollection();
		}

		// Distance fog (no height fog, sun glow or ambient tint; the sky fogged) on the mirror scene: the mirror view fogs the
		// path from the glass to each marker and the main view the path from the camera to the glass, so the glass shows the
		// reflected scene under the same fog, as the main camera sees the markers' mirror images, and not the camera-to-glass
		// stretch twice.
		TEST_CASE("PlanarReflections: a reflection is fogged from the glass on, so a fogged mirror shows the reflected scene's fog"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				const RenderFog fog{ .Enabled = true, .Density = 0.15f, .MaxOpacity = 1.0f, .HeightDensity = 0.0f, .SunGlow = 0.0f, .FogSkybox = true };
				const std::vector<glm::dvec4> clear = RenderSceneColor(device, fixture, MakeMirrorScene(fixture));
				RenderSnapshot snapshot = MakeMirrorScene(fixture);
				snapshot.Fog = fog;
				const std::vector<glm::dvec4> mirrored = RenderSceneColor(device, fixture, snapshot);
				CHECK(fixture.Renderer().GetRenderStats().PlanarReflections == 1);

				RenderSnapshot reference = MakeMirrorScene(fixture, false);
				reference.Fog = fog;
				const glm::mat4 reflection = ComputePlanarReflectionMatrix(MirrorPlane);
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
				// The red marker, 8 m away along the reflected path, is fogged: e^-1.2 of its 2 plus the fog's colour.
				const double transmittance = std::exp(-0.15 * 8.0);
				CHECK(Texel(mirrored, 26, 31).r == doctest::Approx(2.0 * transmittance + fog.Color.r * (1.0 - transmittance)).epsilon(0.02));
				CHECK(Texel(clear, 26, 31).r > 1.5);
			}
			device.RunGarbageCollection();
		}

		// The skybox (a cube whose faces differ) in the mirror scene: the mirror view draws the sky along its own view, the
		// main view's mirrored, so the glass shows the sky behind the camera, as the mirror camera sees it, and not the sky
		// around the glass.
		TEST_CASE("PlanarReflections: a mirror shows the skybox along the mirrored view" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeMirrorScene(fixture);
				snapshot.Environment = RenderEnvironment{ .Environment = SkyEnvironmentHandle, .Intensity = 1.0f, .ShowSkybox = true, .FallbackColor = glm::vec3(0.0f) };
				snapshot.Camera.ClearToSkybox = true;
				const std::vector<glm::dvec4> view = RenderSceneColor(device, fixture, snapshot);
				CHECK(fixture.Renderer().GetRenderStats().PlanarReflections == 1);
				const std::vector<glm::dvec4> mirrorView = RenderMirrorCameraView(device, fixture, snapshot);
				CHECK(CountGlassMismatches(view, mirrorView, 1e-2) <= 8);
				// The glass's sky (above the markers) is the sky behind the camera, neither the clear colour nor the sky ahead.
				CHECK_FALSE(IsNear(Texel(view, 32, 20), glm::dvec3(ClearColor), 5e-2));
				CHECK_FALSE(IsNear(Texel(view, 32, 20), Texel(view, 32, 2), 5e-2));
			}
			device.RunGarbageCollection();
		}

		// Two overlapping transparent quads behind the camera, red at z = +1 and blue at z = +1.5 (nearer and farther for the
		// mirror camera, the other way round for the main camera's depth): the mirror view's ForwardTransparent sorts them for
		// the mirror camera, so the glass shows them blended as the mirror camera sees them.
		TEST_CASE("PlanarReflections: a mirror's transparent pass blends back to front for the mirror camera" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeMirrorScene(fixture);
				static_cast<void>(RenderSceneColor(device, fixture, snapshot));
				const RenderStats plainStats = fixture.Renderer().GetRenderStats();
				const RenderPassStats* plain = Test::SceneRendererIntegrationFixture::FindPass(plainStats, "PlanarReflection0");
				REQUIRE(plain != nullptr);
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 1.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.5f)),
					.Mesh = QuadMeshHandle,
					.Materials = { RedGlassMaterialHandle },
					.Entity = UUID(5) });
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.4f, 0.2f, 1.5f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.5f)),
					.Mesh = QuadMeshHandle,
					.Materials = { BlueGlassMaterialHandle },
					.Entity = UUID(6) });
				const std::vector<glm::dvec4> view = RenderSceneColor(device, fixture, snapshot);
				const RenderStats stats = fixture.Renderer().GetRenderStats();
				const RenderPassStats* row = Test::SceneRendererIntegrationFixture::FindPass(stats, "PlanarReflection0");
				REQUIRE(row != nullptr);
				CHECK(row->DrawCalls == plain->DrawCalls + 2);
				const std::vector<glm::dvec4> mirrorView = RenderMirrorCameraView(device, fixture, snapshot);
				CHECK(CountGlassMismatches(view, mirrorView, 1e-2) <= 8);
				// Where they overlap (the mirror images: red at pixels 28 to 36, blue at 31 to 38), the nearer red lies over the
				// blue.
				const glm::dvec3 overlap = Texel(view, 33, 31);
				CHECK(overlap.r > overlap.b);
				CHECK(overlap.b > 0.1);
			}
			device.RunGarbageCollection();
		}

		// A lit, wavy terrain under the mirror, below the glass's sight in the main view, which the mirror view draws with its
		// own terrain state and scissor (TerrainDepthNormal and TerrainForward): the glass shows it as the mirror camera sees it.
		TEST_CASE("PlanarReflections: a terrain shows in a mirror as the mirror camera sees it" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeMirrorScene(fixture);
				snapshot.Lights = { LightData{ .Type = RenderLightType::Directional, .Intensity = 1.0f, .Direction = glm::normalize(glm::vec3(0.3f, -1.0f, 0.2f)) } };
				TerrainItem terrain;
				terrain.World = glm::translate(glm::mat4(1.0f), glm::vec3(-4.0f, -1.6f, -2.5f));
				terrain.Heightfield = HeightfieldHandle;
				terrain.Size = glm::vec2(8.0f);
				terrain.HeightScale = 0.3f;
				terrain.Layers = { TerrainLayerItem{ .Tint = glm::vec3(0.2f, 0.7f, 0.25f), .Tiling = 3.0f, .Roughness = 0.8f } };
				terrain.Entity = UUID(0x7e44);
				snapshot.Terrains.push_back(std::move(terrain));
				const std::vector<glm::dvec4> view = RenderSceneColor(device, fixture, snapshot);
				CHECK(fixture.Renderer().GetRenderStats().PlanarReflections == 1);
				const std::vector<glm::dvec4> mirrorView = RenderMirrorCameraView(device, fixture, snapshot);
				CHECK(CountGlassMismatches(view, mirrorView, 1e-2) <= 8);
				// The glass's lower rows show the terrain, green, not the clear colour.
				const glm::dvec3 ground = Texel(view, 32, 44);
				CHECK(ground.g > ground.b);
				CHECK_FALSE(IsNear(ground, glm::dvec3(ClearColor), 5e-2));
			}
			device.RunGarbageCollection();
		}

		// Two particles behind the camera, which only the mirror view sees, drawn with the slot's plan, depth and scissor: the
		// glass shows them as the mirror camera sees them.
		TEST_CASE("PlanarReflections: particles show in a mirror as the mirror camera sees them" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeMirrorScene(fixture);
				const std::array<ParticleItem, 2> particles = { ParticleItem{ .Position = glm::vec3(-0.3f, 0.2f, 1.2f), .Size = 0.6f, .Color = glm::vec4(1.0f, 0.5f, 0.2f, 1.0f) },
					ParticleItem{ .Position = glm::vec3(0.3f, -0.1f, 1.6f), .Size = 0.5f, .Color = glm::vec4(0.2f, 0.6f, 1.0f, 0.7f) } };
				snapshot.ParticleEmitters.push_back(ParticleEmitterItem{ .Texture = ParticleTextureHandle,
					.Blend = RenderParticleBlend::Alpha,
					.First = 0,
					.Count = static_cast<uint32_t>(particles.size()),
					.Entity = UUID(70) });
				snapshot.Particles.insert(snapshot.Particles.end(), particles.begin(), particles.end());
				const std::vector<glm::dvec4> view = RenderSceneColor(device, fixture, snapshot);
				CHECK(fixture.Renderer().GetRenderStats().PlanarReflections == 1);
				const std::vector<glm::dvec4> mirrorView = RenderMirrorCameraView(device, fixture, snapshot);
				CHECK(CountGlassMismatches(view, mirrorView, 1e-2) <= 8);
				// The first particle's mirror image, at pixels 27 to 31 across and 28 to 32 down, shows its orange.
				const glm::dvec3 orange = Texel(view, 30, 30);
				CHECK(orange.r > 0.8);
				CHECK(orange.b < 0.4);
			}
			device.RunGarbageCollection();
		}

		// An emitting floor that crosses the glass's plane, at heights that move the junction across a pixel: the prepass and
		// the forward passes clip it at the plane with the same exact distance (Common/ClipPlane.slang), so its reflection meets
		// the floor with no pixel of the reflection's clear colour between them.
		TEST_CASE("PlanarReflections: geometry that crosses the plane meets its reflection without a gap" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				for (uint32_t step = 0; step < 8; ++step)
				{
					const float height = -0.5f + 0.007f * static_cast<float>(step);
					CAPTURE(height);
					RenderSnapshot snapshot = MakeMirrorScene(fixture, false);
					snapshot.Meshes.clear();
					snapshot.Meshes.push_back(MeshDrawItem{ .World = FloorWorld(glm::vec3(0.0f, height, 3.5f), glm::vec2(6.0f, 9.5f)),
						.Mesh = QuadMeshHandle,
						.Materials = { WhiteMaterialHandle },
						.Entity = UUID(5) });
					AddMirror(snapshot, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -3.0f)), MirrorEntity);
					const std::vector<glm::dvec4> view = RenderSceneColor(device, fixture, snapshot);
					REQUIRE(fixture.Renderer().GetRenderStats().PlanarReflections == 1);
					// From the floor's far edge's reflection (row 33) down past the junction (rows 40 to 42), every pixel of the
					// glass's columns is the floor's.
					uint32_t gaps = 0;
					for (uint32_t y = 35; y < 56; ++y)
					{
						for (uint32_t x = 18; x < 46; ++x)
						{
							if (Texel(view, x, y).r < 0.5)
								++gaps;
						}
					}
					CHECK(gaps == 0);
				}
			}
			device.RunGarbageCollection();
		}
	}

}
