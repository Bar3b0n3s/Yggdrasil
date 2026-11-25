#include "EnginePCH.h"
#include "Engine/Renderer/ProbeCapture.h"

#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Graphics/CommandListRecording.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GpuOutOfMemory.h"
#include "Engine/Graphics/GpuRecreation.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/ProbeRelight.h"
#include "Engine/Renderer/SceneRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <numbers>
#include <string>
#include <utility>

// Probe captures (ProbeCapture.h; Docs/Decisions/0037-m26-contract.md decision 8): the GPU capture of a cube of radiance
// through a SceneRenderer, and the CPU steps that turn it into an irradiance probe's SH9 and a reflection probe's Radiance HDR
// image. The CPU steps use IEEE-exact operations, sqrt and DetMath only, in double precision and a fixed order, so they give
// the same bits in every configuration. M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3): the relight capture's
// visibility and sky totals, and the sky transfer's projection.

namespace Engine {

	namespace Utils {

		// The face sizes a capture accepts (ProbeCapture::Capture): powers of two in [16, 512].
		constexpr uint32_t MinProbeFaceSize = 16;
		constexpr uint32_t MaxProbeFaceSize = 512;
		// The largest finite binary16 value: the most SceneColor holds, and what a capture's texels are clamped to.
		constexpr float MaxProbeRadiance = 65504.0f;

		// The axes of Vulkan's cube face selection table, per face in Vulkan's order (+X, -X, +Y, -Y, +Z, -Z): the major axis,
		// and the directions whose components are the face's sc and tc, so that a texel's direction is Major + s Sc + t Tc with
		// (s, t) in [-1, 1] (EnvironmentData.h's convention, as PointShadowMaps' faces).
		struct ProbeFaceAxes
		{
			glm::dvec3 Major{};
			glm::dvec3 Sc{};
			glm::dvec3 Tc{};
		};

		constexpr std::array<ProbeFaceAxes, 6> ProbeFaces = { {
			{ .Major = { 1, 0, 0 }, .Sc = { 0, 0, -1 }, .Tc = { 0, -1, 0 } },
			{ .Major = { -1, 0, 0 }, .Sc = { 0, 0, 1 }, .Tc = { 0, -1, 0 } },
			{ .Major = { 0, 1, 0 }, .Sc = { 1, 0, 0 }, .Tc = { 0, 0, 1 } },
			{ .Major = { 0, -1, 0 }, .Sc = { 1, 0, 0 }, .Tc = { 0, 0, -1 } },
			{ .Major = { 0, 0, 1 }, .Sc = { 1, 0, 0 }, .Tc = { 0, -1, 0 } },
			{ .Major = { 0, 0, -1 }, .Sc = { -1, 0, 0 }, .Tc = { 0, -1, 0 } },
		} };

		// The real SH basis of EnvironmentData.h (the constants the shaders evaluate with), in its order.
		constexpr double ShBand0 = 0.282095;
		constexpr double ShBand1 = 0.488603;
		constexpr double ShBand2Cross = 1.092548;
		constexpr double ShBand2Zonal = 0.315392;
		constexpr double ShBand2Sectoral = 0.546274;
		constexpr std::array<uint32_t, 9> ShBands = { 0, 1, 1, 1, 2, 2, 2, 2, 2 };

		[[nodiscard]] static std::array<double, 9> EvaluateShBasis(const glm::dvec3& direction)
		{
			const double x = direction.x;
			const double y = direction.y;
			const double z = direction.z;
			return {
				ShBand0,
				ShBand1 * y,
				ShBand1 * z,
				ShBand1 * x,
				ShBand2Cross * x * y,
				ShBand2Cross * y * z,
				ShBand2Zonal * (3.0 * z * z - 1.0),
				ShBand2Cross * x * z,
				ShBand2Sectoral * (x * x - y * y),
			};
		}

		// The unit direction of texel (column, row) of `face` of a cube of `size` texels a face (its centre).
		[[nodiscard]] static glm::dvec3 GetTexelDirection(uint32_t face, uint32_t column, uint32_t row, uint32_t size)
		{
			const double s = 2.0 * (static_cast<double>(column) + 0.5) / static_cast<double>(size) - 1.0;
			const double t = 2.0 * (static_cast<double>(row) + 0.5) / static_cast<double>(size) - 1.0;
			const ProbeFaceAxes& axes = ProbeFaces[face];
			const glm::dvec3 direction = axes.Major + s * axes.Sc + t * axes.Tc;
			return direction / std::sqrt(glm::dot(direction, direction));
		}

		// The integral over [0, x] x [0, y] of a cube face at distance 1 as a solid angle.
		[[nodiscard]] static double FaceAreaElement(double x, double y)
		{
			return DetMath::ATan2(x * y, std::sqrt(x * x + y * y + 1.0));
		}

		// The exact solid angle of texel (column, row) of a face of `size` texels.
		[[nodiscard]] static double GetTexelSolidAngle(uint32_t column, uint32_t row, uint32_t size)
		{
			const double scale = 2.0 / static_cast<double>(size);
			const double x0 = static_cast<double>(column) * scale - 1.0;
			const double x1 = (static_cast<double>(column) + 1.0) * scale - 1.0;
			const double y0 = static_cast<double>(row) * scale - 1.0;
			const double y1 = (static_cast<double>(row) + 1.0) * scale - 1.0;
			return FaceAreaElement(x0, y0) - FaceAreaElement(x0, y1) - FaceAreaElement(x1, y0) + FaceAreaElement(x1, y1);
		}

		// The capture's texel at (column, row) of `face`, its indices clamped to the face.
		[[nodiscard]] static glm::dvec3 GetCaptureTexel(const ProbeCubeCapture& capture, uint32_t face, int64_t column, int64_t row)
		{
			const int64_t last = static_cast<int64_t>(capture.FaceSize) - 1;
			const size_t x = static_cast<size_t>(std::clamp<int64_t>(column, 0, last));
			const size_t y = static_cast<size_t>(std::clamp<int64_t>(row, 0, last));
			return glm::dvec3(glm::vec3(capture.Faces[face][y * capture.FaceSize + x]));
		}

		// The bilinear sample of `capture` in the unit direction `direction`: the face Vulkan's selection takes (the largest
		// component, ties to X before Y before Z), sampled within that face and clamped at its edges.
		[[nodiscard]] static glm::dvec3 SampleCapture(const ProbeCubeCapture& capture, const glm::dvec3& direction)
		{
			const glm::dvec3 magnitude = glm::abs(direction);
			uint32_t face = 0;
			double major = 0.0;
			if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z)
			{
				face = direction.x >= 0.0 ? 0 : 1;
				major = magnitude.x;
			}
			else if (magnitude.y >= magnitude.z)
			{
				face = direction.y >= 0.0 ? 2 : 3;
				major = magnitude.y;
			}
			else
			{
				face = direction.z >= 0.0 ? 4 : 5;
				major = magnitude.z;
			}
			const ProbeFaceAxes& axes = ProbeFaces[face];
			const double size = static_cast<double>(capture.FaceSize);
			// (s, t) in [0, 1] per the face table, then texel coordinates whose integers are texel centres.
			const double s = (glm::dot(direction, axes.Sc) / major + 1.0) * 0.5;
			const double t = (glm::dot(direction, axes.Tc) / major + 1.0) * 0.5;
			const double x = s * size - 0.5;
			const double y = t * size - 0.5;
			const double xFloor = std::floor(x);
			const double yFloor = std::floor(y);
			const double fx = x - xFloor;
			const double fy = y - yFloor;
			const int64_t column = static_cast<int64_t>(xFloor);
			const int64_t row = static_cast<int64_t>(yFloor);
			const glm::dvec3 upper = GetCaptureTexel(capture, face, column, row) * (1.0 - fx) + GetCaptureTexel(capture, face, column + 1, row) * fx;
			const glm::dvec3 lower = GetCaptureTexel(capture, face, column, row + 1) * (1.0 - fx) + GetCaptureTexel(capture, face, column + 1, row + 1) * fx;
			return upper * (1.0 - fy) + lower * fy;
		}

		// A radiance value as the capture and the HDR writer keep it: finite and in [0, MaxProbeRadiance] (NaN and negative
		// values 0, which SceneColor's guards make rare).
		[[nodiscard]] static float SanitizeRadiance(float value)
		{
			if (!(value > 0.0f))
				return 0.0f;
			return std::min(value, MaxProbeRadiance);
		}

		// Greg Ward's RGBE encoding of a sanitized texel: the largest channel's mantissa in [128, 256), the others scaled with it.
		[[nodiscard]] static std::array<std::byte, 4> EncodeRgbe(const glm::vec3& texel)
		{
			const float red = SanitizeRadiance(texel.r);
			const float green = SanitizeRadiance(texel.g);
			const float blue = SanitizeRadiance(texel.b);
			const float largest = std::max(red, std::max(green, blue));
			if (largest < 1e-32f)
				return { std::byte{ 0 }, std::byte{ 0 }, std::byte{ 0 }, std::byte{ 0 } };
			int exponent = 0;
			const float mantissa = std::frexp(largest, &exponent);
			const float scale = mantissa * 256.0f / largest;
			const auto channel = [scale](float value)
			{
				return std::byte{ static_cast<uint8_t>(std::min(value * scale, 255.0f)) };
			};
			return { channel(red), channel(green), channel(blue), std::byte{ static_cast<uint8_t>(exponent + 128) } };
		}

		// IEEE 754 binary16 to float, exactly (every binary16 value is a float).
		[[nodiscard]] static float HalfToFloat(uint16_t half)
		{
			const uint32_t exponent = (half >> 10) & 0x1FU;
			const uint32_t mantissa = half & 0x3FFU;
			float magnitude = 0.0f;
			if (exponent == 0)
				magnitude = std::ldexp(static_cast<float>(mantissa), -24);
			else if (exponent == 0x1F)
				magnitude = mantissa == 0 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
			else
				magnitude = std::ldexp(static_cast<float>(mantissa | 0x400U), static_cast<int>(exponent) - 25);
			return (half & 0x8000U) != 0 ? -magnitude : magnitude;
		}

		// One read-back face of SceneColor (RGBA16_FLOAT, rows top first) as the capture's texels. The capture's camera is a
		// rotation whose right axis is -Sc (a camera along Sc would be a reflection, which would flip every triangle's
		// winding), so the rendered column c is the face's column FaceSize - 1 - c; rows are the face's rows (NVRHI's flipped
		// viewport puts NDC +Y on row 0, the face's t = -1). Texels are sanitized (SanitizeRadiance), alpha 1.
		[[nodiscard]] static Result<std::vector<glm::vec4>> ReadFaceTexels(const Image& image, uint32_t faceSize)
		{
			constexpr size_t TexelBytes = 8;
			if (image.Format != nvrhi::Format::RGBA16_FLOAT || image.Width != faceSize || image.Height != faceSize
				|| image.Pixels.size() != static_cast<size_t>(faceSize) * faceSize * TexelBytes)
			{
				return MakeError(ErrorCode::Gpu, "a probe capture's face was read back as {} x {} texels of another format", image.Width, image.Height);
			}
			std::vector<glm::vec4> texels(static_cast<size_t>(faceSize) * faceSize);
			for (uint32_t row = 0; row < faceSize; ++row)
			{
				const std::span<const std::byte> pixels = image.GetRow(row);
				for (uint32_t column = 0; column < faceSize; ++column)
				{
					glm::vec4 texel(1.0f);
					for (glm::length_t channel = 0; channel < 3; ++channel)
					{
						const size_t offset = static_cast<size_t>(column) * TexelBytes + static_cast<size_t>(channel) * 2;
						const uint16_t half = static_cast<uint16_t>(std::to_integer<uint32_t>(pixels[offset]) | (std::to_integer<uint32_t>(pixels[offset + 1]) << 8));
						texel[channel] = SanitizeRadiance(HalfToFloat(half));
					}
					texels[static_cast<size_t>(row) * faceSize + (faceSize - 1 - column)] = texel;
				}
			}
			return texels;
		}

		// M45: one face's sky visibility, read back from its SceneDepth (D32, reverse-Z: the clear leaves 0, the far plane, where
		// no geometry is drawn; the sky pass writes no depth) through a staging texture of that format on `commandList`, which
		// `device` executes and waits for: 1 where the depth is 0, else 0, its columns mirrored back as ReadFaceTexels' are.
		// Graphics/Readback reads color formats only; a depth copy to a buffer is a depth-aspect vkCmdCopyImageToBuffer, four
		// bytes a texel.
		[[nodiscard]] static Result<std::vector<float>> ReadFaceVisibility(GraphicsDevice& device, nvrhi::ICommandList& commandList, nvrhi::ITexture& depth,
			uint32_t faceSize)
		{
			const nvrhi::TextureDesc& source = depth.getDesc();
			if (source.format != nvrhi::Format::D32 || source.width != faceSize || source.height != faceSize)
				return MakeError(ErrorCode::Gpu, "a probe capture's depth is {} x {} texels of another format than D32", source.width, source.height);
			nvrhi::TextureDesc desc;
			desc.width = faceSize;
			desc.height = faceSize;
			desc.format = nvrhi::Format::D32;
			desc.dimension = nvrhi::TextureDimension::Texture2D;
			desc.debugName = "ProbeCapture.Visibility";
			ENGINE_TRY_ASSIGN(const nvrhi::StagingTextureHandle staging, device.CreateStagingTexture(desc, nvrhi::CpuAccessMode::Read));
			CommandListRecording recording(device, commandList);
			commandList.copyTexture(staging, nvrhi::TextureSlice(), &depth, nvrhi::TextureSlice());
			const uint64_t submission = recording.Execute();
			WaitForSubmission(device, submission, "a probe capture's visibility read-back");
			if (device.IsLost())
				return MakeError(ErrorCode::Gpu, "the device was lost while a probe capture's visibility was read back");
			size_t rowPitch = 0;
			nvrhi::IDevice* nvrhiDevice = device.GetNvrhiDevice();
			const void* mapped = nvrhiDevice->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch);
			if (mapped == nullptr)
				return MakeError(ErrorCode::Gpu, "cannot map a probe capture's visibility staging texture");
			if (rowPitch < static_cast<size_t>(faceSize) * sizeof(float))
			{
				nvrhiDevice->unmapStagingTexture(staging);
				return MakeError(ErrorCode::Gpu, "a probe capture's visibility staging texture has a row pitch of {} bytes", rowPitch);
			}
			std::vector<float> visibility(static_cast<size_t>(faceSize) * faceSize);
			const auto* bytes = static_cast<const std::byte*>(mapped);
			for (uint32_t row = 0; row < faceSize; ++row)
			{
				for (uint32_t column = 0; column < faceSize; ++column)
				{
					float value = 0.0f;
					std::memcpy(&value, bytes + static_cast<size_t>(row) * rowPitch + static_cast<size_t>(column) * sizeof(float), sizeof(float));
					visibility[static_cast<size_t>(row) * faceSize + (faceSize - 1 - column)] = value == 0.0f ? 1.0f : 0.0f;
				}
			}
			nvrhiDevice->unmapStagingTexture(staging);
			return visibility;
		}

		// M45: the sky's radiance totals of the capture's renderer (SceneRenderer::GetSkyRadianceTotals), through a CPU-readable
		// buffer on `commandList`, which `device` executes and waits for.
		[[nodiscard]] static Result<std::array<glm::vec4, 10>> ReadSkyRadianceTotals(GraphicsDevice& device, nvrhi::ICommandList& commandList, nvrhi::IBuffer& totals)
		{
			std::array<glm::vec4, 10> values{};
			nvrhi::BufferDesc desc;
			desc.byteSize = sizeof(values);
			desc.cpuAccess = nvrhi::CpuAccessMode::Read;
			desc.initialState = nvrhi::ResourceStates::CopyDest;
			desc.keepInitialState = true;
			desc.debugName = "ProbeCapture.SkyTotals";
			ENGINE_TRY_ASSIGN(const nvrhi::BufferHandle staging, device.CreateBuffer(desc));
			CommandListRecording recording(device, commandList);
			commandList.copyBuffer(staging, 0, &totals, 0, sizeof(values));
			const uint64_t submission = recording.Execute();
			WaitForSubmission(device, submission, "a probe capture's sky read-back");
			if (device.IsLost())
				return MakeError(ErrorCode::Gpu, "the device was lost while a probe capture's sky was read back");
			nvrhi::IDevice* nvrhiDevice = device.GetNvrhiDevice();
			const void* mapped = nvrhiDevice->mapBuffer(staging, nvrhi::CpuAccessMode::Read);
			if (mapped == nullptr)
				return MakeError(ErrorCode::Gpu, "cannot map a probe capture's sky staging buffer");
			std::memcpy(values.data(), mapped, sizeof(values));
			nvrhiDevice->unmapBuffer(staging);
			return values;
		}

		// The capture's view of face `face` from `position`: a rotation looking along the face's major axis with -Tc up and -Sc
		// right (a right-handed basis; ReadFaceTexels mirrors the columns back).
		[[nodiscard]] static glm::mat4 MakeFaceView(uint32_t face, const glm::vec3& position)
		{
			const ProbeFaceAxes& axes = ProbeFaces[face];
			const glm::dvec3 right = -axes.Sc;
			const glm::dvec3 up = -axes.Tc;
			const glm::dvec3 back = -axes.Major;
			const glm::dvec3 origin(position);
			glm::dmat4 view(1.0);
			for (glm::length_t component = 0; component < 3; ++component)
			{
				view[component][0] = right[component];
				view[component][1] = up[component];
				view[component][2] = back[component];
			}
			view[3] = glm::dvec4(-glm::dot(right, origin), -glm::dot(up, origin), -glm::dot(back, origin), 1.0);
			return glm::mat4(view);
		}

		// The reverse-Z perspective of §8.3 with an infinite far plane for a 90-degree square face: f = 1 exactly, so the face's
		// texels are exactly the cube's (ComputeReverseZProjection's tangent would round).
		[[nodiscard]] static glm::mat4 MakeFaceProjection(float nearClip)
		{
			glm::mat4 projection(0.0f);
			projection[0][0] = 1.0f;
			projection[1][1] = 1.0f;
			projection[2][3] = -1.0f;
			projection[3][2] = nearClip;
			return projection;
		}

		// `source` as a capture renders it (ProbeCapture.h): a Lit view at the face size with the default output stage, the
		// probes' indirect term and the planar reflections off, no particles, sprites, tilemaps, 2D lights, UI, texts, overlays,
		// icons or selection, and every temporal input reset (no previous camera or worlds, no jitter, delta time 0). The
		// capture reads SceneColor before the HDR chain, so bloom, exposure, the tonemap, the LUT and FXAA never reach it; of
		// the post settings that write SceneColor before the read, TAA (a view's resolve and jitter) and SSR (a view-dependent screen-space
		// reflection, which a 90-degree face would trace only within its own image and fade at its edges, seaming the cube)
		// are off, and GTAO is kept as the scene sets it (ProbeCapture.h); so is the fog (M32). The camera and its clear colour
		// are set per face by Capture.
		[[nodiscard]] static RenderSnapshot MakeCaptureSnapshot(const RenderSnapshot& source)
		{
			RenderSnapshot snapshot = source;
			snapshot.HasCamera = true;
			snapshot.Camera.ProjectionKind = RenderProjection::Perspective;
			snapshot.Camera.VerticalFov = 90.0f;
			snapshot.Camera.Entity = UUID();
			snapshot.Camera.PreviousViewProjection.reset();
			snapshot.Camera.Jitter = glm::vec2(0.0f);
			snapshot.Camera.DeltaTime = 0.0f;
			for (MeshDrawItem& mesh : snapshot.Meshes)
				mesh.PreviousWorld.reset();
			// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3): StaticSet stays, shared: a capture draws the static
			// scenery too, whose items carry no previous matrix.
			snapshot.Post.AntiAliasing = RenderAntiAliasing::None;
			snapshot.Post.SsrEnabled = false;
			snapshot.Quality.Probes = false;
			// M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 4): a probe bakes the light, which the view's fog then covers
			// where the probe's light is shaded.
			snapshot.Fog = {};
			snapshot.DebugView = RenderDebugView::Lit;
			snapshot.Flags = RenderViewFlags::None;
			snapshot.SelectedEntities.clear();
			snapshot.Annotations = {};
			snapshot.Texts.clear();
			snapshot.DebugDraw = {};
			snapshot.AnnotationDraw = {};
			snapshot.Icons.clear();
			snapshot.Output = {};
			snapshot.Sprites.clear();
			snapshot.Tilemaps.clear();
			snapshot.Lights2D.clear();
			snapshot.Ui = {};
			snapshot.ParticleEmitters.clear();
			snapshot.Particles.clear();
			// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): one bounce, as the probes' term is off: a mirror bakes
			// as the surface its material draws without the component.
			snapshot.PlanarReflections.clear();
			// M47 (Docs/Decisions/0065-m47-water.md decision 3): the sea is out of the bakes, as the fog is.
			snapshot.Waters.clear();
			return snapshot;
		}

		// Sets the capture's capturing flag for its lifetime, so the flag drops however a capture ends (a vk::SystemError thrown
		// out of NVRHI unwinds through it too), as ViewportCapture's.
		class ProbeCapturingScope
		{
		public:
			explicit ProbeCapturingScope(bool& capturing)
				: m_Capturing(&capturing)
			{
				*m_Capturing = true;
			}

			~ProbeCapturingScope()
			{
				*m_Capturing = false;
			}

			ProbeCapturingScope(const ProbeCapturingScope&) = delete;
			ProbeCapturingScope& operator=(const ProbeCapturingScope&) = delete;
		private:
			bool* m_Capturing = nullptr; // the capture's IsCapturing, which outlives the scope
		};

		// The capture's command list. Not an immediate command list: NVRHI's validation allows one open immediate list at a
		// time, and a bake may run while the caller's frame has its own list open (as ViewportCapture's).
		[[nodiscard]] static Result<nvrhi::CommandListHandle> CreateProbeCommandList(GraphicsDevice& device)
		{
			return device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false));
		}

	}

	std::array<glm::vec3, 9> ProjectCubeToIrradianceSH9(const ProbeCubeCapture& capture)
	{
		const uint32_t size = capture.FaceSize;
		for ([[maybe_unused]] const std::vector<glm::vec4>& face : capture.Faces)
			ENGINE_CORE_ASSERT(face.size() == static_cast<size_t>(size) * size, "A probe capture's face holds {} texels, not {}^2", face.size(), size);
		std::array<glm::dvec3, 9> sums{};
		for (uint32_t face = 0; face < 6; ++face)
		{
			for (uint32_t row = 0; row < size; ++row)
			{
				for (uint32_t column = 0; column < size; ++column)
				{
					const glm::dvec3 radiance(glm::vec3(capture.Faces[face][static_cast<size_t>(row) * size + column]));
					const double solidAngle = Utils::GetTexelSolidAngle(column, row, size);
					const std::array<double, 9> basis = Utils::EvaluateShBasis(Utils::GetTexelDirection(face, column, row, size));
					for (size_t index = 0; index < sums.size(); ++index)
						sums[index] += radiance * (basis[index] * solidAngle);
				}
			}
		}
		const std::array<double, 3> factors = GetIrradianceBandFactors();
		std::array<glm::vec3, 9> coefficients{};
		for (size_t index = 0; index < coefficients.size(); ++index)
			coefficients[index] = glm::vec3(sums[index] * factors[Utils::ShBands[index]]);
		return coefficients;
	}

	std::array<float, IrradianceTransferCount> ProjectCubeToSkyTransfer(const ProbeCubeCapture& capture)
	{
		const uint32_t size = capture.FaceSize;
		for ([[maybe_unused]] const std::vector<float>& face : capture.Visibility)
			ENGINE_CORE_ASSERT(face.size() == static_cast<size_t>(size) * size, "A relight capture's face holds {} visibility values, not {}^2", face.size(), size);
		std::array<double, IrradianceTransferCount> sums{};
		for (uint32_t face = 0; face < 6; ++face)
		{
			for (uint32_t row = 0; row < size; ++row)
			{
				for (uint32_t column = 0; column < size; ++column)
				{
					const double visibility = static_cast<double>(capture.Visibility[face][static_cast<size_t>(row) * size + column]);
					if (visibility == 0.0)
						continue;
					const double weight = visibility * Utils::GetTexelSolidAngle(column, row, size);
					const std::array<double, 9> basis = Utils::EvaluateShBasis(Utils::GetTexelDirection(face, column, row, size));
					for (size_t i = 0; i < 9; ++i)
					{
						for (size_t j = i; j < 9; ++j)
							sums[GetSkyTransferIndex(i, j)] += basis[i] * basis[j] * weight;
					}
				}
			}
		}
		std::array<float, IrradianceTransferCount> transfer{};
		for (size_t index = 0; index < transfer.size(); ++index)
			transfer[index] = static_cast<float>(sums[index]);
		return transfer;
	}

	RenderSnapshot MakeRelightCaptureSnapshot(const RenderSnapshot& source)
	{
		RenderSnapshot snapshot = source;
		if (!snapshot.Sky.Enabled)
			return snapshot;
		snapshot.Sky.Intensity = 0.0f;
		for (const std::optional<uint32_t>& light : { snapshot.Sky.SunLight, snapshot.Sky.MoonLight })
		{
			if (light.has_value() && *light < snapshot.Lights.size())
				snapshot.Lights[*light].Color = glm::vec3(0.0f);
		}
		return snapshot;
	}

	ProbeEquirectImage ResampleCubeToEquirect(const ProbeCubeCapture& capture)
	{
		for ([[maybe_unused]] const std::vector<glm::vec4>& face : capture.Faces)
		{
			ENGINE_CORE_ASSERT(capture.FaceSize > 0 && face.size() == static_cast<size_t>(capture.FaceSize) * capture.FaceSize,
				"A probe capture's face holds {} texels, not {}^2", face.size(), capture.FaceSize);
		}
		constexpr double Pi = std::numbers::pi;
		ProbeEquirectImage image;
		image.Width = 4 * capture.FaceSize;
		image.Height = 2 * capture.FaceSize;
		image.Texels.reserve(static_cast<size_t>(image.Width) * image.Height);
		for (uint32_t row = 0; row < image.Height; ++row)
		{
			// EnvironmentData.h's mapping: theta = v pi from +Y, phi = (u - 0.5) 2 pi, the image centre -Z and u = 0.75 +X.
			const double v = (static_cast<double>(row) + 0.5) / static_cast<double>(image.Height);
			const SinCosResult<double> theta = DetMath::SinCos(v * Pi);
			for (uint32_t column = 0; column < image.Width; ++column)
			{
				const double u = (static_cast<double>(column) + 0.5) / static_cast<double>(image.Width);
				const SinCosResult<double> phi = DetMath::SinCos((u - 0.5) * 2.0 * Pi);
				const glm::dvec3 direction(theta.Sin * phi.Sin, theta.Cos, -theta.Sin * phi.Cos);
				image.Texels.push_back(glm::vec3(Utils::SampleCapture(capture, direction)));
			}
		}
		return image;
	}

	Buffer WriteRadianceHdr(const ProbeEquirectImage& image)
	{
		ENGINE_CORE_ASSERT(image.Texels.size() == static_cast<size_t>(image.Width) * image.Height, "An HDR image of {} x {} holds {} texels",
			image.Width, image.Height, image.Texels.size());
		// The header, then flat (uncompressed) RGBE scanlines from the top row: readers (stb_image among them) take a scanline
		// whose first pixel is not the RLE marker (2, 2, < 128) as flat, which a normalized RGBE pixel never is (its largest
		// mantissa is at least 128) and a zero pixel is not either.
		const std::string header = std::format("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {} +X {}\n", image.Height, image.Width);
		Buffer bytes;
		bytes.reserve(header.size() + image.Texels.size() * 4);
		for (const char character : header)
			bytes.push_back(static_cast<std::byte>(character));
		for (const glm::vec3& texel : image.Texels)
		{
			const std::array<std::byte, 4> rgbe = Utils::EncodeRgbe(texel);
			bytes.insert(bytes.end(), rgbe.begin(), rgbe.end());
		}
		return bytes;
	}

	struct ProbeCapture::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		Scope<SceneRenderer> Renderer{};
		Scope<Readback> FaceReadback{};
		nvrhi::CommandListHandle CommandList{};
		uint64_t FrameIndex = 0;
		// A Capture is running, so the trim hook leaves the renderer's targets to it (as ViewportCapture's).
		bool IsCapturing = false;
		// The registration with the device's recreation registry (Create); declared last, so it is dropped first.
		GpuRecreationRegistration Recreation{};
	};

	ProbeCapture::ProbeCapture(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	ProbeCapture::~ProbeCapture() = default;

	Result<Scope<ProbeCapture>> ProbeCapture::Create(GraphicsDevice& device, SceneRendererPipelines& pipelines, GpuResourceCache& cache,
		AssetManager& assets)
	{
		// The renderer starts at the smallest face; each Capture resizes it to its face size.
		ENGINE_TRY_ASSIGN(Scope<SceneRenderer> renderer,
			SceneRenderer::Create(device, pipelines, cache, assets, { .Width = Utils::MinProbeFaceSize, .Height = Utils::MinProbeFaceSize }));
		ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle commandList, Utils::CreateProbeCommandList(device));

		Scope<ProbeCapture> capture = CreateScope<ProbeCapture>(ConstructionKey());
		State& state = *capture->m_State;
		state.Device = &device;
		state.Renderer = std::move(renderer);
		state.FaceReadback = CreateScope<Readback>(device);
		state.CommandList = std::move(commandList);

		// M18's recreation protocol (Docs/Decisions/0029-m18-contract.md decisions 26, 27 and 33): the renderer is the placement
		// rule's place 2 and registered itself in SceneRenderer::Create; the capture registers after it, so it is released
		// before the renderer and created again after it. Its Release drops its own GPU objects, the command list and the
		// Readback's (a new Readback holds none until its first read), and its Recreate creates the command list again.
		State* owner = &state;
		GpuRecreationHooks hooks;
		hooks.Name = "ProbeCapture";
		hooks.Release = [owner]()
		{
			owner->CommandList = nullptr;
			owner->FaceReadback = CreateScope<Readback>(*owner->Device);
		};
		hooks.Recreate = [owner]() -> Status
		{
			ENGINE_TRY_ASSIGN(owner->CommandList, Utils::CreateProbeCommandList(*owner->Device));
			return {};
		};
		// M18 (C18.4, decision 43): between captures nothing displays the last one's faces, so a trim another request runs
		// releases the renderer's targets (the next Capture creates them again); inside a capture its own trims leave them.
		hooks.TrimUnused = [owner]()
		{
			if (!owner->IsCapturing)
				owner->Renderer->ReleaseTargets();
		};
		state.Recreation = device.GetRecreation().Register(std::move(hooks));
		return capture;
	}

	Result<ProbeCubeCapture> ProbeCapture::Capture(const RenderSnapshot& snapshot, const glm::vec3& position, uint32_t faceSize, ProbeCaptureMode mode)
	{
		if (faceSize < Utils::MinProbeFaceSize || faceSize > Utils::MaxProbeFaceSize || (faceSize & (faceSize - 1)) != 0)
		{
			return MakeError(ErrorCode::InvalidArgument, "a probe capture's face size is a power of two from {} to {}, got {}", Utils::MinProbeFaceSize,
				Utils::MaxProbeFaceSize, faceSize);
		}
		if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
			return MakeError(ErrorCode::InvalidArgument, "a probe capture's position ({}, {}, {}) is not finite", position.x, position.y, position.z);
		const bool relight = mode == ProbeCaptureMode::Relight;
		if (relight && !snapshot.Sky.Enabled)
			return MakeError(ErrorCode::InvalidArgument, "a relight capture renders a scene under an enabled Sky, and the snapshot has none");
		State& state = *m_State;
		if (state.CommandList == nullptr)
			return MakeError(ErrorCode::Gpu, "the probe capture's command list was not created again after a device loss");

		RenderSnapshot face = Utils::MakeCaptureSnapshot(snapshot);
		face.Camera.Position = position;
		face.Camera.ViewportWidth = faceSize;
		face.Camera.ViewportHeight = faceSize;
		face.Camera.Projection = Utils::MakeFaceProjection(face.Camera.NearClip);

		// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decisions 42 to 45): a capture is one request, as ViewportCapture's: its
		// faces are rendered and read back (each submitted and waited for) inside it, so the trim of its retry runs between
		// recordings; the renderer's own Resize trims and retries inside it.
		ProbeCubeCapture capture;
		capture.FaceSize = faceSize;
		const Utils::ProbeCapturingScope capturing(state.IsCapturing);
		const Status captured = RetryAfterTrim(*state.Device, std::format("a {}-texel probe capture", faceSize),
			[&state, &face, &capture, &position, faceSize, relight]() -> Status
		{
			if (state.Renderer->GetWidth() != faceSize || state.Renderer->GetHeight() != faceSize)
				state.Renderer->ReleaseTargets();
			ENGINE_TRY(state.Renderer->Resize(faceSize, faceSize));
			for (uint32_t index = 0; index < 6; ++index)
			{
				face.Camera.View = Utils::MakeFaceView(index, position);
				ENGINE_CORE_VERIFY(state.FrameIndex < std::numeric_limits<uint64_t>::max(), "probe capture frame identity exhausted");
				face.FrameIndex = ++state.FrameIndex;
				// The list is executed whatever Render reports (an error names a skipped draw; the rest rendered).
				CommandListRecording recording(*state.Device, *state.CommandList);
				const Status rendered = state.Renderer->Render(*state.CommandList, face);
				const uint64_t submission = recording.Execute();
				state.Renderer->OnSubmitted(face.FrameIndex, submission);
				ENGINE_TRY(rendered);
				nvrhi::ITexture* sceneColor = state.Renderer->GetSceneColorTexture();
				if (sceneColor == nullptr)
					return MakeError(ErrorCode::Gpu, "the probe capture's renderer holds no SceneColor after rendering face {}", index);
				ENGINE_TRY_ASSIGN(const Image image, state.FaceReadback->ReadTexture(*sceneColor));
				ENGINE_TRY_ASSIGN(capture.Faces[index], Utils::ReadFaceTexels(image, faceSize));
				if (!relight)
					continue;
				// M45: the face's sky visibility, and after the first face the sky's totals (every face renders the same sky).
				nvrhi::ITexture* sceneDepth = state.Renderer->GetSceneDepthTexture();
				if (sceneDepth == nullptr)
					return MakeError(ErrorCode::Gpu, "the probe capture's renderer holds no SceneDepth after rendering face {}", index);
				ENGINE_TRY_ASSIGN(capture.Visibility[index], Utils::ReadFaceVisibility(*state.Device, *state.CommandList, *sceneDepth, faceSize));
				if (index == 0)
				{
					nvrhi::IBuffer* totals = state.Renderer->GetSkyRadianceTotals();
					if (totals == nullptr)
						return MakeError(ErrorCode::Gpu, "the relight capture's renderer made no sky lighting for its snapshot's Sky");
					ENGINE_TRY_ASSIGN(capture.SkyRadianceTotals, Utils::ReadSkyRadianceTotals(*state.Device, *state.CommandList, *totals));
				}
			}
			return {};
		});
		// World Tree's brief 21 (a bake of 507 probes ended with 23,521 native allocations): each face's read-back waited for
		// its copy, queued after the face's render, so every submission of the capture completed, and what they alone still
		// held (each face's staging texture, what a face's render stopped using) goes now, not at the end of the frame that
		// runs the bake. A bake's thousands of captures thus hold one capture's device allocations, whatever its probe count;
		// a failed capture collects what completed too.
		state.Device->CollectCompletedSubmissions();
		ENGINE_TRY(captured);
		return capture;
	}

}
