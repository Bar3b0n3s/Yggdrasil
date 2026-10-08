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
#include <vector>

// The skybox pass (Architecture §8.3 pass 8, §8.6) on synthetic inputs: a cube whose faces have distinct colours drawn into
// a cleared SceneColor. Skeletons of the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 9); stream B implements
// the pass and removes the skips.

namespace Engine {

	namespace {

		constexpr uint32_t TargetSize = 32;

		// A 1x1-per-face RGBA16F cube: face f has the colour Colors[f].
		constexpr std::array<glm::vec3, 6> FaceColors = { glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1), glm::vec3(1, 1, 0),
			glm::vec3(0, 1, 1), glm::vec3(1, 0, 1) };

		nvrhi::TextureHandle CreateFaceColorCube(GraphicsDevice& device)
		{
			nvrhi::TextureDesc desc;
			desc.width = 1;
			desc.height = 1;
			desc.arraySize = 6;
			desc.dimension = nvrhi::TextureDimension::TextureCube;
			desc.format = nvrhi::Format::RGBA16_FLOAT;
			desc.isShaderResource = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = "SkyboxPassTests.Cube";
			std::array<std::array<uint16_t, 4>, 6> texels{};
			std::vector<TextureSubresourceData> subresources;
			for (uint32_t face = 0; face < 6; ++face)
			{
				texels[face] = { Test::DoubleToHalf(FaceColors[face].r), Test::DoubleToHalf(FaceColors[face].g), Test::DoubleToHalf(FaceColors[face].b),
					Test::DoubleToHalf(1.0) };
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
				ViewConstants view{};
				view.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 90.0f, 10.0f, 0.1f, 100.0f, TargetSize, TargetSize);
				view.View = glm::mat4(1.0f);
				view.ViewProjection = view.Projection;
				view.InverseProjection = glm::inverse(view.Projection);
				view.InverseViewProjection = view.InverseProjection;
				view.ViewportSize = glm::vec2(static_cast<float>(TargetSize));
				view.InverseViewportSize = glm::vec2(1.0f / static_cast<float>(TargetSize));
				view.TanHalfFovY = 1.0f;

				Result<nvrhi::CommandListHandle> commandList = m_Device.CreateCommandList();
				REQUIRE(commandList.has_value());
				(*commandList)->open();
				(*commandList)->writeBuffer(m_ViewBuffer, &view, sizeof(view));
				(*commandList)->writeBuffer(m_EnvironmentBuffer, &environment, sizeof(environment));
				(*commandList)->clearTextureFloat(m_Color, nvrhi::AllSubresources, nvrhi::Color(0.0f));
				(*commandList)->clearDepthStencilTexture(m_Depth, nvrhi::AllSubresources, true, 0.0f, false, 0);
				PassBindingCache bindings;
				const Status recorded = m_Pass->Record(**commandList, bindings,
					{ .Framebuffer = m_Framebuffer, .ViewConstants = m_ViewBuffer, .EnvironmentConstants = m_EnvironmentBuffer, .Skybox = m_Cube });
				(*commandList)->close();
				REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
				m_Device.ExecuteCommandList(**commandList);

				Readback readback(m_Device);
				const Result<Image> image = readback.ReadTexture(*m_Color);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				std::array<uint16_t, 4> centre{};
				std::memcpy(centre.data(), image->GetRow(TargetSize / 2).data() + static_cast<size_t>(TargetSize / 2) * sizeof(centre), sizeof(centre));
				return glm::dvec3(Test::HalfToDouble(centre[0]), Test::HalfToDouble(centre[1]), Test::HalfToDouble(centre[2]));
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
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
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
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("SkyboxPass: Rotation turns the environment and Intensity scales it" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
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
	}

}
