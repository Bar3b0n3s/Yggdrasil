#include "TestsPCH.h"

#include "Engine/Renderer/ParticlePass.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Support/DeviceLossTest.h"
#include "Support/ExpectLog.h"
#include "Support/GlmApprox.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

// The Particles pass (M26, C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decision 7, stream P-B): the plan's quads facing
// the camera, sorted and culled on the CPU, and the pass drawn by the SceneRenderer in the Sprites stage, its loss test
// through the shared helper (decision 29); and M33's look through the particles' own program (Docs/Decisions/
// 0048-m33-particle-look.md decisions 4 to 6): alignment and rotation, flipbooks, soft particles and motion vectors. The gpu
// suite runs every GPU case under API 1.4 and again capped at 1.3.

namespace Engine {

	namespace {

		// 8 x 8 opaque white texels, which a particle's Color tints, and 8 x 8 texels of distinct sRGB values, texel (x, y)
		// (32 x, 32 y, 255 - 16 x, 255). The tests' own textures, so the cases do not depend on the built-in one.
		constexpr uint32_t ParticleTexels = 8;
		const AssetHandle WhiteParticleTextureHandle = UUID(8960);
		const AssetHandle PatternParticleTextureHandle = UUID(8961);

		Ref<TextureData> MakeParticleTexture(bool pattern)
		{
			Ref<TextureData> texture = CreateRef<TextureData>();
			texture->Format = TextureFormat::RGBA8Srgb;
			texture->Width = ParticleTexels;
			texture->Height = ParticleTexels;
			texture->Mips = { TextureMip{ .Width = ParticleTexels, .Height = ParticleTexels, .Offset = 0, .Size = ParticleTexels * ParticleTexels * 4 } };
			for (uint32_t y = 0; y < ParticleTexels; ++y)
			{
				for (uint32_t x = 0; x < ParticleTexels; ++x)
				{
					texture->Pixels.push_back(static_cast<std::byte>(pattern ? 32 * x : 255));
					texture->Pixels.push_back(static_cast<std::byte>(pattern ? 32 * y : 255));
					texture->Pixels.push_back(static_cast<std::byte>(pattern ? 255 - 16 * x : 255));
					texture->Pixels.push_back(std::byte{ 255 });
				}
			}
			return texture;
		}

		// A 64 x 64 view of a 16 x 16 pixel-art scene (scale 4, the 2D colour path: exact output bytes): an orthographic camera
		// at z = 10 looking down -Z whose view is 8 units high, so a unit is 2 scene pixels (8 view pixels) and world (x, y)
		// lands on scene pixel ((x + 4) * 2, (4 - y) * 2). No mesh: SceneDepth stays cleared to the far plane.
		RenderSnapshot MakeParticleSnapshot(const Test::SceneRendererIntegrationFixture& setup)
		{
			RenderSnapshot snapshot = setup.Snapshot(RenderProjection::Orthographic);
			snapshot.Meshes.clear();
			snapshot.PickTable.clear();
			snapshot.Output = RenderOutputSettings{ .SceneWidth = 16, .SceneHeight = 16, .PixelArt = true };
			snapshot.Camera.View = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -10.0f));
			snapshot.Camera.Position = glm::vec3(0.0f, 0.0f, 10.0f);
			snapshot.Camera.OrthographicSize = 4.0f;
			snapshot.Camera.ViewportWidth = 16;
			snapshot.Camera.ViewportHeight = 16;
			snapshot.Camera.Projection = ComputeReverseZProjection(RenderProjection::Orthographic, 60.0f, 4.0f, 0.1f, 20.0f, 16, 16);
			return snapshot;
		}

		// Appends an emitter of `particles` to `snapshot`.
		void AddEmitter(RenderSnapshot& snapshot, AssetHandle texture, RenderParticleBlend blend, std::span<const ParticleItem> particles, UUID entity)
		{
			snapshot.ParticleEmitters.push_back(ParticleEmitterItem{ .Texture = texture,
				.Blend = blend,
				.First = static_cast<uint32_t>(snapshot.Particles.size()),
				.Count = static_cast<uint32_t>(particles.size()),
				.Entity = entity });
			snapshot.Particles.insert(snapshot.Particles.end(), particles.begin(), particles.end());
		}

		glm::u8vec4 ParticlePixel(const Image& image, uint32_t x, uint32_t y)
		{
			const std::span<const std::byte> row = image.GetRow(y);
			const size_t offset = 4 * static_cast<size_t>(x);
			return glm::u8vec4(std::to_integer<uint8_t>(row[offset]), std::to_integer<uint8_t>(row[offset + 1]), std::to_integer<uint8_t>(row[offset + 2]),
				std::to_integer<uint8_t>(row[offset + 3]));
		}

		// The triangles the last render recorded under the Particles row (0 when it recorded no row).
		uint32_t ParticleTriangles(Test::SceneRendererIntegrationFixture& setup)
		{
			const RenderStats stats = setup.Renderer().GetRenderStats();
			const RenderPassStats* pass = Test::SceneRendererIntegrationFixture::FindPass(stats, "Particles");
			return pass != nullptr ? pass->Triangles : 0u;
		}

		// A camera looking at the origin from (0, 0, 10): view z is the world z minus 10.
		glm::mat4 MakeFrontView()
		{
			return glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -10.0f));
		}

		glm::mat4 MakeFrontViewProjection()
		{
			return ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 4.0f, 0.1f, 100.0f, 64, 64) * MakeFrontView();
		}

		// M33 (Docs/Decisions/0048-m33-particle-look.md decision 5): a 2 x 1 texture of an opaque red and an opaque green texel,
		// cut by a sheet into two one-texel frames (red, then green).
		const AssetHandle FlipbookTextureHandle = UUID(8962);
		const AssetHandle FlipbookSheetHandle = UUID(8963);

		Ref<TextureData> MakeFlipbookTexture()
		{
			Ref<TextureData> texture = CreateRef<TextureData>();
			texture->Format = TextureFormat::RGBA8Srgb;
			texture->Width = 2;
			texture->Height = 1;
			texture->Mips = { TextureMip{ .Width = 2, .Height = 1, .Offset = 0, .Size = 8 } };
			for (const int value : { 255, 0, 0, 255, 0, 255, 0, 255 })
				texture->Pixels.push_back(static_cast<std::byte>(value));
			return texture;
		}

		Ref<SpriteSheetData> MakeFlipbookSheet()
		{
			Ref<SpriteSheetData> sheet = CreateRef<SpriteSheetData>();
			sheet->Texture = TypedAssetHandle<AssetType::Texture>(FlipbookTextureHandle);
			sheet->Frames = { SpriteFrame{ .Rect = { .X = 0, .Y = 0, .Width = 1, .Height = 1 } }, SpriteFrame{ .Rect = { .X = 1, .Y = 0, .Width = 1, .Height = 1 } } };
			return sheet;
		}

		// The scene pixel of world position `position` through `viewProjection` in a `size` x `size` view (+x right, +y down).
		glm::vec2 ProjectParticleToPixel(const glm::mat4& viewProjection, const glm::vec3& position, uint32_t size)
		{
			const glm::vec4 clip = viewProjection * glm::vec4(position, 1.0f);
			const glm::vec2 ndc = glm::vec2(clip) / clip.w;
			return glm::vec2((ndc.x * 0.5f + 0.5f) * static_cast<float>(size), (0.5f - ndc.y * 0.5f) * static_cast<float>(size));
		}

		// The last render's SceneVelocity of the fixture's renderer, one motion vector per scene pixel.
		std::vector<glm::vec2> ReadParticleVelocity(GraphicsDevice& device, SceneRenderer& renderer)
		{
			nvrhi::ITexture* texture = renderer.GetSceneVelocityTexture();
			REQUIRE(texture != nullptr);
			Readback readback(device);
			const Result<Image> image = readback.ReadTexture(*texture);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			std::vector<uint16_t> halves(image->Pixels.size() / sizeof(uint16_t));
			std::memcpy(halves.data(), image->Pixels.data(), halves.size() * sizeof(uint16_t));
			std::vector<glm::vec2> motion;
			for (size_t index = 0; index + 1 < halves.size(); index += 2)
				motion.emplace_back(glm::unpackHalf1x16(halves[index]), glm::unpackHalf1x16(halves[index + 1]));
			return motion;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("ParticlePass: a particle's quad is a square of its size facing the camera, centred on its position")
		{
			const glm::mat4 view = glm::lookAt(glm::vec3(3.0f, 4.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			const glm::mat4 viewProjection = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 4.0f, 0.1f, 100.0f, 64, 64) * view;
			const std::vector<ParticleItem> particles = { { .Position = glm::vec3(0.5f, 0.25f, -0.5f), .Size = 0.5f, .Color = glm::vec4(1.0f, 0.5f, 0.25f, 0.75f) } };
			const std::vector<ParticleEmitterItem> emitters = { { .First = 0, .Count = 1, .Entity = UUID(31) } };
			ParticlePassPlan plan;
			const Status planned = PlanParticlePass(emitters, particles, {}, view, viewProjection, plan);
			REQUIRE_MESSAGE(planned.has_value(), planned.error().ToString());
			REQUIRE(plan.Quads.size() == 1);
			REQUIRE(plan.Runs.size() == 1);
			// A null texture is the built-in particle texture; the run keeps the emitter's blend and entity.
			CHECK(plan.Runs[0] == ParticlePassRun{ .Texture = BuiltinAssetHandles::ParticleTexture, .Blend = RenderParticleBlend::Alpha, .First = 0, .Count = 1, .Entity = UUID(31) });
			const ParticlePassQuad& quad = plan.Quads[0];
			CHECK(quad.Color == particles[0].Color);
			// In view space the corners are the centre's, offset by half the size along x (right) and y (up), at its depth:
			// top-left, top-right, bottom-right, bottom-left.
			const glm::vec3 centre = glm::vec3(view * glm::vec4(particles[0].Position, 1.0f));
			const std::array<glm::vec2, 4> offsets = { glm::vec2(-0.25f, 0.25f), glm::vec2(0.25f, 0.25f), glm::vec2(0.25f, -0.25f), glm::vec2(-0.25f, -0.25f) };
			for (size_t corner = 0; corner < 4; ++corner)
			{
				CAPTURE(corner);
				const glm::vec3 viewCorner = glm::vec3(view * glm::vec4(quad.Corners[corner], 1.0f));
				CHECK(Test::ApproxEqual(viewCorner, centre + glm::vec3(offsets[corner], 0.0f), 1e-5f));
			}
			CHECK(plan.Culled == 0);
			CHECK(plan.Skipped == 0);
		}

		TEST_CASE("ParticlePass: an Alpha emitter's quads go back to front, ties in particle order, and an Additive emitter's keep their order")
		{
			// Each particle is told apart by its colour's red channel, its index.
			const auto particle = [](float x, float z, uint32_t index)
			{
				return ParticleItem{ .Position = glm::vec3(x, 0.0f, z), .Size = 0.5f, .Color = glm::vec4(static_cast<float>(index), 0.0f, 0.0f, 1.0f) };
			};
			const std::vector<ParticleItem> particles = { particle(0.0f, 0.0f, 0), particle(-1.0f, -2.0f, 1), particle(1.0f, 1.0f, 2), particle(1.0f, -2.0f, 3),
				particle(0.0f, 1.0f, 4), particle(0.0f, -2.0f, 5), particle(0.0f, 0.0f, 6) };
			const std::vector<ParticleEmitterItem> emitters = {
				{ .Texture = WhiteParticleTextureHandle, .Blend = RenderParticleBlend::Alpha, .First = 0, .Count = 4, .Entity = UUID(1) },
				{ .Texture = WhiteParticleTextureHandle, .Blend = RenderParticleBlend::Additive, .First = 4, .Count = 3, .Entity = UUID(2) },
			};
			ParticlePassPlan plan;
			const Status planned = PlanParticlePass(emitters, particles, {}, MakeFrontView(), MakeFrontViewProjection(), plan);
			REQUIRE_MESSAGE(planned.has_value(), planned.error().ToString());
			REQUIRE(plan.Quads.size() == 7);
			std::vector<uint32_t> order;
			for (const ParticlePassQuad& quad : plan.Quads)
				order.push_back(static_cast<uint32_t>(quad.Color.r));
			// The two farthest (z = -2) in particle order, then z = 0, then the nearest; the additive ones as given.
			CHECK(order == std::vector<uint32_t>{ 1, 3, 0, 2, 4, 5, 6 });
			REQUIRE(plan.Runs.size() == 2);
			CHECK(plan.Runs[0] == ParticlePassRun{ .Texture = WhiteParticleTextureHandle, .Blend = RenderParticleBlend::Alpha, .First = 0, .Count = 4, .Entity = UUID(1) });
			CHECK(plan.Runs[1] == ParticlePassRun{ .Texture = WhiteParticleTextureHandle, .Blend = RenderParticleBlend::Additive, .First = 4, .Count = 3, .Entity = UUID(2) });

			// A plan is a pure function of its inputs, and planning again replaces the previous plan.
			ParticlePassPlan again = plan;
			const Status replanned = PlanParticlePass(emitters, particles, {}, MakeFrontView(), MakeFrontViewProjection(), again);
			REQUIRE(replanned.has_value());
			CHECK(again == plan);
		}

		TEST_CASE("ParticlePass: particles outside the view are culled, and non-finite or empty ones skipped, neither an error")
		{
			constexpr float Infinity = std::numeric_limits<float>::infinity();
			const std::vector<ParticleItem> particles = {
				{ .Position = glm::vec3(0.0f), .Size = 0.5f },                               // drawn
				{ .Position = glm::vec3(100.0f, 0.0f, 0.0f), .Size = 0.5f },                 // beside the view: culled
				{ .Position = glm::vec3(0.0f, 0.0f, 20.0f), .Size = 0.5f },                  // behind the camera: culled
				{ .Position = glm::vec3(std::nanf(""), 0.0f, 0.0f), .Size = 0.5f },          // skipped
				{ .Position = glm::vec3(0.0f), .Size = 0.0f },                               // skipped
				{ .Position = glm::vec3(0.0f), .Size = -1.0f },                              // skipped
				{ .Position = glm::vec3(0.0f), .Size = Infinity },                           // skipped
				{ .Position = glm::vec3(0.0f), .Size = 0.5f, .Color = glm::vec4(Infinity) }, // skipped
				{ .Position = glm::vec3(3.0e38f, 0.0f, 0.0f), .Size = 1.0e38f },             // a corner overflows: skipped
			};
			// The second emitter draws nothing, so it has no run.
			const std::vector<ParticleEmitterItem> emitters = { { .First = 0, .Count = 8, .Entity = UUID(1) }, { .First = 8, .Count = 1, .Entity = UUID(2) } };
			ParticlePassPlan plan;
			const Status planned = PlanParticlePass(emitters, particles, {}, MakeFrontView(), MakeFrontViewProjection(), plan);
			REQUIRE_MESSAGE(planned.has_value(), planned.error().ToString());
			CHECK(plan.Quads.size() == 1);
			REQUIRE(plan.Runs.size() == 1);
			CHECK(plan.Runs[0].Entity == UUID(1));
			CHECK(plan.Runs[0].Count == 1);
			CHECK(plan.Culled == 2);
			CHECK(plan.Skipped == 6);

			// A view whose axes are not finite or have no length plans nothing.
			const Status degenerate = PlanParticlePass(emitters, particles, {}, glm::mat4(0.0f), MakeFrontViewProjection(), plan);
			REQUIRE(degenerate.has_value());
			CHECK(plan == ParticlePassPlan{});
		}

		// M33 (Docs/Decisions/0048-m33-particle-look.md decision 4): the quad's axes by alignment, seen from the front view (right
		// +X, up +Y): a turned Camera square turns counter-clockwise, a Flat square lies in its emitter's plane, and a Velocity
		// quad's top runs along its motion across the view, Size wide and Size plus that speed times VelocityStretch long.
		TEST_CASE("ParticlePass: a turned square turns counter-clockwise, a Flat one lies in its plane and a Velocity one stretches along its motion")
		{
			const std::vector<ParticleItem> particles = {
				{ .Position = glm::vec3(0.0f), .Size = 2.0f, .Rotation = 1.5707963267948966f },
				{ .Position = glm::vec3(0.0f), .Size = 2.0f },
				{ .Position = glm::vec3(0.0f), .Size = 0.5f, .Velocity = glm::vec3(3.0f, 0.0f, 0.0f) },
				{ .Position = glm::vec3(0.0f), .Size = 0.5f, .Velocity = glm::vec3(0.0f, 0.0f, -5.0f) },
			};
			const std::vector<ParticleEmitterItem> emitters = {
				{ .Blend = RenderParticleBlend::Additive, .First = 0, .Count = 1, .Entity = UUID(1) },
				{ .Blend = RenderParticleBlend::Additive,
					.First = 1,
					.Count = 1,
					.Entity = UUID(2),
					.Alignment = RenderParticleAlignment::Flat,
					.PlaneRight = glm::vec3(1.0f, 0.0f, 0.0f),
					.PlaneUp = glm::vec3(0.0f, 0.0f, -1.0f) },
				{ .Blend = RenderParticleBlend::Additive, .First = 2, .Count = 2, .Entity = UUID(3), .Alignment = RenderParticleAlignment::Velocity, .VelocityStretch = 0.5f },
			};
			ParticlePassPlan plan;
			const Status planned = PlanParticlePass(emitters, particles, {}, MakeFrontView(), MakeFrontViewProjection(), plan);
			REQUIRE_MESSAGE(planned.has_value(), planned.error().ToString());
			REQUIRE(plan.Quads.size() == 4);
			const auto checkCorners = [](const ParticlePassQuad& quad, const std::array<glm::vec3, 4>& expected)
			{
				for (size_t corner = 0; corner < 4; ++corner)
				{
					CAPTURE(corner);
					CHECK(Test::ApproxEqual(quad.Corners[corner], expected[corner], 1e-6f));
				}
			};
			// A quarter turn: the top-left corner moves from (-1, 1) to (-1, -1).
			checkCorners(plan.Quads[0], { glm::vec3(-1.0f, -1.0f, 0.0f), glm::vec3(-1.0f, 1.0f, 0.0f), glm::vec3(1.0f, 1.0f, 0.0f), glm::vec3(1.0f, -1.0f, 0.0f) });
			// Flat in the XZ plane, its top toward -Z.
			checkCorners(plan.Quads[1], { glm::vec3(-1.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 1.0f), glm::vec3(-1.0f, 0.0f, 1.0f) });
			// Moving right at 3 m/s: 0.5 wide and 0.5 + 3 * 0.5 = 2 long, its top toward +X.
			checkCorners(plan.Quads[2], { glm::vec3(1.0f, 0.25f, 0.0f), glm::vec3(1.0f, -0.25f, 0.0f), glm::vec3(-1.0f, -0.25f, 0.0f), glm::vec3(-1.0f, 0.25f, 0.0f) });
			// Moving straight at the camera: no motion across the view, so the square facing it.
			checkCorners(plan.Quads[3], { glm::vec3(-0.25f, 0.25f, 0.0f), glm::vec3(0.25f, 0.25f, 0.0f), glm::vec3(0.25f, -0.25f, 0.0f), glm::vec3(-0.25f, -0.25f, 0.0f) });
		}

		// M33 (decisions 4 to 6): a flipbook quad shows frame floor(phase n) mod n of its emitter's n frames and the next one, the
		// blend their fraction; a soft emitter's quads carry the inverse distance, and a particle the view drew before its offset
		// from there, so its motion vectors follow it.
		TEST_CASE("ParticlePass: a flipbook quad takes its phase's frame and the next, a soft one its inverse distance, a seen one its previous offset")
		{
			const AssetHandle sheetTexture = UUID(8970);
			const std::vector<ParticleFlipbook> flipbooks = {
				{ .Texture = sheetTexture,
					.Frames = { glm::vec4(0.0f, 0.0f, 0.5f, 0.5f), glm::vec4(0.5f, 0.0f, 1.0f, 0.5f), glm::vec4(0.0f, 0.5f, 0.5f, 1.0f), glm::vec4(0.5f, 0.5f, 1.0f, 1.0f) } },
				{},
			};
			const std::vector<ParticleItem> particles = {
				{ .Position = glm::vec3(0.0f), .Size = 1.0f, .FramePhase = 1.4375f, .Previous = ParticleDrawState{ .Position = glm::vec3(-0.5f, 0.0f, 0.0f), .Size = 1.0f } },
				{ .Position = glm::vec3(1.0f, 0.0f, 0.0f), .Size = 1.0f },
				{ .Position = glm::vec3(0.0f), .Size = 1.0f, .FramePhase = 0.75f },
			};
			const std::vector<ParticleEmitterItem> emitters = {
				{ .Blend = RenderParticleBlend::Additive,
					.First = 0,
					.Count = 2,
					.Entity = UUID(1),
					.Sheet = UUID(8971),
					.BlendFrames = true,
					.SoftDistance = 0.5f,
					.FrameCycles = 2.0f },
				{ .Texture = WhiteParticleTextureHandle, .Blend = RenderParticleBlend::Additive, .First = 2, .Count = 1, .Entity = UUID(2) },
			};
			ParticlePassPlan plan;
			const Status planned = PlanParticlePass(emitters, particles, flipbooks, MakeFrontView(), MakeFrontViewProjection(), plan);
			REQUIRE_MESSAGE(planned.has_value(), planned.error().ToString());
			REQUIRE(plan.Quads.size() == 3);
			REQUIRE(plan.Runs.size() == 2);
			CHECK(plan.Runs[0].Texture == sheetTexture);
			CHECK(plan.Runs[1].Texture == WhiteParticleTextureHandle);
			// 1.4375 of four frames is frame 5.75: frame 1 blending three quarters toward frame 2.
			const ParticlePassQuad& blended = plan.Quads[0];
			CHECK(blended.TexCoords == flipbooks[0].Frames[1]);
			CHECK(blended.NextTexCoords == flipbooks[0].Frames[2]);
			CHECK(blended.FrameBlend == 0.75f);
			CHECK(blended.InverseSoftDistance == 2.0f);
			for (size_t corner = 0; corner < 4; ++corner)
				CHECK(blended.PreviousCorners[corner] == blended.Corners[corner] - glm::vec3(0.5f, 0.0f, 0.0f));
			// Phase 0: the first frame, the second next, no blend; no previous position, no offset.
			const ParticlePassQuad& first = plan.Quads[1];
			CHECK(first.TexCoords == flipbooks[0].Frames[0]);
			CHECK(first.NextTexCoords == flipbooks[0].Frames[1]);
			CHECK(first.FrameBlend == 0.0f);
			CHECK(first.PreviousCorners == first.Corners);
			// Without a sheet: the whole texture, a hard cut, whatever the phase.
			const ParticlePassQuad& whole = plan.Quads[2];
			CHECK(whole.TexCoords == glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
			CHECK(whole.NextTexCoords == glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
			CHECK(whole.FrameBlend == 0.0f);
			CHECK(whole.InverseSoftDistance == 0.0f);
		}

		// M33 (decision 5, the review's finding 1): the range plays FrameCycles times over a life, so a blended particle on the
		// last frame of the last play holds it instead of blending into the first frame, while one between plays blends on.
		TEST_CASE("ParticlePass: the last frame of a blended range's last play is held, not blended into the first")
		{
			const std::vector<ParticleFlipbook> flipbooks = { { .Texture = UUID(8975),
				.Frames = { glm::vec4(0.0f, 0.0f, 0.5f, 1.0f), glm::vec4(0.5f, 0.0f, 1.0f, 1.0f) } } };
			// A dying particle of a once-played range (phase just below 1: frame 1, three quarters on), and a particle at the
			// same place in the first of two plays.
			const std::vector<ParticleItem> particles = { { .Position = glm::vec3(0.0f), .Size = 1.0f, .FramePhase = 0.875f },
				{ .Position = glm::vec3(1.0f, 0.0f, 0.0f), .Size = 1.0f, .FramePhase = 0.875f } };
			std::vector<ParticleEmitterItem> emitters = { { .First = 0, .Count = 1, .Entity = UUID(1), .Sheet = UUID(8976), .BlendFrames = true },
				{ .First = 1, .Count = 1, .Entity = UUID(2), .Sheet = UUID(8976), .BlendFrames = true, .FrameCycles = 2.0f } };
			const std::vector<ParticleFlipbook> both = { flipbooks[0], flipbooks[0] };
			ParticlePassPlan plan;
			const Status planned = PlanParticlePass(emitters, particles, both, MakeFrontView(), MakeFrontViewProjection(), plan);
			REQUIRE_MESSAGE(planned.has_value(), planned.error().ToString());
			REQUIRE(plan.Quads.size() == 2);
			CHECK(plan.Quads[0].TexCoords == flipbooks[0].Frames[1]);
			CHECK(plan.Quads[0].FrameBlend == 0.0f);
			CHECK(plan.Quads[1].TexCoords == flipbooks[0].Frames[1]);
			CHECK(plan.Quads[1].NextTexCoords == flipbooks[0].Frames[0]);
			CHECK(plan.Quads[1].FrameBlend == 0.75f);
		}

		// M33 (decision 6, the review's finding 2): a particle's previous corners are laid from how the view's previous render
		// drew it, so its spin, its growth and a Velocity quad's stretch reach the motion vectors, not its centre's motion
		// alone; a Velocity particle not moving across the view faces the camera unturned (the review's finding 10).
		TEST_CASE("ParticlePass: a seen particle's previous corners are laid from its previous angle, size and stretch")
		{
			const std::vector<ParticleItem> particles = {
				// Now turned a quarter and 2 m wide; drawn unturned and 1 m wide half a metre to the left before.
				{ .Position = glm::vec3(0.0f), .Size = 2.0f, .Rotation = 1.5707963267948966f, .Previous = ParticleDrawState{ .Position = glm::vec3(-0.5f, 0.0f, 0.0f), .Size = 1.0f } },
				// A spark now moving right at 3 m/s, before at 1 m/s: its previous length 0.5 + 1 * 0.5.
				{ .Position = glm::vec3(0.0f), .Size = 0.5f, .Velocity = glm::vec3(3.0f, 0.0f, 0.0f), .Previous = ParticleDrawState{ .Position = glm::vec3(0.0f), .Size = 0.5f, .Velocity = glm::vec3(1.0f, 0.0f, 0.0f) } },
				// A spark moving straight at the camera with an angle: an unturned square.
				{ .Position = glm::vec3(0.0f), .Size = 0.5f, .Rotation = 1.0f, .Velocity = glm::vec3(0.0f, 0.0f, 5.0f) },
			};
			const std::vector<ParticleEmitterItem> emitters = {
				{ .Blend = RenderParticleBlend::Additive, .First = 0, .Count = 1, .Entity = UUID(1) },
				{ .Blend = RenderParticleBlend::Additive, .First = 1, .Count = 2, .Entity = UUID(2), .Alignment = RenderParticleAlignment::Velocity, .VelocityStretch = 0.5f },
			};
			ParticlePassPlan plan;
			const Status planned = PlanParticlePass(emitters, particles, {}, MakeFrontView(), MakeFrontViewProjection(), plan);
			REQUIRE_MESSAGE(planned.has_value(), planned.error().ToString());
			REQUIRE(plan.Quads.size() == 3);
			const auto checkCorners = [](const std::array<glm::vec3, 4>& corners, const std::array<glm::vec3, 4>& expected)
			{
				for (size_t corner = 0; corner < 4; ++corner)
				{
					CAPTURE(corner);
					CHECK(Test::ApproxEqual(corners[corner], expected[corner], 1e-6f));
				}
			};
			checkCorners(plan.Quads[0].PreviousCorners,
				{ glm::vec3(-1.0f, 0.5f, 0.0f), glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(-1.0f, -0.5f, 0.0f) });
			checkCorners(plan.Quads[1].PreviousCorners,
				{ glm::vec3(0.5f, 0.25f, 0.0f), glm::vec3(0.5f, -0.25f, 0.0f), glm::vec3(-0.5f, -0.25f, 0.0f), glm::vec3(-0.5f, 0.25f, 0.0f) });
			checkCorners(plan.Quads[2].Corners,
				{ glm::vec3(-0.25f, 0.25f, 0.0f), glm::vec3(0.25f, 0.25f, 0.0f), glm::vec3(0.25f, -0.25f, 0.0f), glm::vec3(-0.25f, -0.25f, 0.0f) });
			CHECK(plan.Quads[2].PreviousCorners == plan.Quads[2].Corners);
		}

		// M33 (decision 5): a missing or failed sheet, or a range that does not fit it, resolves to no frames, and the emitter's
		// particles draw nothing, as a sprite's; the other emitters draw.
		TEST_CASE("ParticlePass: an emitter whose flipbook did not resolve draws nothing")
		{
			const std::vector<ParticleItem> particles(3, ParticleItem{ .Position = glm::vec3(0.0f), .Size = 0.5f });
			const std::vector<ParticleEmitterItem> emitters = {
				{ .First = 0, .Count = 1, .Entity = UUID(1), .Sheet = UUID(8972) },
				{ .First = 1, .Count = 1, .Entity = UUID(2), .Sheet = UUID(8973) },
				{ .First = 2, .Count = 1, .Entity = UUID(3) },
			};
			ParticlePassPlan plan;
			// The first emitter's flipbook has no frames; the second has none at all (past the list's end).
			const std::vector<ParticleFlipbook> flipbooks = { ParticleFlipbook{ .Texture = UUID(8974) } };
			const Status planned = PlanParticlePass(emitters, particles, flipbooks, MakeFrontView(), MakeFrontViewProjection(), plan);
			REQUIRE_MESSAGE(planned.has_value(), planned.error().ToString());
			REQUIRE(plan.Runs.size() == 1);
			CHECK(plan.Runs[0].Entity == UUID(3));
			CHECK(plan.Quads.size() == 1);
			CHECK(plan.Skipped == 0);
		}

		TEST_CASE("ParticlePass: an emitter whose range lies outside the snapshot's particles is skipped with InvalidArgument")
		{
			const std::vector<ParticleItem> particles(3, ParticleItem{ .Position = glm::vec3(0.0f), .Size = 0.5f });
			const std::vector<ParticleEmitterItem> emitters = {
				{ .First = 1, .Count = 5, .Entity = UUID(1) },
				{ .First = std::numeric_limits<uint32_t>::max(), .Count = 2, .Entity = UUID(2) },
				{ .First = 0, .Count = 3, .Entity = UUID(3) },
			};
			ParticlePassPlan plan;
			const Status planned = PlanParticlePass(emitters, particles, {}, MakeFrontView(), MakeFrontViewProjection(), plan);
			REQUIRE_FALSE(planned.has_value());
			CHECK(planned.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE(plan.Runs.size() == 1);
			CHECK(plan.Runs[0].Entity == UUID(3));
			CHECK(plan.Quads.size() == 3);
		}

		TEST_CASE("Particles: an emitter's particles draw as squares of their size and colour facing the camera" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(WhiteParticleTextureHandle, MakeParticleTexture(false));
			RenderSnapshot snapshot = MakeParticleSnapshot(setup);
			// A red square 2 units wide at the origin (scene pixels [6, 10), view pixels [24, 40)) and a green one 1 unit wide
			// centred at (-2.5, 2.5) (scene pixels [2, 4), view pixels [8, 16)).
			const std::vector<ParticleItem> particles = {
				{ .Position = glm::vec3(0.0f), .Size = 2.0f, .Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f) },
				{ .Position = glm::vec3(-2.5f, 2.5f, 0.0f), .Size = 1.0f, .Color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f) },
			};
			AddEmitter(snapshot, WhiteParticleTextureHandle, RenderParticleBlend::Alpha, particles, UUID(70));
			const Image image = setup.Render(snapshot);
			for (uint32_t y = 0; y < 64; ++y)
			{
				for (uint32_t x = 0; x < 64; ++x)
				{
					const bool red = x >= 24 && x < 40 && y >= 24 && y < 40;
					const bool green = x >= 8 && x < 16 && y >= 8 && y < 16;
					const glm::u8vec4 expected = red ? glm::u8vec4(255, 0, 0, 255) : (green ? glm::u8vec4(0, 255, 0, 255) : glm::u8vec4(0, 0, 0, 255));
					if (ParticlePixel(image, x, y) != expected)
					{
						CAPTURE(x);
						CAPTURE(y);
						CHECK(ParticlePixel(image, x, y) == expected);
					}
				}
			}
			// The Particles row: two quads in one draw (one texture).
			const RenderStats stats = setup.Renderer().GetRenderStats();
			const RenderPassStats* pass = Test::SceneRendererIntegrationFixture::FindPass(stats, "Particles");
			REQUIRE(pass != nullptr);
			CHECK(pass->Triangles == 4);
			CHECK(pass->DrawCalls == 1);
		}

		TEST_CASE("Particles: alpha particles blend back to front within an emitter, and additive particles add in any order" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(WhiteParticleTextureHandle, MakeParticleTexture(false));
			RenderSnapshot snapshot = MakeParticleSnapshot(setup);
			// Alpha: a near red square over x [-3, -1] listed before a far green one over [-2.5, -0.5]; drawn back to front, red
			// covers the overlap (scene pixels [3, 6)). Additive: two squares of red at alpha 0.5 over x [0.5, 2.5] and [1.5, 3.5]
			// add 0.5 each, 1 in their overlap (scene pixels [11, 13)).
			const std::vector<ParticleItem> blended = {
				{ .Position = glm::vec3(-2.0f, 0.0f, 2.0f), .Size = 2.0f, .Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f) },
				{ .Position = glm::vec3(-1.5f, 0.0f, -2.0f), .Size = 2.0f, .Color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f) },
			};
			std::vector<ParticleItem> added = {
				{ .Position = glm::vec3(1.5f, 0.0f, 0.0f), .Size = 2.0f, .Color = glm::vec4(1.0f, 0.0f, 0.0f, 0.5f) },
				{ .Position = glm::vec3(2.5f, 0.0f, 1.0f), .Size = 2.0f, .Color = glm::vec4(1.0f, 0.0f, 0.0f, 0.5f) },
			};
			AddEmitter(snapshot, WhiteParticleTextureHandle, RenderParticleBlend::Alpha, blended, UUID(71));
			AddEmitter(snapshot, WhiteParticleTextureHandle, RenderParticleBlend::Additive, added, UUID(72));
			const Image image = setup.Render(snapshot);
			// Row 32 is scene row 8, inside every square.
			CHECK(ParticlePixel(image, 9, 32) == glm::u8vec4(255, 0, 0, 255));  // scene pixel 2: red alone
			CHECK(ParticlePixel(image, 18, 32) == glm::u8vec4(255, 0, 0, 255)); // scene pixel 4: the near red over the far green
			CHECK(ParticlePixel(image, 26, 32) == glm::u8vec4(0, 255, 0, 255)); // scene pixel 6: green alone
			CHECK(ParticlePixel(image, 46, 32) == glm::u8vec4(255, 0, 0, 255)); // scene pixel 11: 0.5 + 0.5
			// Scene pixel 9: 0.5 alone, the sRGB encoding of 0.5 (187.5).
			const glm::u8vec4 half = ParticlePixel(image, 37, 32);
			CHECK(half.r >= 187);
			CHECK(half.r <= 188);
			CHECK(half.g == 0);
			CHECK(half.b == 0);

			// Additive particles draw the same image in the other order.
			std::swap(added[0], added[1]);
			RenderSnapshot reordered = MakeParticleSnapshot(setup);
			reordered.FrameIndex = snapshot.FrameIndex + 1;
			AddEmitter(reordered, WhiteParticleTextureHandle, RenderParticleBlend::Alpha, blended, UUID(71));
			AddEmitter(reordered, WhiteParticleTextureHandle, RenderParticleBlend::Additive, added, UUID(72));
			CHECK(setup.Render(reordered).Pixels == image.Pixels);
		}

		TEST_CASE("Particles: particles are depth-tested against the opaque scene without writing depth" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(WhiteParticleTextureHandle, MakeParticleTexture(false));
			// The fixture's opaque 2 x 2 quad at z = -3, over scene pixels [6, 10).
			RenderSnapshot snapshot = MakeParticleSnapshot(setup);
			snapshot.Meshes = setup.Snapshot().Meshes;
			snapshot.PickTable = setup.Snapshot().PickTable;
			const Image meshOnly = setup.Render(snapshot);

			// A particle behind the quad is hidden.
			const std::vector<ParticleItem> behind = { { .Position = glm::vec3(0.0f, 0.0f, -5.0f), .Size = 1.0f, .Color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f) } };
			RenderSnapshot hidden = snapshot;
			hidden.FrameIndex += 1;
			AddEmitter(hidden, WhiteParticleTextureHandle, RenderParticleBlend::Alpha, behind, UUID(73));
			CHECK(setup.Render(hidden).Pixels == meshOnly.Pixels);

			// One in front of it draws; and of two additive particles over one another, the nearer listed first, the farther
			// still adds where the nearer drew, since particles write no depth (centred at (2.5, 2.5): view pixels [48, 56) x
			// [8, 16)).
			const std::vector<ParticleItem> front = { { .Position = glm::vec3(0.0f, 0.0f, 0.0f), .Size = 1.0f, .Color = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f) } };
			const std::vector<ParticleItem> stacked = {
				{ .Position = glm::vec3(2.5f, 2.5f, 4.0f), .Size = 1.0f, .Color = glm::vec4(1.0f, 0.0f, 0.0f, 0.5f) },
				{ .Position = glm::vec3(2.5f, 2.5f, 2.0f), .Size = 1.0f, .Color = glm::vec4(1.0f, 0.0f, 0.0f, 0.5f) },
			};
			RenderSnapshot visible = snapshot;
			visible.FrameIndex += 2;
			AddEmitter(visible, WhiteParticleTextureHandle, RenderParticleBlend::Alpha, front, UUID(74));
			AddEmitter(visible, WhiteParticleTextureHandle, RenderParticleBlend::Additive, stacked, UUID(75));
			const Image image = setup.Render(visible);
			CHECK(ParticlePixel(image, 32, 32) == glm::u8vec4(0, 0, 255, 255));
			CHECK(ParticlePixel(image, 50, 10) == glm::u8vec4(255, 0, 0, 255));
		}

		// Particles draw in the Lit view only, as sprites do (decision 7): the data views, the material views, the Overdraw view
		// and the Lit view's wireframe show none, and the Particles row is not recorded.
		TEST_CASE("Particles: data views, material views and the wireframe draw no particles" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(WhiteParticleTextureHandle, MakeParticleTexture(false));
			RenderSnapshot snapshot = MakeParticleSnapshot(setup);
			const std::vector<ParticleItem> particles = { { .Position = glm::vec3(0.0f), .Size = 2.0f, .Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f) } };
			AddEmitter(snapshot, WhiteParticleTextureHandle, RenderParticleBlend::Alpha, particles, UUID(76));
			const glm::u8vec4 red(255, 0, 0, 255);
			REQUIRE(ParticlePixel(setup.Render(snapshot), 32, 32) == red);
			REQUIRE(ParticleTriangles(setup) == 2);
			for (uint32_t view = 1; view < RenderDebugViewCount; ++view)
			{
				const auto debugView = static_cast<RenderDebugView>(view);
				CAPTURE(RenderDebugViewToString(debugView));
				snapshot.DebugView = debugView;
				snapshot.FrameIndex += 1;
				CHECK(ParticlePixel(setup.Render(snapshot), 32, 32) != red);
				CHECK(ParticleTriangles(setup) == 0);
			}
			snapshot.DebugView = RenderDebugView::Lit;
			snapshot.Flags = snapshot.Flags | RenderViewFlags::Wireframe;
			snapshot.FrameIndex += 1;
			CHECK(ParticlePixel(setup.Render(snapshot), 32, 32) != red);
			CHECK(ParticleTriangles(setup) == 0);
		}

		TEST_CASE("Particles: an emitter without a texture draws the built-in particle texture, a soft white disc" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			RenderSnapshot snapshot = MakeParticleSnapshot(setup);
			// A white particle over the whole scene (8 units): each scene pixel shows two texels' worth of the 32 x 32 disc.
			const std::vector<ParticleItem> particles = { { .Position = glm::vec3(0.0f), .Size = 8.0f, .Color = glm::vec4(1.0f) } };
			AddEmitter(snapshot, AssetHandle(), RenderParticleBlend::Alpha, particles, UUID(77));
			const Image image = setup.Render(snapshot);
			for (uint32_t y = 0; y < 64; ++y)
			{
				for (uint32_t x = 0; x < 64; ++x)
				{
					// White blended over black by the texel's alpha: grey levels only (never the Missing placeholder's magenta).
					const glm::u8vec4 pixel = ParticlePixel(image, x, y);
					if (pixel.r != pixel.g || pixel.g != pixel.b)
					{
						CAPTURE(x);
						CAPTURE(y);
						CHECK(pixel.r == pixel.g);
						CHECK(pixel.g == pixel.b);
					}
				}
			}
			// Opaque at the centre, clear in the corners (outside the disc) and soft between.
			CHECK(ParticlePixel(image, 32, 32).r >= 250);
			CHECK(ParticlePixel(image, 1, 1) == glm::u8vec4(0, 0, 0, 255));
			CHECK(ParticlePixel(image, 62, 62) == glm::u8vec4(0, 0, 0, 255));
			const uint8_t soft = ParticlePixel(image, 13, 33).r; // scene pixel (3, 8), at about half the radius
			CHECK(soft > 50);
			CHECK(soft < 250);
		}

		TEST_CASE("Particles: in pixel-art mode a particle's texture is point-sampled, its top-left texel at the quad's top-left corner" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(PatternParticleTextureHandle, MakeParticleTexture(true));
			RenderSnapshot snapshot = MakeParticleSnapshot(setup);
			// The 8 x 8 texels over the 16 x 16 scene pixels: each texel two scene pixels, eight view pixels a side. A bilinear
			// sample between texel centres would blend neighbours; the Point sampler shows each texel exactly.
			const std::vector<ParticleItem> particles = { { .Position = glm::vec3(0.0f), .Size = 8.0f, .Color = glm::vec4(1.0f) } };
			AddEmitter(snapshot, PatternParticleTextureHandle, RenderParticleBlend::Alpha, particles, UUID(78));
			const Image image = setup.Render(snapshot);
			for (uint32_t y = 0; y < 64; ++y)
			{
				for (uint32_t x = 0; x < 64; ++x)
				{
					const uint32_t texelX = x / 8;
					const uint32_t texelY = y / 8;
					const glm::u8vec4 expected(32 * texelX, 32 * texelY, 255 - 16 * texelX, 255);
					if (ParticlePixel(image, x, y) != expected)
					{
						CAPTURE(x);
						CAPTURE(y);
						CHECK(ParticlePixel(image, x, y) == expected);
					}
				}
			}
		}

		// M33 (decision 5): a flipbook particle shows its phase's frame of its sheet, point-sampled in pixel-art mode, and a
		// blended one mixes it with the next before its colour multiplies them.
		TEST_CASE("Particles: a flipbook particle shows the frame of its phase and blends toward the next" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(FlipbookTextureHandle, MakeFlipbookTexture());
			setup.Assets().Publish(FlipbookSheetHandle, MakeFlipbookSheet());
			RenderSnapshot snapshot = MakeParticleSnapshot(setup);
			// Phase 0.25 of two frames is frame 0 (red), 0.75 frame 1 (green); blended, 0.75 is frame 1 halfway to frame 0.
			const std::vector<ParticleItem> held = {
				{ .Position = glm::vec3(-2.5f, 2.5f, 0.0f), .Size = 1.0f, .FramePhase = 0.25f },
				{ .Position = glm::vec3(2.5f, 2.5f, 0.0f), .Size = 1.0f, .FramePhase = 0.75f },
			};
			const std::vector<ParticleItem> blended = { { .Position = glm::vec3(0.0f, -2.5f, 0.0f), .Size = 1.0f, .FramePhase = 0.75f } };
			AddEmitter(snapshot, WhiteParticleTextureHandle, RenderParticleBlend::Alpha, held, UUID(81));
			snapshot.ParticleEmitters.back().Sheet = FlipbookSheetHandle;
			AddEmitter(snapshot, WhiteParticleTextureHandle, RenderParticleBlend::Alpha, blended, UUID(82));
			snapshot.ParticleEmitters.back().Sheet = FlipbookSheetHandle;
			snapshot.ParticleEmitters.back().BlendFrames = true;
			snapshot.ParticleEmitters.back().FrameCycles = 2.0f; // the first of two plays: frame 1 blends on into frame 0
			const Image image = setup.Render(snapshot);
			CHECK(ParticlePixel(image, 12, 12) == glm::u8vec4(255, 0, 0, 255));
			CHECK(ParticlePixel(image, 52, 12) == glm::u8vec4(0, 255, 0, 255));
			// Half green and half red in linear light: the sRGB encoding of 0.5 (187.5) in both channels.
			const glm::u8vec4 mixed = ParticlePixel(image, 32, 52);
			CHECK(mixed.r >= 187);
			CHECK(mixed.r <= 188);
			CHECK(mixed.g >= 187);
			CHECK(mixed.g <= 188);
			CHECK(mixed.b == 0);

			// A range that starts or ends past the sheet's frames draws nothing (the particles are the only red or green here).
			RenderSnapshot missing = snapshot;
			missing.FrameIndex += 1;
			missing.ParticleEmitters[0].FirstFrame = 2;
			missing.ParticleEmitters[1].FirstFrame = 1;
			missing.ParticleEmitters[1].FrameCount = 2;
			const Image none = setup.Render(missing);
			CHECK(ParticlePixel(none, 12, 12) == glm::u8vec4(0, 0, 0, 255));
			CHECK(ParticlePixel(none, 32, 52) == glm::u8vec4(0, 0, 0, 255));
		}

		// M33 (decision 4): with a soft distance a particle's alpha fades with its view-depth distance to the surface behind it
		// (a tenth of the way here), so it no longer stops at a hard line; three metres in front of the surface, and over the
		// cleared far plane, it is whole, as a hard particle in front of the surface is.
		TEST_CASE("Particles: a soft particle fades toward the surface behind it, and a hard one is cut where it was" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(WhiteParticleTextureHandle, MakeParticleTexture(false));
			// The fixture's opaque 2 x 2 quad at z = -3, over view pixels [24, 40).
			RenderSnapshot base = MakeParticleSnapshot(setup);
			base.Meshes = setup.Snapshot().Meshes;
			base.PickTable = setup.Snapshot().PickTable;
			const Image meshOnly = setup.Render(base);
			const auto render = [&setup, &base](float softDistance)
			{
				RenderSnapshot snapshot = base;
				snapshot.FrameIndex += softDistance > 0.0f ? 2 : 1;
				const std::vector<ParticleItem> particles = {
					{ .Position = glm::vec3(0.0f, 0.0f, -2.9f), .Size = 1.0f, .Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f) }, // 0.1 m in front
					{ .Position = glm::vec3(-1.0f, 1.0f, 0.0f), .Size = 1.0f, .Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f) }, // 3 m in front
					{ .Position = glm::vec3(2.5f, 2.5f, 0.0f), .Size = 1.0f, .Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f) },  // over the far plane
				};
				AddEmitter(snapshot, WhiteParticleTextureHandle, RenderParticleBlend::Alpha, particles, UUID(83));
				snapshot.ParticleEmitters.back().SoftDistance = softDistance;
				return setup.Render(snapshot);
			};
			const Image hard = render(0.0f);
			const Image soft = render(1.0f);
			const glm::u8vec4 red(255, 0, 0, 255);
			CHECK(ParticlePixel(hard, 32, 32) == red);
			CHECK(ParticlePixel(hard, 26, 26) == red);
			CHECK(ParticlePixel(hard, 52, 12) == red);
			CHECK(ParticlePixel(soft, 26, 26) == red);
			CHECK(ParticlePixel(soft, 52, 12) == red);
			// Near the surface: a tenth of the red over the quad, so far less red than the hard particle and the quad's own
			// green and blue barely dimmed.
			const glm::u8vec4 faded = ParticlePixel(soft, 32, 32);
			const glm::u8vec4 surface = ParticlePixel(meshOnly, 32, 32);
			CHECK(faded.r > surface.r);
			CHECK(faded.r < 200);
			CHECK(faded.g <= surface.g);
			CHECK(faded.g + 40 >= surface.g);
		}

		// M33 (decision 6): with TAA a particle writes the motion it was drawn with since the view's previous render into
		// SceneVelocity, where its alpha is at least ParticleVelocityAlpha; one that stood still, or that the view never drew
		// before, writes the camera's motion (none here), and without TAA particles write no motion at all.
		TEST_CASE("Particles: a moving particle writes its drawn motion into SceneVelocity, and one that stood still writes none" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(WhiteParticleTextureHandle, MakeParticleTexture(false));
			GraphicsDevice& device = gpu.GetDevice();
			// The fixture's still quad at z = -3 (its prepass writes no motion) behind most of the particles, 0.5 m in front of it.
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Post.AntiAliasing = RenderAntiAliasing::Taa;
			const glm::vec3 offset(0.25f, 0.0f, 0.0f);
			const glm::vec3 moving(0.6f, -0.4f, -2.5f);
			const glm::vec3 still(-0.6f, -0.4f, -2.5f);
			const glm::vec3 unseen(0.0f, 0.6f, -2.5f);
			// The motion draw's two rules (the review's finding 4): a moving particle below ParticleVelocityAlpha, and a moving
			// particle the quad hides, both leave the motion already there.
			const glm::vec3 faint(-0.4f, 0.3f, -2.5f);
			const glm::vec3 hidden(0.5f, 0.4f, -4.0f);
			const std::vector<ParticleItem> particles = {
				{ .Position = moving, .Size = 0.4f, .Previous = ParticleDrawState{ .Position = moving - offset, .Size = 0.4f } },
				{ .Position = still, .Size = 0.4f, .Previous = ParticleDrawState{ .Position = still, .Size = 0.4f } },
				{ .Position = unseen, .Size = 0.4f },
				{ .Position = faint, .Size = 0.4f, .Color = glm::vec4(1.0f, 1.0f, 1.0f, 0.4f), .Previous = ParticleDrawState{ .Position = faint - offset, .Size = 0.4f } },
				{ .Position = hidden, .Size = 0.4f, .Previous = ParticleDrawState{ .Position = hidden - offset, .Size = 0.4f } },
			};
			AddEmitter(snapshot, WhiteParticleTextureHandle, RenderParticleBlend::Additive, particles, UUID(84));
			setup.Submit(snapshot);
			const std::vector<glm::vec2> motion = ReadParticleVelocity(device, setup.Renderer());
			const glm::mat4 viewProjection = snapshot.Camera.Projection * snapshot.Camera.View;
			const glm::vec2 shift = ProjectParticleToPixel(viewProjection, moving, 64) - ProjectParticleToPixel(viewProjection, moving - offset, 64);
			REQUIRE(shift.x > 2.0f);
			const auto at = [&motion](const glm::vec2& pixel)
			{
				return motion[static_cast<size_t>(pixel.y) * 64 + static_cast<size_t>(pixel.x)];
			};
			const glm::vec2 movingPixel = ProjectParticleToPixel(viewProjection, moving, 64);
			CHECK(std::abs(at(movingPixel).x - shift.x) <= 0.05f);
			CHECK(std::abs(at(movingPixel).y) <= 0.05f);
			CHECK(at(ProjectParticleToPixel(viewProjection, still, 64)) == glm::vec2(0.0f));
			CHECK(at(ProjectParticleToPixel(viewProjection, unseen, 64)) == glm::vec2(0.0f));
			CHECK(at(ProjectParticleToPixel(viewProjection, faint, 64)) == glm::vec2(0.0f));
			CHECK(at(ProjectParticleToPixel(viewProjection, hidden, 64)) == glm::vec2(0.0f));
			CHECK(motion[0] == glm::vec2(0.0f));

			// Without TAA nothing reads SceneVelocity, and the particles write none.
			snapshot.Post.AntiAliasing = RenderAntiAliasing::None;
			snapshot.FrameIndex += 1;
			setup.Submit(snapshot);
			const std::vector<glm::vec2> none = ReadParticleVelocity(device, setup.Renderer());
			CHECK(none[static_cast<size_t>(movingPixel.y) * 64 + static_cast<size_t>(movingPixel.x)] == glm::vec2(0.0f));
		}

		// M33 (decision 5, the review's finding 5): the flipbook resolution's fallbacks, as a sprite's: a sheet without a texture
		// samples opaque white, tinted by the colour; a sheet that is missing draws nothing and is reported; a sheet whose
		// texture is missing draws the texture's placeholder whole over each quad.
		TEST_CASE("Particles: a sheet without a texture draws white, a missing sheet nothing, and a missing texture its placeholder" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			const AssetHandle untextured = UUID(8965);
			const AssetHandle missingSheet = UUID(8966);
			const AssetHandle missingTexture = UUID(8967);
			Ref<SpriteSheetData> white = CreateRef<SpriteSheetData>();
			white->Frames = { SpriteFrame{ .Rect = { .X = 0, .Y = 0, .Width = 4, .Height = 4 } } };
			setup.Assets().Publish(untextured, white);
			Ref<SpriteSheetData> broken = CreateRef<SpriteSheetData>();
			broken->Texture = TypedAssetHandle<AssetType::Texture>(UUID(8968));
			broken->Frames = { SpriteFrame{ .Rect = { .X = 0, .Y = 0, .Width = 4, .Height = 4 } } };
			setup.Assets().Publish(missingTexture, broken);
			Test::ExpectLog missing(LogLevel::Error, std::string(AssetMissingCode));
			RenderSnapshot snapshot = MakeParticleSnapshot(setup);
			const auto addFlipbook = [&snapshot](AssetHandle sheet, const glm::vec3& position, UUID entity)
			{
				const std::vector<ParticleItem> particles = { { .Position = position, .Size = 1.0f, .Color = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f) } };
				AddEmitter(snapshot, AssetHandle(), RenderParticleBlend::Alpha, particles, entity);
				snapshot.ParticleEmitters.back().Sheet = sheet;
			};
			addFlipbook(untextured, glm::vec3(-2.5f, 2.5f, 0.0f), UUID(85));
			addFlipbook(missingSheet, glm::vec3(2.5f, 2.5f, 0.0f), UUID(86));
			addFlipbook(missingTexture, glm::vec3(0.0f, -2.5f, 0.0f), UUID(87));
			const Image image = setup.Render(snapshot);
			CHECK(ParticlePixel(image, 12, 12) == glm::u8vec4(0, 0, 255, 255));
			CHECK(ParticlePixel(image, 52, 12) == glm::u8vec4(0, 0, 0, 255));
			// The placeholder's texels, tinted blue: some of its square is lit.
			bool drawn = false;
			for (uint32_t y = 48; y < 56; ++y)
			{
				for (uint32_t x = 28; x < 36; ++x)
					drawn = drawn || ParticlePixel(image, x, y) != glm::u8vec4(0, 0, 0, 255);
			}
			CHECK(drawn);
			CHECK(ParticleTriangles(setup) == 4);
		}

		// Snapshot data no extraction produces is a partial error (Architecture §8.2): the emitter is skipped, the others drawn.
		TEST_CASE("Particles: an emitter whose particle range lies outside the snapshot's is skipped with an InvalidArgument error" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(WhiteParticleTextureHandle, MakeParticleTexture(false));
			GraphicsDevice& device = gpu.GetDevice();
			RenderSnapshot drawn = MakeParticleSnapshot(setup);
			const std::vector<ParticleItem> particles = { { .Position = glm::vec3(0.0f), .Size = 2.0f, .Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f) } };
			AddEmitter(drawn, WhiteParticleTextureHandle, RenderParticleBlend::Alpha, particles, UUID(79));
			const Image expected = setup.Render(drawn);
			REQUIRE(ParticlePixel(expected, 32, 32) == glm::u8vec4(255, 0, 0, 255));

			RenderSnapshot snapshot = drawn;
			snapshot.FrameIndex += 1;
			snapshot.ParticleEmitters.push_back(ParticleEmitterItem{ .Texture = WhiteParticleTextureHandle, .First = 0, .Count = 2, .Entity = UUID(80) });
			Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
			REQUIRE(list.has_value());
			(*list)->open();
			const Status rendered = setup.Renderer().Render(**list, snapshot);
			(*list)->close();
			setup.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(**list));
			REQUIRE_FALSE(rendered.has_value());
			CHECK(rendered.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(setup.Read().Pixels == expected.Pixels);
		}

		// Decision 29 (the recreation protocol): the pass's pipelines, samplers and buffers are SceneRendererPipelines' (M33's
		// ParticlePass, place 1), its textures the cache's mirrors (the built-in particle texture's included) and its binding
		// sets the renderer's, so a recovered renderer draws the same particles.
		TEST_CASE("Particles: a renderer drawing particles recovers from a device loss with the same image" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(WhiteParticleTextureHandle, MakeParticleTexture(false));
			GraphicsDevice& device = gpu.GetDevice();
			// The built-in disc over the left half (alpha) and an additive red square on the right.
			const std::vector<ParticleItem> disc = { { .Position = glm::vec3(-2.0f, 0.0f, 0.0f), .Size = 4.0f, .Color = glm::vec4(1.0f) } };
			const std::vector<ParticleItem> glow = { { .Position = glm::vec3(2.0f, 0.0f, 0.0f), .Size = 2.0f, .Color = glm::vec4(1.0f, 0.0f, 0.0f, 0.5f) },
				{ .Position = glm::vec3(2.0f, 0.0f, 0.0f), .Size = 2.0f, .Color = glm::vec4(1.0f, 0.0f, 0.0f, 0.5f) } };
			uint64_t frame = 50;
			Test::DeviceLossTest test;
			test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
			// One use renders the snapshot into each of the renderer's frames in flight, so its profiler holds the timer queries
			// of every frame slot (each slot's are created at its first frame) before the loss as after the recovery.
			test.Use = [&device, &setup, &frame, &disc, &glow]() -> Result<std::vector<std::byte>>
			{
				std::vector<std::byte> pixels;
				for (uint32_t slot = 0; slot < device.GetFramesInFlight(); ++slot)
				{
					RenderSnapshot snapshot = MakeParticleSnapshot(setup);
					AddEmitter(snapshot, AssetHandle(), RenderParticleBlend::Alpha, disc, UUID(81));
					AddEmitter(snapshot, WhiteParticleTextureHandle, RenderParticleBlend::Additive, glow, UUID(82));
					snapshot.FrameIndex = ++frame;
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
					ENGINE_TRY_ASSIGN(Image read, readback.ReadTexture(*image));
					pixels = std::move(read.Pixels);
				}
				return pixels;
			};
			// The fixture creates its renderer's targets at the 64 x 64 view size, which the pixel-art snapshot's 16 x 16 scene
			// leaves free in the pool until EndFrame releases them, DefaultKeepFrames frames later, while the recovery creates
			// the targets of the last scene resolution only: the renderer reaches that steady state first, so the live counts
			// before the loss are the ones the recovery reproduces.
			for (uint32_t warmUp = 0; warmUp <= RenderTargetPool::DefaultKeepFrames; ++warmUp)
			{
				const Result<std::vector<std::byte>> steady = test.Use();
				REQUIRE_MESSAGE(steady.has_value(), steady.error().ToString());
			}
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			// Both emitters drew: the disc's centre (scene pixel (4, 8)) is near white, the glow's (scene pixel (12, 8)) red.
			REQUIRE(report->Before.size() == 64 * 64 * 4);
			const auto byte = [&report](uint32_t x, uint32_t y, size_t channel)
			{
				return std::to_integer<int>(report->Before[(static_cast<size_t>(y) * 64 + x) * 4 + channel]);
			};
			CHECK(byte(17, 33, 1) >= 240);
			CHECK(byte(49, 33, 0) == 255);
			CHECK(byte(49, 33, 1) == 0);
		}
	}

}
