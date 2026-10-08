#include "TestsPCH.h"

#include "Engine/Renderer/SkyboxPass.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/HostImageUpload.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/ViewConstants.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/RenderReference.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

// The skybox pass (Architecture §8.3 pass 8, §8.6; Docs/Decisions/0013-m8-decisions.md decision 9) on synthetic inputs: a cube
// whose faces have distinct colours drawn into a cleared SceneColor.

namespace Engine {

	namespace {

		constexpr uint32_t TargetSize = 32;

		// A RGBA16F cube of FaceSize² texels per face: face f has the colour FaceColors[f] in every texel. Faces of several texels
		// keep the bilinear (seamless) filtering of a ray near a face's centre inside that face; with 1x1 faces any ray off the
		// exact centre would blend in the neighbouring faces.
		constexpr uint32_t FaceSize = 4;
		constexpr std::array<glm::vec3, 6> FaceColors = { glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1), glm::vec3(1, 1, 0),
			glm::vec3(0, 1, 1), glm::vec3(1, 0, 1) };

		nvrhi::TextureHandle CreateFaceColorCube(GraphicsDevice& device)
		{
			nvrhi::TextureDesc desc;
			desc.width = FaceSize;
			desc.height = FaceSize;
			desc.arraySize = 6;
			desc.dimension = nvrhi::TextureDimension::TextureCube;
			desc.format = nvrhi::Format::RGBA16_FLOAT;
			desc.isShaderResource = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = "SkyboxPassTests.Cube";
			std::array<std::array<std::array<uint16_t, 4>, FaceSize * FaceSize>, 6> texels{};
			std::vector<TextureSubresourceData> subresources;
			for (uint32_t face = 0; face < 6; ++face)
			{
				const std::array<uint16_t, 4> color = { Test::DoubleToHalf(FaceColors[face].r), Test::DoubleToHalf(FaceColors[face].g),
					Test::DoubleToHalf(FaceColors[face].b), Test::DoubleToHalf(1.0) };
				texels[face].fill(color);
				subresources.push_back({ .MipLevel = 0, .ArraySlice = face, .Data = std::as_bytes(std::span(texels[face])) });
			}
			Result<TextureUpload> upload = device.GetHostImageUpload().CreateTexture(desc, subresources, TextureUploadPath::Staging);
			REQUIRE_MESSAGE(upload.has_value(), upload.error().ToString());
			return upload->Texture;
		}

		// Environment constants with a map: `intensity` and the rotation's sine and cosine, the rest zero.
		EnvironmentConstants MakeEnvironment(float intensity, float rotationSin, float rotationCos)
		{
			EnvironmentConstants environment{};
			environment.Intensity = intensity;
			environment.RotationSin = rotationSin;
			environment.RotationCos = rotationCos;
			environment.HasEnvironment = 1;
			return environment;
		}

		// The pass, a SceneColor/SceneDepth framebuffer of TargetSize² and the constant buffers, on the fixture's device.
		class SkyboxSetup
		{
		public:
			explicit SkyboxSetup(Test::HeadlessGpuFixture& gpu)
				: m_Device(gpu.GetDevice())
			{
				Result<Scope<SkyboxPass>> pass = SkyboxPass::Create(m_Device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				m_Pass = std::move(*pass);
				m_Cube = CreateFaceColorCube(m_Device);

				nvrhi::TextureDesc colorDesc;
				colorDesc.width = TargetSize;
				colorDesc.height = TargetSize;
				colorDesc.format = SceneColorFormat;
				colorDesc.isRenderTarget = true;
				colorDesc.isShaderResource = true;
				colorDesc.initialState = nvrhi::ResourceStates::RenderTarget;
				colorDesc.keepInitialState = true;
				colorDesc.debugName = "SkyboxPassTests.SceneColor";
				nvrhi::TextureDesc depthDesc = colorDesc;
				depthDesc.format = SceneDepthFormat;
				depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
				depthDesc.debugName = "SkyboxPassTests.SceneDepth";
				Result<nvrhi::TextureHandle> color = m_Device.CreateTexture(colorDesc);
				Result<nvrhi::TextureHandle> depth = m_Device.CreateTexture(depthDesc);
				REQUIRE(color.has_value());
				REQUIRE(depth.has_value());
				m_Color = *color;
				m_Depth = *depth;
				Result<nvrhi::FramebufferHandle> framebuffer =
					m_Device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_Color).setDepthAttachment(m_Depth));
				REQUIRE(framebuffer.has_value());
				m_Framebuffer = *framebuffer;

				nvrhi::BufferDesc bufferDesc;
				bufferDesc.isConstantBuffer = true;
				bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
				bufferDesc.keepInitialState = true;
				bufferDesc.byteSize = sizeof(ViewConstants);
				Result<nvrhi::BufferHandle> viewBuffer = m_Device.CreateBuffer(bufferDesc);
				bufferDesc.byteSize = sizeof(EnvironmentConstants);
				Result<nvrhi::BufferHandle> environmentBuffer = m_Device.CreateBuffer(bufferDesc);
				REQUIRE(viewBuffer.has_value());
				REQUIRE(environmentBuffer.has_value());
				m_ViewBuffer = *viewBuffer;
				m_EnvironmentBuffer = *environmentBuffer;
			}

			[[nodiscard]] SkyboxPass& GetPass() { return *m_Pass; }

			// Renders the skybox for a camera at the origin looking down -Z (90 degrees, square) with `environment`, over a
			// depth of 0 everywhere, and returns the linear colour at the centre.
			glm::dvec3 RenderCentre(const EnvironmentConstants& environment)
			{
				return GetPixel(Render(environment, RenderProjection::Perspective), TargetSize / 2, TargetSize / 2);
			}

			// The linear RGB of pixel (x, y) of a SceneColor readback.
			static glm::dvec3 GetPixel(const Image& image, uint32_t x, uint32_t y)
			{
				std::array<uint16_t, 4> texel{};
				std::memcpy(texel.data(), image.GetRow(y).data() + static_cast<size_t>(x) * sizeof(texel), sizeof(texel));
				return glm::dvec3(Test::HalfToDouble(texel[0]), Test::HalfToDouble(texel[1]), Test::HalfToDouble(texel[2]));
			}

			// Renders the skybox for a camera at the origin looking down -Z with `environment` and returns SceneColor: a
			// 90-degree perspective camera, or an orthographic one 10 units high (near 0.1, far 100). SceneColor starts as
			// `clearColor`, and SceneDepth as 0 everywhere, or as `depth` (TargetSize² values, rows top first) when given.
			Image Render(const EnvironmentConstants& environment, RenderProjection projection, std::span<const float> depth = {},
				const glm::vec4& clearColor = glm::vec4(0.0f))
			{
				ViewConstants view{};
				view.Projection = ComputeReverseZProjection(projection, 90.0f, 10.0f, 0.1f, 100.0f, TargetSize, TargetSize);
				view.View = glm::mat4(1.0f);
				view.ViewProjection = view.Projection;
				view.InverseProjection = glm::inverse(view.Projection);
				view.InverseViewProjection = view.InverseProjection;
				view.ViewportSize = glm::vec2(static_cast<float>(TargetSize));
				view.InverseViewportSize = glm::vec2(1.0f / static_cast<float>(TargetSize));
				view.Near = 0.1f;
				if (projection == RenderProjection::Orthographic)
				{
					view.ProjectionKind = ProjectionKindOrthographic;
					view.OrthoHalfExtents = glm::vec2(5.0f);
					view.Far = 100.0f;
				}
				else
				{
					view.TanHalfFovY = 1.0f;
				}

				Result<nvrhi::CommandListHandle> commandList = m_Device.CreateCommandList();
				REQUIRE(commandList.has_value());
				(*commandList)->open();
				(*commandList)->writeBuffer(m_ViewBuffer, &view, sizeof(view));
				(*commandList)->writeBuffer(m_EnvironmentBuffer, &environment, sizeof(environment));
				(*commandList)->clearTextureFloat(m_Color, nvrhi::AllSubresources, nvrhi::Color(clearColor.r, clearColor.g, clearColor.b, clearColor.a));
				if (depth.empty())
				{
					(*commandList)->clearDepthStencilTexture(m_Depth, nvrhi::AllSubresources, true, 0.0f, false, 0);
				}
				else
				{
					REQUIRE(depth.size() == static_cast<size_t>(TargetSize) * TargetSize);
					(*commandList)->writeTexture(m_Depth, 0, 0, depth.data(), TargetSize * sizeof(float));
				}
				PassBindingCache bindings;
				const Status recorded = m_Pass->Record(**commandList, bindings,
					{ .Framebuffer = m_Framebuffer, .ViewConstants = m_ViewBuffer, .EnvironmentConstants = m_EnvironmentBuffer, .Skybox = m_Cube });
				(*commandList)->close();
				REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
				m_Device.ExecuteCommandList(**commandList);

				Readback readback(m_Device);
				Result<Image> image = readback.ReadTexture(*m_Color);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				return std::move(*image);
			}
		private:
			GraphicsDevice& m_Device;
			Scope<SkyboxPass> m_Pass;
			nvrhi::TextureHandle m_Cube;
			nvrhi::TextureHandle m_Color;
			nvrhi::TextureHandle m_Depth;
			nvrhi::FramebufferHandle m_Framebuffer;
			nvrhi::BufferHandle m_ViewBuffer;
			nvrhi::BufferHandle m_EnvironmentBuffer;
		};

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("SkyboxPass: fills the pixels left at depth 0 with the cube along each view ray"
			* doctest::test_suite(Test::GpuSuite))
		{
			// A camera at the origin looking down -Z sees the -Z face (magenta) at the centre.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				SkyboxSetup setup(gpu);
				CHECK(setup.GetPass().GetPipelineCount() == SkyboxPass::PipelineCount);
				const glm::dvec3 centre = setup.RenderCentre(MakeEnvironment(1.0f, 0.0f, 1.0f));
				CHECK(centre.x == doctest::Approx(1.0));
				CHECK(centre.y == doctest::Approx(0.0));
				CHECK(centre.z == doctest::Approx(1.0));

				// Geometry in the left half (SceneDepth 0.5 there, the colour a marker the opaque passes left): the skybox
				// leaves it untouched (depth 0 fails GreaterOrEqual against 0.5) and fills only the right half, where the ray of
				// an off-centre pixel (x/z = 0.53) still ends inside the -Z face.
				std::vector<float> depth(static_cast<size_t>(TargetSize) * TargetSize, 0.0f);
				for (uint32_t y = 0; y < TargetSize; ++y)
				{
					for (uint32_t x = 0; x < TargetSize / 2; ++x)
						depth[static_cast<size_t>(y) * TargetSize + x] = 0.5f;
				}
				const glm::vec4 marker(0.25f, 0.5f, 0.75f, 1.0f);
				const Image covered = setup.Render(MakeEnvironment(1.0f, 0.0f, 1.0f), RenderProjection::Perspective, depth, marker);
				for (const auto& [x, y] : { std::pair<uint32_t, uint32_t>(0, 0), std::pair<uint32_t, uint32_t>(TargetSize / 4, TargetSize / 2),
						 std::pair<uint32_t, uint32_t>(TargetSize / 2 - 1, TargetSize - 1) })
				{
					CAPTURE(x);
					CAPTURE(y);
					const glm::dvec3 kept = SkyboxSetup::GetPixel(covered, x, y);
					CHECK(kept.x == doctest::Approx(0.25));
					CHECK(kept.y == doctest::Approx(0.5));
					CHECK(kept.z == doctest::Approx(0.75));
				}
				const glm::dvec3 sky = SkyboxSetup::GetPixel(covered, TargetSize * 3 / 4, TargetSize / 2);
				CHECK(sky.x == doctest::Approx(1.0));
				CHECK(sky.y == doctest::Approx(0.0));
				CHECK(sky.z == doctest::Approx(1.0));
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SkyboxPass: Rotation turns the environment and Intensity scales it" * doctest::test_suite(Test::GpuSuite))
		{
			// Rotation 90 degrees: RotateEnvironmentDirection((0, 0, -1), sin 90, cos 90) = (1, 0, 0), the +X face (red); an
			// Intensity of 0.5 halves it.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				SkyboxSetup setup(gpu);
				const glm::dvec3 centre = setup.RenderCentre(MakeEnvironment(0.5f, 1.0f, 0.0f));
				CHECK(centre.x == doctest::Approx(0.5));
				CHECK(centre.y == doctest::Approx(0.0));
				CHECK(centre.z == doctest::Approx(0.0));
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SkyboxPass: an orthographic camera sees the constant view direction in every pixel" * doctest::test_suite(Test::GpuSuite))
		{
			// §8.3: orthographic views use the constant direction (0, 0, -1), so even the corner pixel shows the centre of the -Z
			// face (magenta); a 90-degree perspective camera's corner ray ends near the face's corner, where the filtering blends
			// in the neighbouring faces.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				SkyboxSetup setup(gpu);
				const EnvironmentConstants environment = MakeEnvironment(1.0f, 0.0f, 1.0f);
				const Image orthographic = setup.Render(environment, RenderProjection::Orthographic);
				for (const auto& [x, y] : { std::pair<uint32_t, uint32_t>(0, 0), std::pair<uint32_t, uint32_t>(TargetSize - 1, TargetSize - 1),
						 std::pair<uint32_t, uint32_t>(TargetSize / 2, TargetSize / 2) })
				{
					CAPTURE(x);
					CAPTURE(y);
					const glm::dvec3 pixel = SkyboxSetup::GetPixel(orthographic, x, y);
					CHECK(pixel.x == doctest::Approx(1.0));
					CHECK(pixel.y == doctest::Approx(0.0));
					CHECK(pixel.z == doctest::Approx(1.0));
				}
				const glm::dvec3 perspectiveCorner = SkyboxSetup::GetPixel(setup.Render(environment, RenderProjection::Perspective), 0, 0);
				CHECK(perspectiveCorner.y > 0.01);
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
