#include "TestsPCH.h"

#include "Engine/Renderer/TaaPass.h"

#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/HostImageUpload.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderPassDeclaration.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Engine/Renderer/TemporalHistory.h"
#include "Shared/TaaConstants.h"
#include "Support/DeviceLossTest.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// Temporal anti-aliasing (M26, C26.1, R-TAA-VEL, R-TAA; Docs/Decisions/0037-m26-contract.md decisions 10, 11, 14, 17 and 29,
// stream X-C): the Taa pass's resolve, the prepass's motion vectors and specular inputs, the renderer's jitter and its rule
// (a Lit view outside pixel-art mode), the unjittered passes after the HDR chain and the pass's loss test; the byte-identical
// captures after the warm-up are ViewportCaptureTests.cpp's.

namespace Engine {

	namespace {

		using HalfTexel = std::array<uint16_t, 4>;

		constexpr AssetHandle StripHandle(0x7aa0000000000001);
		constexpr AssetHandle StripMaterial(0x7aa0000000000002);
		constexpr AssetHandle HalfRoughMaterial(0x7aa0000000000003);

		uint16_t ToHalf(float value)
		{
			return glm::packHalf1x16(value);
		}

		// A uniform grey texel `value` with alpha 1.
		HalfTexel Grey(float value)
		{
			const uint16_t half = ToHalf(value);
			return { half, half, half, ToHalf(1.0f) };
		}

		// A shader-readable texture of `width` x `height` texels of `format` holding `bytes`, rows top first, from the staging
		// upload (a copy destination too).
		nvrhi::TextureHandle CreateFilledTexture(GraphicsDevice& device, nvrhi::TextureDesc desc, std::span<const std::byte> bytes)
		{
			desc.isShaderResource = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			const std::array<TextureSubresourceData, 1> subresources = { { { .Data = bytes } } };
			Result<TextureUpload> texture = device.GetHostImageUpload().CreateTexture(desc, subresources, TextureUploadPath::Staging);
			REQUIRE_MESSAGE(texture.has_value(), texture.error().ToString());
			return texture->Texture;
		}

		nvrhi::TextureDesc MakeDesc(uint32_t width, uint32_t height, nvrhi::Format format, const char* name)
		{
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = format;
			desc.debugName = name;
			return desc;
		}

		// The RGBA16F texels of a read-back image as floats.
		std::vector<glm::vec4> ReadHalfTexels(GraphicsDevice& device, nvrhi::ITexture& texture)
		{
			Readback readback(device);
			const Result<Image> image = readback.ReadTexture(texture);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			std::vector<HalfTexel> halves(image->Pixels.size() / sizeof(HalfTexel));
			std::memcpy(halves.data(), image->Pixels.data(), halves.size() * sizeof(HalfTexel));
			std::vector<glm::vec4> texels;
			texels.reserve(halves.size());
			for (const HalfTexel& half : halves)
				texels.emplace_back(glm::unpackHalf1x16(half[0]), glm::unpackHalf1x16(half[1]), glm::unpackHalf1x16(half[2]), glm::unpackHalf1x16(half[3]));
			return texels;
		}

		// The Taa pass on the fixture's device over `width` x `height` images; Run uploads the current image, the motion
		// vectors and (unless reset) the history, records and executes one pass, and returns the resolved SceneColor.
		class TaaSetup
		{
		public:
			TaaSetup(Test::HeadlessGpuFixture& gpu, uint32_t width, uint32_t height)
				: m_Device(gpu.GetDevice()), m_Width(width), m_Height(height)
			{
				Result<Scope<TaaPass>> pass = TaaPass::Create(m_Device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				m_Pass = std::move(*pass);
				CHECK(m_Pass->GetPipelineCount() == TaaPass::PipelineCount);
				Result<nvrhi::TextureHandle> resolved = m_Device.CreateTexture(TaaPass::GetHistoryDesc(width, height));
				REQUIRE(resolved.has_value());
				m_Resolved = *resolved;
			}

			~TaaSetup() { m_Device.WaitForIdle(); }
			TaaSetup(const TaaSetup&) = delete;
			TaaSetup& operator=(const TaaSetup&) = delete;

			std::vector<glm::vec4> Run(std::span<const HalfTexel> current, std::span<const glm::vec2> motion, std::optional<std::span<const HalfTexel>> history)
			{
				nvrhi::TextureHandle sceneColor = CreateFilledTexture(m_Device, MakeDesc(m_Width, m_Height, SceneColorFormat, "Taa.SceneColor"), std::as_bytes(current));
				std::vector<uint16_t> motionHalves;
				for (const glm::vec2& vector : motion)
				{
					motionHalves.push_back(ToHalf(vector.x));
					motionHalves.push_back(ToHalf(vector.y));
				}
				nvrhi::TextureHandle velocity = CreateFilledTexture(m_Device, MakeDesc(m_Width, m_Height, SceneVelocityFormat, "Taa.Velocity"), std::as_bytes(std::span(motionHalves)));
				nvrhi::TextureHandle previous;
				if (history.has_value())
					previous = CreateFilledTexture(m_Device, MakeDesc(m_Width, m_Height, TaaHistoryFormat, "Taa.History"), std::as_bytes(*history));
				Result<nvrhi::CommandListHandle> list = m_Device.CreateCommandList();
				REQUIRE(list.has_value());
				(*list)->open();
				RenderPassCounters counters;
				const Status recorded = m_Pass->Record(**list, m_Bindings,
					{ .SceneColor = sceneColor, .Velocity = velocity, .History = previous.Get(), .Resolved = m_Resolved, .Reset = !history.has_value() }, counters);
				(*list)->close();
				REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
				CHECK(counters.Dispatches == 1);
				m_Device.ExecuteCommandList(**list);
				m_Device.WaitForIdle();
				// The pass copies the resolved image into SceneColor, which bloom, exposure and the tonemap read next.
				const std::vector<glm::vec4> resolved = ReadHalfTexels(m_Device, *m_Resolved);
				CHECK(ReadHalfTexels(m_Device, *sceneColor) == resolved);
				return resolved;
			}

			[[nodiscard]] TaaPass& GetPass() { return *m_Pass; }
			[[nodiscard]] nvrhi::ITexture* GetResolved() { return m_Resolved; }
		private:
			GraphicsDevice& m_Device;
			uint32_t m_Width = 0;
			uint32_t m_Height = 0;
			Scope<TaaPass> m_Pass;
			nvrhi::TextureHandle m_Resolved;
			PassBindingCache m_Bindings;
		};

		// The scene pixel (x right, y down) of world point `world` seen by `camera` in a `width` x `height` view.
		glm::vec2 ProjectToPixel(const glm::mat4& viewProjection, const glm::vec3& world, uint32_t width, uint32_t height)
		{
			const glm::vec4 clip = viewProjection * glm::vec4(world, 1.0f);
			const glm::vec2 ndc = glm::vec2(clip) / clip.w;
			return glm::vec2((ndc.x * 0.5f + 0.5f) * static_cast<float>(width), (0.5f - ndc.y * 0.5f) * static_cast<float>(height));
		}

		// The last render's SceneVelocity of the fixture's renderer, one motion vector per scene pixel.
		std::vector<glm::vec2> ReadVelocity(GraphicsDevice& device, SceneRenderer& renderer)
		{
			nvrhi::ITexture* texture = renderer.GetSceneVelocityTexture();
			REQUIRE(texture != nullptr);
			CHECK(texture->getDesc().format == SceneVelocityFormat);
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

		// The last render's SceneRoughness of the fixture's renderer, two words per scene pixel.
		std::vector<glm::uvec2> ReadRoughness(GraphicsDevice& device, SceneRenderer& renderer)
		{
			nvrhi::ITexture* texture = renderer.GetSceneRoughnessTexture();
			REQUIRE(texture != nullptr);
			CHECK(texture->getDesc().format == SceneRoughnessFormat);
			Readback readback(device);
			const Result<Image> image = readback.ReadTexture(*texture);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			std::vector<glm::uvec2> words(image->Pixels.size() / sizeof(glm::uvec2));
			std::memcpy(words.data(), image->Pixels.data(), words.size() * sizeof(glm::uvec2));
			return words;
		}

		uint32_t GetByte(uint32_t word, uint32_t index)
		{
			return (word >> (8 * index)) & 0xFFu;
		}

		// `snapshot` continuing its view's history: the previous frame's camera is its own and every item's previous world is
		// its world (nothing moved), with TAA's jitter of frame `frame`.
		RenderSnapshot Continued(RenderSnapshot snapshot, uint32_t frame)
		{
			snapshot.Camera.PreviousViewProjection = snapshot.Camera.Projection * snapshot.Camera.View;
			for (MeshDrawItem& item : snapshot.Meshes)
				item.PreviousWorld = item.World;
			snapshot.Camera.Jitter = GetTaaJitter(frame);
			return snapshot;
		}

		// The fixture's quad, lit, with TAA (its history reset, no jitter).
		RenderSnapshot MakeTaaSnapshot(const Test::SceneRendererIntegrationFixture& setup)
		{
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Lights = { LightData{ .Type = RenderLightType::Directional, .Intensity = 2.0f, .Direction = glm::vec3(0.0f, 0.0f, -1.0f) } };
			snapshot.Meshes[0].World = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -3.0f)), 0.3f, glm::vec3(0.0f, 0.0f, 1.0f));
			snapshot.Post.AntiAliasing = RenderAntiAliasing::Taa;
			return snapshot;
		}

		// A strip of two quads along +Y facing +Z (the bottom row on joint 0, the top row on joint 1, the middle row half each).
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
			return mesh;
		}

	}

	TEST_SUITE("Renderer")
	{
		// The Taa declaration holds the pass's one pipeline of its program, and its history images are SceneColor's format,
		// readable, writable by the resolve and kept readable between command lists.
		TEST_CASE("TaaPass: its declaration holds its one pipeline of the program Taa, and its history images TaaHistoryFormat")
		{
			const auto taa = std::find_if(ScenePassDeclarations.begin(), ScenePassDeclarations.end(), [](const RenderPassDeclaration& declaration)
			{
				return declaration.Name == "Taa";
			});
			REQUIRE(taa != ScenePassDeclarations.end());
			CHECK(taa->Stage == RenderStage::HdrPost);
			CHECK(taa->PipelineCount == TaaPass::PipelineCount);
			CHECK(TaaPass::PipelineCount == 1);
			REQUIRE(taa->Programs.size() == 1);
			CHECK(taa->Programs[0] == "Taa");
			REQUIRE(taa->Targets.size() == 1);
			CHECK(taa->Targets[0] == "TaaHistory");
			CHECK(TaaPass::GetLayoutDescriptions().size() == TaaPass::PipelineCount);
			const nvrhi::TextureDesc history = TaaPass::GetHistoryDesc(40, 20);
			CHECK(history.width == 40);
			CHECK(history.height == 20);
			CHECK(history.format == TaaHistoryFormat);
			CHECK(TaaHistoryFormat == SceneColorFormat);
			CHECK(history.isShaderResource);
			CHECK(history.isUAV);
			CHECK(history.keepInitialState);
		}

		// The push constants carry the image's size, the fixed feedback and the reset flag.
		TEST_CASE("TaaPass: the push constants carry the image, the feedback and the reset")
		{
			TaaPassInputs inputs;
			inputs.Reset = false;
			const TaaConstants continued = MakeTaaConstants(inputs, 40, 20);
			CHECK(continued.ImageSize == glm::uvec2(40, 20));
			CHECK(continued.InverseImageSize == glm::vec2(1.0f / 40.0f, 1.0f / 20.0f));
			CHECK(continued.Feedback == TaaFeedback);
			CHECK(continued.Flags == 0);
			inputs.Reset = true;
			CHECK(MakeTaaConstants(inputs, 40, 20).Flags == TaaFlagReset);
			CHECK(TaaFeedback > 0.0f);
			CHECK(TaaFeedback < 1.0f);
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		// Decision 17's resolve: a reset is the current image exactly; a history inside the current neighbourhood's box is
		// blended with the feedback; one outside it is clamped to the box first; the history is read at the pixel's previous
		// position (its centre minus its motion vector); and a previous position off the image resolves to the current pixel.
		TEST_CASE("TaaPass: the resolve blends the reprojected history, clamped to the current neighbourhood, with the current image")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				constexpr uint32_t Width = 16;
				constexpr uint32_t Height = 8;
				constexpr size_t Count = static_cast<size_t>(Width) * Height;
				TaaSetup setup(gpu, Width, Height);
				const auto at = [](uint32_t x, uint32_t y)
				{
					return static_cast<size_t>(y) * Width + x;
				};
				const std::vector<glm::vec2> still(Count, glm::vec2(0.0f));

				// A checkerboard of 0 and 1 (every 3 x 3 box spans [0, 1]) against a uniform history of 0.5.
				std::vector<HalfTexel> checker(Count);
				for (uint32_t y = 0; y < Height; ++y)
					for (uint32_t x = 0; x < Width; ++x)
						checker[at(x, y)] = Grey((x + y) % 2 == 0 ? 0.0f : 1.0f);
				const std::vector<HalfTexel> half(Count, Grey(0.5f));

				// A reset resolves to the current image, bit for bit.
				const std::vector<glm::vec4> reset = setup.Run(checker, still, std::nullopt);
				for (size_t index = 0; index < Count; ++index)
					CHECK(reset[index] == glm::vec4(glm::vec3((index / Width + index % Width) % 2 == 0 ? 0.0f : 1.0f), 1.0f));

				// Inside the box: lerp(current, history, feedback).
				const std::vector<glm::vec4> blended = setup.Run(checker, still, std::span<const HalfTexel>(half));
				for (const uint32_t x : { 0U, 5U, 8U, 15U })
				{
					for (const uint32_t y : { 0U, 3U, 7U })
					{
						CAPTURE(x);
						CAPTURE(y);
						const float current = (x + y) % 2 == 0 ? 0.0f : 1.0f;
						const float expected = current + TaaFeedback * (0.5f - current);
						CHECK(std::abs(blended[at(x, y)].r - expected) <= 1e-3f);
						CHECK(blended[at(x, y)].a == 1.0f);
					}
				}

				// Outside the box: a uniform current image of 1 clamps a history of 4 to 1.
				const std::vector<HalfTexel> one(Count, Grey(1.0f));
				const std::vector<HalfTexel> four(Count, Grey(4.0f));
				const std::vector<glm::vec4> clamped = setup.Run(one, still, std::span<const HalfTexel>(four));
				for (const glm::vec4& texel : clamped)
					CHECK(texel == glm::vec4(1.0f));

				// Reprojected: a ramp (texel x holds x) moving one pixel right reads the history at x - 1.
				std::vector<HalfTexel> ramp(Count);
				std::vector<HalfTexel> rampHistory(Count);
				for (uint32_t y = 0; y < Height; ++y)
				{
					for (uint32_t x = 0; x < Width; ++x)
					{
						ramp[at(x, y)] = Grey(static_cast<float>(x));
						rampHistory[at(x, y)] = Grey(static_cast<float>(x) + 0.25f);
					}
				}
				const std::vector<glm::vec2> right(Count, glm::vec2(1.0f, 0.0f));
				const std::vector<glm::vec4> moved = setup.Run(ramp, right, std::span<const HalfTexel>(rampHistory));
				for (const uint32_t x : { 1U, 7U, 14U })
				{
					CAPTURE(x);
					const float current = static_cast<float>(x);
					const float previous = static_cast<float>(x - 1) + 0.25f;
					CHECK(std::abs(moved[at(x, 4)].g - (current + TaaFeedback * (previous - current))) <= 2e-2f);
				}
				// Column 0's previous position is off the image: the current pixel.
				CHECK(moved[at(0, 4)].g == 0.0f);
				// Far off the image everywhere: the current image.
				const std::vector<glm::vec2> away(Count, glm::vec2(100.0f, 0.0f));
				const std::vector<glm::vec4> outside = setup.Run(ramp, away, std::span<const HalfTexel>(rampHistory));
				for (uint32_t x = 0; x < Width; ++x)
					CHECK(outside[at(x, 2)].r == static_cast<float>(x));
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("TaaPass: Record refuses missing images, a history that is the resolved image and sizes that differ")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				GraphicsDevice& device = gpu.GetDevice();
				TaaSetup setup(gpu, 8, 8);
				const std::vector<HalfTexel> texels(64, Grey(1.0f));
				const std::vector<uint16_t> zero(128, 0);
				nvrhi::TextureHandle color = CreateFilledTexture(device, MakeDesc(8, 8, SceneColorFormat, "Taa.Color"), std::as_bytes(std::span(texels)));
				nvrhi::TextureHandle velocity = CreateFilledTexture(device, MakeDesc(8, 8, SceneVelocityFormat, "Taa.Velocity"), std::as_bytes(std::span(zero)));
				Result<nvrhi::TextureHandle> small = device.CreateTexture(TaaPass::GetHistoryDesc(4, 8));
				REQUIRE(small.has_value());
				nvrhi::ITexture* resolved = setup.GetResolved();
				Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
				REQUIRE(list.has_value());
				(*list)->open();
				PassBindingCache bindings;
				RenderPassCounters counters;
				const std::array<TaaPassInputs, 6> invalid = { {
					{ .SceneColor = nullptr, .Velocity = velocity, .Resolved = resolved },
					{ .SceneColor = color, .Velocity = nullptr, .Resolved = resolved },
					{ .SceneColor = color, .Velocity = velocity, .Resolved = nullptr },
					{ .SceneColor = color, .Velocity = velocity, .History = nullptr, .Resolved = resolved, .Reset = false },
					{ .SceneColor = color, .Velocity = velocity, .History = resolved, .Resolved = resolved, .Reset = false },
					{ .SceneColor = color, .Velocity = velocity, .History = *small, .Resolved = resolved, .Reset = false },
				} };
				for (size_t index = 0; index < invalid.size(); ++index)
				{
					CAPTURE(index);
					const Status recorded = setup.GetPass().Record(**list, bindings, invalid[index], counters);
					REQUIRE_FALSE(recorded.has_value());
					CHECK(recorded.error().GetCode() == ErrorCode::InvalidArgument);
				}
				CHECK(counters.Dispatches == 0);
				(*list)->close();
				device.ExecuteCommandList(**list);
				device.WaitForIdle();
				CHECK(bindings.GetSize() == 0);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// Decision 11 and 14's motion vectors: a quad that moved half a unit right since the previous frame, or one seen by a
		// camera that moved half a unit left, moves right on screen by the projected distance at every pixel it covers; the
		// background has none; the jitter of a TAA render is not in them (both positions are unjittered); and a reset history
		// has no motion.
		TEST_CASE("SceneRenderer: the prepass writes each pixel's motion since the previous frame into SceneVelocity")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				GraphicsDevice& device = gpu.GetDevice();
				uint64_t frame = 100;
				const auto render = [&setup, &frame](RenderSnapshot snapshot)
				{
					snapshot.FrameIndex = ++frame;
					setup.Submit(snapshot);
				};
				const RenderSnapshot base = setup.Snapshot();
				const glm::mat4 viewProjection = base.Camera.Projection * base.Camera.View;
				const glm::vec2 shift = ProjectToPixel(viewProjection, glm::vec3(0.0f, 0.0f, -3.0f), 64, 64)
					- ProjectToPixel(viewProjection, glm::vec3(-0.5f, 0.0f, -3.0f), 64, 64);
				REQUIRE(shift.x > 5.0f);
				const auto at = [](uint32_t x, uint32_t y)
				{
					return static_cast<size_t>(y) * 64 + x;
				};
				const auto checkQuad = [&at, &shift](const std::vector<glm::vec2>& motion)
				{
					for (const uint32_t x : { 25U, 32U, 38U })
					{
						for (const uint32_t y : { 25U, 32U, 38U })
						{
							CAPTURE(x);
							CAPTURE(y);
							CHECK(std::abs(motion[at(x, y)].x - shift.x) <= 0.05f);
							CHECK(std::abs(motion[at(x, y)].y) <= 0.05f);
						}
					}
					CHECK(motion[at(1, 1)] == glm::vec2(0.0f));
					CHECK(motion[at(62, 60)] == glm::vec2(0.0f));
				};

				// The quad moved: its previous world was half a unit left.
				RenderSnapshot moved = base;
				moved.Camera.PreviousViewProjection = viewProjection;
				moved.Meshes[0].PreviousWorld = glm::translate(glm::mat4(1.0f), glm::vec3(-0.5f, 0.0f, -3.0f));
				render(moved);
				checkQuad(ReadVelocity(device, setup.Renderer()));

				// The camera moved: it was half a unit right (the quad seen half a unit further left).
				RenderSnapshot panned = base;
				panned.Camera.PreviousViewProjection = base.Camera.Projection * glm::translate(base.Camera.View, glm::vec3(-0.5f, 0.0f, 0.0f));
				render(panned);
				checkQuad(ReadVelocity(device, setup.Renderer()));

				// With TAA's jitter the motion is the same: both positions are unjittered.
				RenderSnapshot jittered = moved;
				jittered.Post.AntiAliasing = RenderAntiAliasing::Taa;
				jittered.Camera.Jitter = glm::vec2(0.375f, -0.25f);
				render(jittered);
				checkQuad(ReadVelocity(device, setup.Renderer()));

				// A reset history (no previous camera or world) has no motion; the picking prepass writes the same targets.
				RenderSnapshot reset = base;
				reset.Flags = RenderViewFlags::Picking;
				render(reset);
				const std::vector<glm::vec2> none = ReadVelocity(device, setup.Renderer());
				CHECK(std::all_of(none.begin(), none.end(), [](const glm::vec2& motion)
				{
					return motion == glm::vec2(0.0f);
				}));
				RenderSnapshot pickedMoved = moved;
				pickedMoved.Flags = RenderViewFlags::Picking;
				render(pickedMoved);
				checkQuad(ReadVelocity(device, setup.Renderer()));
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// Decision 11: a deformed item that continues its view's history moves from its previous deformation (M23's skinned
		// previous positions), so its top row, whose joint moved, has the joint's motion and its bottom row none; after a reset
		// it has no motion, whatever its pose did.
		TEST_CASE("SceneRenderer: a skinned item's motion vectors follow its previous deformation")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				GraphicsDevice& device = gpu.GetDevice();
				setup.Assets().Publish(StripHandle, MakeStrip());
				setup.Assets().Publish(StripMaterial, CreateRef<MaterialData>());
				uint64_t frame = 200;
				const auto render = [&setup, &frame](RenderSnapshot snapshot)
				{
					snapshot.FrameIndex = ++frame;
					setup.Submit(snapshot);
				};
				RenderSnapshot snapshot = setup.Snapshot();
				const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -4.0f));
				snapshot.Meshes = { MeshDrawItem{ .World = world, .Mesh = StripHandle, .Materials = { StripMaterial }, .Entity = UUID(77) } };
				const auto pose = [](float shift)
				{
					return std::vector<glm::mat4>{ glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(shift, 0.0f, 0.0f)) };
				};
				const glm::mat4 viewProjection = snapshot.Camera.Projection * snapshot.Camera.View;
				const float shift = (ProjectToPixel(viewProjection, glm::vec3(0.25f, 0.0f, -4.0f), 64, 64) - ProjectToPixel(viewProjection, glm::vec3(0.0f, 0.0f, -4.0f), 64, 64)).x;
				const glm::vec2 top = ProjectToPixel(viewProjection, glm::vec3(0.1f, 0.9f, -4.0f), 64, 64);
				const glm::vec2 bottom = ProjectToPixel(viewProjection, glm::vec3(0.0f, -0.9f, -4.0f), 64, 64);
				const auto sample = [](const std::vector<glm::vec2>& motion, glm::vec2 pixel)
				{
					return motion[static_cast<size_t>(pixel.y) * 64 + static_cast<size_t>(pixel.x)];
				};

				snapshot.Meshes[0].JointPalette = pose(0.0f);
				render(snapshot);
				// M28 (Docs/Decisions/0043-m28-game-services.md decision 4): the render stats count the skinned mesh the Deform
				// stage deformed.
				CHECK(setup.Renderer().GetRenderStats().SkinnedMeshes == 1);
				RenderSnapshot continued = snapshot;
				continued.Camera.PreviousViewProjection = viewProjection;
				continued.Meshes[0].PreviousWorld = world;
				continued.Meshes[0].JointPalette = pose(0.25f);
				render(continued);
				const std::vector<glm::vec2> motion = ReadVelocity(device, setup.Renderer());
				// The top row's vertices moved by the whole shift; the pixel near the top row by most of it.
				CHECK(sample(motion, top).x > 0.8f * shift);
				CHECK(sample(motion, top).x <= shift + 0.05f);
				CHECK(std::abs(sample(motion, top).y) <= 0.05f);
				CHECK(std::abs(sample(motion, bottom).x) <= 0.1f * shift);

				// A reset after another pose change: no motion anywhere.
				RenderSnapshot reset = snapshot;
				reset.Meshes[0].JointPalette = pose(0.5f);
				render(reset);
				const std::vector<glm::vec2> none = ReadVelocity(device, setup.Renderer());
				CHECK(sample(none, top) == glm::vec2(0.0f));
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// Decision 14's specular inputs: a flat dielectric of roughness 0.5 has a = 128 (the roughness the forward pass samples
		// at, unwidened on a flat surface), a grey multiplier and its occlusion 1 (255); the background is zero.
		TEST_CASE("SceneRenderer: the prepass writes the forward pass's specular inputs into SceneRoughness")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				GraphicsDevice& device = gpu.GetDevice();
				Ref<MaterialData> material = CreateRef<MaterialData>();
				material->Roughness = 0.5f;
				material->Metallic = 0.0f;
				setup.Assets().Publish(HalfRoughMaterial, material);
				RenderSnapshot snapshot = setup.Snapshot();
				snapshot.Meshes[0].Materials = { HalfRoughMaterial };
				for (const bool picking : { false, true })
				{
					CAPTURE(picking);
					snapshot.Flags = picking ? RenderViewFlags::Picking : RenderViewFlags::None;
					snapshot.FrameIndex += 1;
					setup.Submit(snapshot);
					const std::vector<glm::uvec2> words = ReadRoughness(device, setup.Renderer());
					const glm::uvec2 centre = words[static_cast<size_t>(32) * 64 + 32];
					CHECK(GetByte(centre.x, 3) == 128);
					CHECK(GetByte(centre.x, 0) == GetByte(centre.x, 1));
					CHECK(GetByte(centre.x, 1) == GetByte(centre.x, 2));
					CHECK(GetByte(centre.x, 0) > 0);
					CHECK(GetByte(centre.x, 0) < 255);
					CHECK(centre.y == 255u);
					CHECK(words[0] == glm::uvec2(0));
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// Decision 10's rule in the renderer: a Lit view with TAA records the TAA row and resolves to its current image after a
		// reset (so a reset render with no jitter equals the render without TAA, byte for byte), and a continued history of
		// jittered renders resolves to another image; a reset render starts afresh. The debug views record no TAA row.
		TEST_CASE("SceneRenderer: TAA resolves the jittered image against the view's history, which a reset starts afresh")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				uint64_t frame = 300;
				const auto render = [&setup, &frame](RenderSnapshot snapshot)
				{
					snapshot.FrameIndex = ++frame;
					return setup.Render(snapshot);
				};
				const auto recordedTaa = [&setup]() -> std::optional<RenderPassStats>
				{
					const RenderStats stats = setup.Renderer().GetRenderStats();
					const RenderPassStats* row = Test::SceneRendererIntegrationFixture::FindPass(stats, "TAA");
					return row != nullptr ? std::optional<RenderPassStats>(*row) : std::nullopt;
				};
				const RenderSnapshot taa = MakeTaaSnapshot(setup);
				RenderSnapshot none = taa;
				none.Post.AntiAliasing = RenderAntiAliasing::None;
				const Image plain = render(none);
				CHECK_FALSE(recordedTaa().has_value());

				const Image reset = render(taa);
				const std::optional<RenderPassStats> row = recordedTaa();
				REQUIRE(row.has_value());
				CHECK(row->Dispatches == 1);
				CHECK(reset.Pixels == plain.Pixels);

				CHECK(setup.Renderer().GetRenderStats().HistoryFrames == 0);
				Image resolved = reset;
				for (uint32_t index = 1; index < CaptureWarmupFrames; ++index)
				{
					resolved = render(Continued(taa, index));
					// W26.2's review: the stats count the renders since the snapshot's history was reset.
					CHECK(setup.Renderer().GetRenderStats().HistoryFrames == index);
				}
				CHECK(resolved.Pixels != plain.Pixels);
				CHECK(render(taa).Pixels == plain.Pixels);
				CHECK(setup.Renderer().GetRenderStats().HistoryFrames == 0);

				RenderSnapshot albedo = Continued(taa, 1);
				albedo.DebugView = RenderDebugView::Albedo;
				static_cast<void>(render(albedo));
				CHECK_FALSE(recordedTaa().has_value());

				// W26.2's review (decision 32 (b)): a render that skipped the pass leaves no history, so the next TAA render
				// starts afresh even though its snapshot continues the view's history: it equals a reset render of the same
				// jitter (the current image alone), not a blend with the history the debug view left behind.
				const Image afterSkip = render(Continued(taa, 2));
				REQUIRE(recordedTaa().has_value());
				RenderSnapshot freshJittered = taa;
				freshJittered.Camera.Jitter = GetTaaJitter(2);
				CHECK(render(freshJittered).Pixels == afterSkip.Pixels);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// The acceptance (decision 10): pixel-art mode keeps M18's 2D colour path, where Taa acts as None: no TAA row and no
		// jitter, so a pixel-art render with TAA and a jitter equals the one without TAA, byte for byte.
		TEST_CASE("SceneRenderer: TAA is off in pixel-art mode")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				uint64_t frame = 400;
				const auto render = [&setup, &frame](RenderSnapshot snapshot)
				{
					snapshot.FrameIndex = ++frame;
					return setup.Render(snapshot);
				};
				RenderSnapshot pixelArt = Continued(MakeTaaSnapshot(setup), 3);
				pixelArt.Output.PixelArt = true;
				const Image withTaa = render(pixelArt);
				const RenderStats stats = setup.Renderer().GetRenderStats();
				CHECK(Test::SceneRendererIntegrationFixture::FindPass(stats, "TAA") == nullptr);
				RenderSnapshot plain = pixelArt;
				plain.Post.AntiAliasing = RenderAntiAliasing::None;
				plain.Camera.Jitter = glm::vec2(0.0f);
				CHECK(render(plain).Pixels == withTaa.Pixels);
				// Outside pixel-art mode the same snapshot records the pass and its jitter changes the image.
				pixelArt.Output.PixelArt = false;
				const Image lit = render(pixelArt);
				CHECK(Test::SceneRendererIntegrationFixture::FindPass(setup.Renderer().GetRenderStats(), "TAA") != nullptr);
				plain.Output.PixelArt = false;
				CHECK(render(plain).Pixels != lit.Pixels);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// Decision 10: the passes after the HDR chain draw with the unjittered constants, so a TAA view's debug lines land where
		// the view without TAA draws them (over a uniform image, which TAA's reset leaves as it is), however it is jittered.
		TEST_CASE("SceneRenderer: with TAA the passes after the HDR chain draw unjittered")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				uint64_t frame = 500;
				const auto render = [&setup, &frame](RenderSnapshot snapshot)
				{
					snapshot.FrameIndex = ++frame;
					return setup.Render(snapshot);
				};
				RenderSnapshot lines = setup.Snapshot();
				lines.Meshes.clear();
				lines.Camera.ClearColor = glm::vec3(0.25f);
				lines.DebugDraw.AddLine(glm::vec3(-0.6f, -0.45f, -3.0f), glm::vec3(0.7f, 0.55f, -3.0f), glm::vec4(1.0f), 0.0f, DebugDepthMode::OnTop);
				lines.DebugDraw.AddLine(glm::vec3(-0.8f, 0.31f, -3.0f), glm::vec3(0.9f, 0.27f, -3.0f), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), 0.0f, DebugDepthMode::OnTop);
				const Image plain = render(lines);
				RenderSnapshot jittered = lines;
				jittered.Post.AntiAliasing = RenderAntiAliasing::Taa;
				jittered.Camera.Jitter = glm::vec2(0.45f, -0.4f);
				CHECK(render(jittered).Pixels == plain.Pixels);
				CHECK(Test::SceneRendererIntegrationFixture::FindPass(setup.Renderer().GetRenderStats(), "TAA") != nullptr);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// Decision 29: the Taa pass is the pipeline set's (place 1) and its history images, its targets and its unjittered
		// constants the renderer's (place 2); after a device loss they are created again, and a reset render followed by
		// continued jittered ones gives the same image as before the loss.
		TEST_CASE("TaaPass: a renderer with TAA recovers from a device loss with the same image")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::SceneRendererIntegrationFixture setup(gpu);
				GraphicsDevice& device = gpu.GetDevice();
				uint64_t frame = 700;
				Test::DeviceLossTest test;
				test.Owners = { "SceneRendererPipelines", "SceneRenderer" };
				// One use renders the quad with a reset history, then three continued jittered frames, and returns the last image.
				test.Use = [&device, &setup, &frame]() -> Result<std::vector<std::byte>>
				{
					std::vector<std::byte> pixels;
					for (uint32_t index = 0; index < 4; ++index)
					{
						const RenderSnapshot base = MakeTaaSnapshot(setup);
						RenderSnapshot snapshot = index == 0 ? base : Continued(base, index);
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
				// The view's targets reach their steady state in the pool first, as the other renderer loss cases do.
				for (uint32_t warmUp = 0; warmUp <= RenderTargetPool::DefaultKeepFrames; ++warmUp)
				{
					const Result<std::vector<std::byte>> steady = test.Use();
					REQUIRE_MESSAGE(steady.has_value(), steady.error().ToString());
				}
				const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
				REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
				REQUIRE(report->Before.size() == 64 * 64 * 4);
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
