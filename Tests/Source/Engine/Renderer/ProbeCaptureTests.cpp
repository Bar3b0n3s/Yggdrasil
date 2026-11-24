#include "TestsPCH.h"

#include "Engine/Renderer/ProbeCapture.h"

#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/ProbeRelight.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Support/DeviceLossTest.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/RenderReference.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// Probe captures (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 8, stream Q-A): the furnace and
// colour-bleed oracles of an irradiance probe's SH9 projection, against the test support's double-precision reference
// (Test::ProjectIrradianceSH9), the reflection probe's deterministic HDR image, and the GPU capture: a uniform background
// captured as its radiance, each face looking along its cube direction (neither mirrored nor flipped), the scene's SSR left
// out, the release of what each capture used before it returns (World Tree's brief 21), and the capture's recovery from a
// device loss. The bake's byte-identical files are LightingBakeTests.cpp's, the shading's oracles stream Q-B's.

namespace Engine {

	namespace {

		// A capture of `size` texels a face whose texel in direction d has radiance `radiance(d)`.
		template<typename Radiance>
		ProbeCubeCapture MakeCapture(uint32_t size, Radiance radiance)
		{
			ProbeCubeCapture capture;
			capture.FaceSize = size;
			for (uint32_t face = 0; face < 6; ++face)
			{
				for (uint32_t row = 0; row < size; ++row)
				{
					for (uint32_t column = 0; column < size; ++column)
						capture.Faces[face].push_back(glm::vec4(radiance(Test::CubeTexelDirection(face, column, row, size)), 1.0f));
				}
			}
			return capture;
		}

		// The axes of Vulkan's cube faces (EnvironmentData.h): a texel's direction is Major + s Sc + t Tc, (s, t) in [-1, 1].
		struct FaceAxes
		{
			glm::vec3 Major{};
			glm::vec3 Sc{};
			glm::vec3 Tc{};
		};

		constexpr std::array<FaceAxes, 6> CubeFaces = { {
			{ .Major = { 1, 0, 0 }, .Sc = { 0, 0, -1 }, .Tc = { 0, -1, 0 } },
			{ .Major = { -1, 0, 0 }, .Sc = { 0, 0, 1 }, .Tc = { 0, -1, 0 } },
			{ .Major = { 0, 1, 0 }, .Sc = { 1, 0, 0 }, .Tc = { 0, 0, 1 } },
			{ .Major = { 0, -1, 0 }, .Sc = { 1, 0, 0 }, .Tc = { 0, 0, -1 } },
			{ .Major = { 0, 0, 1 }, .Sc = { 1, 0, 0 }, .Tc = { 0, -1, 0 } },
			{ .Major = { 0, 0, -1 }, .Sc = { -1, 0, 0 }, .Tc = { 0, -1, 0 } },
		} };

		// Each face's marker colour: red, green, blue, yellow, magenta, cyan.
		constexpr std::array<glm::vec3, 6> MarkerColors = { {
			{ 1, 0, 0 },
			{ 0, 1, 0 },
			{ 0, 0, 1 },
			{ 1, 1, 0 },
			{ 1, 0, 1 },
			{ 0, 1, 1 },
		} };

		constexpr uint64_t QuadMeshHandle = 0x9B01000000000001ull;
		constexpr uint64_t MirrorMaterialHandle = 0x9B01000000000002ull;
		constexpr uint64_t FirstMarkerMaterialHandle = 0x9B01000000000010ull;

		// The pipelines, the cache and a probe capture over the fixture's device and an in-memory asset manager holding a unit
		// quad (the XY square [-1, 1]^2), one double-sided black material per face, emitting its marker colour 4 times, and a
		// white metal mirror of perceptual roughness 0.
		class ProbeCaptureSetup
		{
		public:
			explicit ProbeCaptureSetup(Test::HeadlessGpuFixture& gpu)
				: m_Device(&gpu.GetDevice()), m_Cache(gpu.GetDevice(), m_Assets)
			{
				auto mesh = CreateRef<MeshData>();
				mesh->Vertices = {
					{ .Position = { -1, -1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 0, 1 } },
					{ .Position = { 1, -1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 1, 1 } },
					{ .Position = { 1, 1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 1, 0 } },
					{ .Position = { -1, 1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 0, 0 } },
				};
				mesh->Indices = { 0, 1, 2, 0, 2, 3 };
				mesh->Bounds = { { -1, -1, 0 }, { 1, 1, 0 } };
				mesh->Submeshes = { { .IndexCount = 6, .Bounds = mesh->Bounds } };
				mesh->Slots = { {} };
				m_Assets.Publish(UUID(QuadMeshHandle), mesh);
				for (size_t face = 0; face < MarkerColors.size(); ++face)
				{
					auto material = CreateRef<MaterialData>();
					material->BaseColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
					material->Emissive = MarkerColors[face];
					material->EmissiveStrength = 4.0f;
					material->DoubleSided = true;
					m_Assets.Publish(UUID(FirstMarkerMaterialHandle + face), material);
				}
				auto mirror = CreateRef<MaterialData>();
				mirror->BaseColor = glm::vec4(1.0f);
				mirror->Metallic = 1.0f;
				mirror->Roughness = 0.0f;
				m_Assets.Publish(UUID(MirrorMaterialHandle), mirror);
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				m_Pipelines = std::move(*pipelines);
				Result<Scope<ProbeCapture>> capture = ProbeCapture::Create(gpu.GetDevice(), *m_Pipelines, m_Cache, m_Assets);
				REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());
				m_Capture = std::move(*capture);
			}

			~ProbeCaptureSetup() { m_Device->WaitForIdle(); }
			ProbeCaptureSetup(const ProbeCaptureSetup&) = delete;
			ProbeCaptureSetup& operator=(const ProbeCaptureSetup&) = delete;

			[[nodiscard]] ProbeCapture& GetCapture() { return *m_Capture; }
		private:
			// Destroyed in reverse: the capture (its renderer), the pipelines, the cache, then the assets (§8.14 item 4).
			GraphicsDevice* m_Device = nullptr; // fixture back-reference; the device outlives every owned GPU object
			Test::InMemoryAssetManager m_Assets;
			GpuResourceCache m_Cache;
			Scope<SceneRendererPipelines> m_Pipelines;
			Scope<ProbeCapture> m_Capture;
		};

		// A snapshot with a camera (whose pose the capture replaces) clearing to `background` without a skybox, and nothing in it.
		RenderSnapshot MakeProbeSnapshot(const glm::vec3& background)
		{
			RenderSnapshot snapshot;
			snapshot.HasCamera = true;
			snapshot.Camera.NearClip = 0.1f;
			snapshot.Camera.FarClip = 100.0f;
			snapshot.Camera.ClearColor = background;
			snapshot.Camera.ClearToSkybox = false;
			snapshot.Post.BloomEnabled = false;
			snapshot.Post.SsaoEnabled = false;
			snapshot.Quality.ShadowMapSize = 256;
			return snapshot;
		}

		// One small marker quad per face, facing the origin from 3 m along the face's major axis, off the face's centre at
		// (s, t) = (0.5625, 0.5625): the centre of texel (12, 12) of a 16-texel face, whose mirror images are texels (3, 12)
		// and (12, 3).
		void AddFaceMarkers(RenderSnapshot& snapshot)
		{
			for (size_t face = 0; face < CubeFaces.size(); ++face)
			{
				const FaceAxes& axes = CubeFaces[face];
				// Columns: the quad's x along Tc, its y along Sc (a right-handed basis, since Tc x Sc is Major), its z along Major.
				glm::mat4 world(1.0f);
				world[0] = glm::vec4(0.4f * axes.Tc, 0.0f);
				world[1] = glm::vec4(0.4f * axes.Sc, 0.0f);
				world[2] = glm::vec4(axes.Major, 0.0f);
				world[3] = glm::vec4(3.0f * (axes.Major + 0.5625f * axes.Sc + 0.5625f * axes.Tc), 1.0f);
				snapshot.Meshes.push_back(MeshDrawItem{
					.World = world, .Mesh = UUID(QuadMeshHandle), .Materials = { UUID(FirstMarkerMaterialHandle + face) }, .Entity = UUID(face + 1) });
			}
		}

		// The capture's texels as bytes, for the loss test's byte comparison.
		std::vector<std::byte> ToBytes(const ProbeCubeCapture& capture)
		{
			std::vector<std::byte> bytes;
			for (const std::vector<glm::vec4>& face : capture.Faces)
			{
				const std::span<const std::byte> texels = std::as_bytes(std::span(face));
				bytes.insert(bytes.end(), texels.begin(), texels.end());
			}
			return bytes;
		}

		Test::ReferenceCube ToReference(const ProbeCubeCapture& capture)
		{
			Test::ReferenceCube cube;
			cube.Size = capture.FaceSize;
			for (uint32_t face = 0; face < 6; ++face)
			{
				for (const glm::vec4& texel : capture.Faces[face])
					cube.Faces[face].push_back(glm::dvec3(texel));
			}
			return cube;
		}

	}

	TEST_SUITE("Renderer")
	{
		// The furnace: a probe inside uniform radiance L sees irradiance pi L from every side, so its coefficients evaluate to L
		// in every direction (EnvironmentData's convention), and a white Lambertian surface lit by it reflects L.
		TEST_CASE("ProbeCapture: a uniform cube projects to its radiance in every direction (the furnace)")
		{
			const glm::vec3 radiance(0.75f, 1.5f, 3.0f);
			const ProbeCubeCapture capture = MakeCapture(16, [&radiance](const glm::dvec3& /*direction*/)
			{
				return glm::vec3(radiance);
			});
			const std::array<glm::vec3, 9> sh = ProjectCubeToIrradianceSH9(capture);
			std::array<glm::dvec3, 9> coefficients{};
			for (size_t k = 0; k < 9; ++k)
				coefficients[k] = glm::dvec3(sh[k]);
			for (const glm::dvec3 normal : { glm::dvec3(1, 0, 0), glm::dvec3(0, -1, 0), glm::dvec3(0, 0, 1), glm::normalize(glm::dvec3(1, 1, -1)) })
			{
				CAPTURE(normal.x);
				const glm::dvec3 value = Test::EvaluateIrradianceSH9(coefficients, normal);
				CHECK(value.x == doctest::Approx(radiance.x).epsilon(1e-4));
				CHECK(value.y == doctest::Approx(radiance.y).epsilon(1e-4));
				CHECK(value.z == doctest::Approx(radiance.z).epsilon(1e-4));
			}
		}

		// Colour bleed: a probe beside a red wall (the +X hemisphere red, the rest black) sees red light from +X and none from
		// -X: its coefficients equal the reference projection, and evaluate to red facing the wall and to black facing away
		// within the SH9 window's ringing.
		TEST_CASE("ProbeCapture: a cube lit red on one side projects to the reference irradiance of the red hemisphere (colour bleed)")
		{
			const ProbeCubeCapture capture = MakeCapture(32, [](const glm::dvec3& direction)
			{
				return direction.x > 0.0 ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f);
			});
			const std::array<glm::vec3, 9> sh = ProjectCubeToIrradianceSH9(capture);
			const std::array<glm::dvec3, 9> reference = Test::ProjectIrradianceSH9(ToReference(capture));
			for (size_t k = 0; k < 9; ++k)
			{
				CAPTURE(k);
				CHECK(sh[k].x == doctest::Approx(reference[k].x).epsilon(1e-5));
				CHECK(sh[k].y == doctest::Approx(reference[k].y).epsilon(1e-5));
				CHECK(sh[k].z == doctest::Approx(reference[k].z).epsilon(1e-5));
			}
			std::array<glm::dvec3, 9> coefficients{};
			for (size_t k = 0; k < 9; ++k)
				coefficients[k] = glm::dvec3(sh[k]);
			CHECK(Test::EvaluateIrradianceSH9(coefficients, glm::dvec3(1, 0, 0)).x > 0.8);
			CHECK(Test::EvaluateIrradianceSH9(coefficients, glm::dvec3(-1, 0, 0)).x < 0.1);
			CHECK(Test::EvaluateIrradianceSH9(coefficients, glm::dvec3(1, 0, 0)).y == doctest::Approx(0.0));
		}

		TEST_CASE("ProbeCapture: a reflection probe's HDR image is the cube resampled, written byte-identically every time")
		{
			const ProbeCubeCapture capture = MakeCapture(8, [](const glm::dvec3& direction)
			{
				return glm::vec3(glm::abs(direction));
			});
			const ProbeEquirectImage image = ResampleCubeToEquirect(capture);
			CHECK(image.Width == 32);
			CHECK(image.Height == 16);
			REQUIRE(image.Texels.size() == 32u * 16u);
			const Buffer first = WriteRadianceHdr(image);
			const Buffer second = WriteRadianceHdr(ResampleCubeToEquirect(capture));
			REQUIRE(first.size() > 32u * 16u * 4u);
			CHECK(first == second);
			const std::string_view header(reinterpret_cast<const char*>(first.data()), 10);
			CHECK(header == "#?RADIANCE");
		}

		// The furnace on the GPU (the acceptance's oracle, decision 8): a capture with nothing around it sees the background
		// everywhere, read back exactly (0.75, 1.5 and 3 are binary16 values), and its projection evaluates to it.
		TEST_CASE("ProbeCapture: a capture in a uniform background is that radiance on every face, and projects to it" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				ProbeCaptureSetup setup(gpu);
				const glm::vec3 radiance(0.75f, 1.5f, 3.0f);
				const Result<ProbeCubeCapture> capture = setup.GetCapture().Capture(MakeProbeSnapshot(radiance), glm::vec3(1.0f, 2.0f, -3.0f), 16);
				REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());
				CHECK(capture->FaceSize == 16);
				for (const std::vector<glm::vec4>& face : capture->Faces)
				{
					REQUIRE(face.size() == 256);
					for (const glm::vec4& texel : face)
						CHECK(texel == glm::vec4(radiance, 1.0f));
				}
				const std::array<glm::vec3, 9> sh = ProjectCubeToIrradianceSH9(*capture);
				std::array<glm::dvec3, 9> coefficients{};
				for (size_t k = 0; k < 9; ++k)
					coefficients[k] = glm::dvec3(sh[k]);
				for (const glm::dvec3 normal : { glm::dvec3(0, 1, 0), glm::dvec3(-1, 0, 0), glm::normalize(glm::dvec3(1, -2, 3)) })
				{
					const glm::dvec3 value = Test::EvaluateIrradianceSH9(coefficients, normal);
					CHECK(value.x == doctest::Approx(radiance.x).epsilon(1e-4));
					CHECK(value.y == doctest::Approx(radiance.y).epsilon(1e-4));
					CHECK(value.z == doctest::Approx(radiance.z).epsilon(1e-4));
				}

				// The face size is a power of two in [16, 512], and the position finite.
				for (const uint32_t size : { 8U, 24U, 1024U })
				{
					CAPTURE(size);
					const Result<ProbeCubeCapture> refused = setup.GetCapture().Capture(MakeProbeSnapshot(radiance), glm::vec3(0.0f), size);
					REQUIRE_FALSE(refused.has_value());
					CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
				}
				const Result<ProbeCubeCapture> nowhere =
					setup.GetCapture().Capture(MakeProbeSnapshot(radiance), glm::vec3(0.0f, std::numeric_limits<float>::infinity(), 0.0f), 16);
				REQUIRE_FALSE(nowhere.has_value());
				CHECK(nowhere.error().GetCode() == ErrorCode::InvalidArgument);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// Each face is the cube's (EnvironmentData.h's face table): a marker off each face's centre shows on its texel, in its
		// face's colour only, never on the texel a mirrored or flipped face would put it at.
		TEST_CASE("ProbeCapture: each face of a capture sees the scene along its cube direction, neither mirrored nor flipped" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				ProbeCaptureSetup setup(gpu);
				RenderSnapshot snapshot = MakeProbeSnapshot(glm::vec3(0.0f));
				AddFaceMarkers(snapshot);
				const Result<ProbeCubeCapture> capture = setup.GetCapture().Capture(snapshot, glm::vec3(0.0f), 16);
				REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());
				const auto texel = [&capture](size_t face, uint32_t column, uint32_t row)
				{
					return glm::vec3(capture->Faces[face][static_cast<size_t>(row) * 16 + column]);
				};
				for (size_t face = 0; face < 6; ++face)
				{
					CAPTURE(face);
					const glm::vec3 marker = texel(face, 12, 12);
					for (glm::length_t channel = 0; channel < 3; ++channel)
					{
						CAPTURE(channel);
						if (MarkerColors[face][channel] > 0.0f)
							CHECK(marker[channel] > 3.0f);
						else
							CHECK(marker[channel] < 0.5f);
					}
					// The mirrored column, the flipped row and the face's centre see the black background.
					for (const glm::vec3 background : { texel(face, 3, 12), texel(face, 12, 3), texel(face, 8, 8) })
						CHECK(glm::max(background.x, glm::max(background.y, background.z)) < 0.5f);
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// W26.2's review: SSR is a view's screen-space effect, which a 90-degree face would trace only within its own image
		// (ProbeCapture.h), so a capture leaves it out: a mirror floor before a red-emitting wall, which SSR reflects in the -Z
		// face's view, captures the same texels with the scene's SsrEnabled on as with it off.
		TEST_CASE("ProbeCapture: a capture leaves the scene's SSR out, so SsrEnabled changes no texel" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				ProbeCaptureSetup setup(gpu);
				RenderSnapshot snapshot = MakeProbeSnapshot(glm::vec3(0.0f, 0.05f, 0.1f));
				// The floor: the quad turned into the XZ plane facing +Y, 12 m square at y = 0. The wall: the quad facing +Z, 6 m
				// wide and 4 m high, standing on the floor at z = -3.
				const glm::mat4 floor(glm::vec4(6.0f, 0.0f, 0.0f, 0.0f), glm::vec4(0.0f, 0.0f, -6.0f, 0.0f), glm::vec4(0.0f, 6.0f, 0.0f, 0.0f),
					glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
				const glm::mat4 wall = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 2.0f, -3.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(3.0f, 2.0f, 1.0f));
				snapshot.Meshes.push_back(MeshDrawItem{ .World = floor, .Mesh = UUID(QuadMeshHandle), .Materials = { UUID(MirrorMaterialHandle) }, .Entity = UUID(1) });
				snapshot.Meshes.push_back(MeshDrawItem{ .World = wall, .Mesh = UUID(QuadMeshHandle), .Materials = { UUID(FirstMarkerMaterialHandle) }, .Entity = UUID(2) });
				const glm::vec3 position(0.0f, 1.0f, 0.0f);
				const Result<ProbeCubeCapture> off = setup.GetCapture().Capture(snapshot, position, 32);
				REQUIRE_MESSAGE(off.has_value(), off.error().ToString());
				snapshot.Post.SsrEnabled = true;
				const Result<ProbeCubeCapture> on = setup.GetCapture().Capture(snapshot, position, 32);
				REQUIRE_MESSAGE(on.has_value(), on.error().ToString());
				CHECK(ToBytes(*on) == ToBytes(*off));
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3; its W42.1 record): a bake is one bounce, as it turns the
		// probes off, so a capture leaves the scene's planar reflections out (MakeCaptureSnapshot clears the list): the mirror
		// floor before the red wall, a planar reflection facing the capture point, captures the same texels with its
		// PlanarReflection as without it.
		TEST_CASE("ProbeCapture: a capture leaves the scene's planar reflections out, so a PlanarReflection changes no texel" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				ProbeCaptureSetup setup(gpu);
				RenderSnapshot snapshot = MakeProbeSnapshot(glm::vec3(0.0f, 0.05f, 0.1f));
				const glm::mat4 floor(glm::vec4(6.0f, 0.0f, 0.0f, 0.0f), glm::vec4(0.0f, 0.0f, -6.0f, 0.0f), glm::vec4(0.0f, 6.0f, 0.0f, 0.0f),
					glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
				const glm::mat4 wall = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 2.0f, -3.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(3.0f, 2.0f, 1.0f));
				snapshot.Meshes.push_back(MeshDrawItem{ .World = floor, .Mesh = UUID(QuadMeshHandle), .Materials = { UUID(MirrorMaterialHandle) }, .Entity = UUID(1) });
				snapshot.Meshes.push_back(MeshDrawItem{ .World = wall, .Mesh = UUID(QuadMeshHandle), .Materials = { UUID(FirstMarkerMaterialHandle) }, .Entity = UUID(2) });
				const glm::vec3 position(0.0f, 1.0f, 0.0f);
				const Result<ProbeCubeCapture> plain = setup.GetCapture().Capture(snapshot, position, 32);
				REQUIRE_MESSAGE(plain.has_value(), plain.error().ToString());
				// The quad's +Z, the floor's +Y: the reflecting side the capture point lies on.
				snapshot.PlanarReflections.push_back(PlanarReflectionItem{ .World = floor, .Normal = glm::vec3(0.0f, 0.0f, 1.0f), .Entity = UUID(1) });
				const Result<ProbeCubeCapture> mirrored = setup.GetCapture().Capture(snapshot, position, 32);
				REQUIRE_MESSAGE(mirrored.has_value(), mirrored.error().ToString());
				CHECK(ToBytes(*mirrored) == ToBytes(*plain));
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 4): a probe bakes the scene's light, which the views fog where they
		// shade it, so a capture leaves the scene's fog out: a dense fog over the skybox and the wall changes no texel.
		TEST_CASE("ProbeCapture: a capture leaves the scene's fog out, so a Fog changes no texel" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				ProbeCaptureSetup setup(gpu);
				RenderSnapshot snapshot = MakeProbeSnapshot(glm::vec3(0.2f, 0.05f, 0.1f));
				const glm::mat4 wall = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 2.0f, -3.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(3.0f, 2.0f, 1.0f));
				snapshot.Meshes.push_back(MeshDrawItem{ .World = wall, .Mesh = UUID(QuadMeshHandle), .Materials = { UUID(FirstMarkerMaterialHandle) }, .Entity = UUID(2) });
				const glm::vec3 position(0.0f, 1.0f, 0.0f);
				const Result<ProbeCubeCapture> clear = setup.GetCapture().Capture(snapshot, position, 16);
				REQUIRE_MESSAGE(clear.has_value(), clear.error().ToString());
				snapshot.Fog = RenderFog{ .Enabled = true, .Color = glm::vec3(0.9f, 0.9f, 0.9f), .Density = 2.0f, .MaxOpacity = 1.0f, .FogSkybox = true };
				const Result<ProbeCubeCapture> fogged = setup.GetCapture().Capture(snapshot, position, 16);
				REQUIRE_MESSAGE(fogged.has_value(), fogged.error().ToString());
				CHECK(ToBytes(*fogged) == ToBytes(*clear));
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// World Tree's brief 21 (its look pass, where a bake of 507 probes left 23,521 native allocations at its end): a capture
		// retires its own submissions before it returns, so what it created and dropped (each face's read-back staging among
		// them) goes with it, not at the frame's garbage collection, and any number of captures between two collections keep
		// the device's allocation count at one capture's.
		TEST_CASE("ProbeCapture: captures release what they used, so many between two collections add no allocation" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				ProbeCaptureSetup setup(gpu);
				RenderSnapshot snapshot = MakeProbeSnapshot(glm::vec3(0.25f));
				AddFaceMarkers(snapshot);
				GraphicsDevice& device = gpu.GetDevice();
				// The first capture creates the renderer's targets and the markers' mirrors; nothing collects after it, or between
				// the captures below.
				const Result<ProbeCubeCapture> first = setup.GetCapture().Capture(snapshot, glm::vec3(0.0f), 16);
				REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
				const uint32_t settled = device.GetMemoryAllocationCount();
				for (uint32_t index = 1; index <= 8; ++index)
				{
					CAPTURE(index);
					const Result<ProbeCubeCapture> capture = setup.GetCapture().Capture(snapshot, glm::vec3(0.25f * static_cast<float>(index), 0.0f, 0.0f), 16);
					REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());
					CHECK(device.GetMemoryAllocationCount() <= settled);
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M18's recreation protocol (decision 29): the capture registers itself for its command list and staging, its renderer
		// is place 2, and a capture after the recovery gives the texels it gave before.
		TEST_CASE("ProbeCapture: a capture after a device loss gives the texels it gave before" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				ProbeCaptureSetup setup(gpu);
				RenderSnapshot snapshot = MakeProbeSnapshot(glm::vec3(0.125f, 0.25f, 0.5f));
				AddFaceMarkers(snapshot);
				ProbeCapture& capture = setup.GetCapture();
				GraphicsDevice& device = gpu.GetDevice();
				Test::DeviceLossTest test;
				test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer", "ProbeCapture" };
				// As often as the capture's renderer takes to settle (its RenderTargetPool keeps a free target for
				// DefaultKeepFrames frames), collecting the garbage after each capture, so the live GPU objects the test compares
				// are those of a settled capture; every capture's texels are compared.
				test.Use = [&device, &capture, &snapshot]() -> Result<std::vector<std::byte>>
				{
					std::vector<std::byte> texels;
					for (uint32_t index = 0; index < 2 * RenderTargetPool::DefaultKeepFrames; ++index)
					{
						ENGINE_TRY_ASSIGN(const ProbeCubeCapture cube, capture.Capture(snapshot, glm::vec3(0.0f), 16));
						device.RunGarbageCollection();
						const std::vector<std::byte> bytes = ToBytes(cube);
						texels.insert(texels.end(), bytes.begin(), bytes.end());
					}
					return texels;
				};
				const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
				REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3): T_ij is the integral of V Y_i Y_j over the sphere. A
		// probe that sees the sky everywhere has the orthonormal basis' Gram matrix, the identity (within the basis constants'
		// six digits and the texels' sampling); one that sees it nowhere has zero, exactly; one that sees the upper hemisphere
		// alone has half of T00 (Y00 is constant) and T01 = Y00 Y1(-1) integrated over it, pi Y00 0.488603 (the y over a hemisphere
		// integrating to pi).
		TEST_CASE("ProbeCapture: an open probe's sky transfer is the identity and a closed one's is zero")
		{
			const auto visible = [](const auto& rule)
			{
				ProbeCubeCapture capture = MakeCapture(16, [](const glm::dvec3&)
				{
					return glm::vec3(0.0f);
				});
				for (uint32_t face = 0; face < 6; ++face)
				{
					for (uint32_t row = 0; row < 16; ++row)
					{
						for (uint32_t column = 0; column < 16; ++column)
							capture.Visibility[face].push_back(rule(Test::CubeTexelDirection(face, column, row, 16)) ? 1.0f : 0.0f);
					}
				}
				return capture;
			};
			const std::array<float, IrradianceTransferCount> open = ProjectCubeToSkyTransfer(visible([](const glm::dvec3&)
			{
				return true;
			}));
			for (size_t i = 0; i < 9; ++i)
			{
				for (size_t j = i; j < 9; ++j)
				{
					CAPTURE(i);
					CAPTURE(j);
					CHECK(open[GetSkyTransferIndex(i, j)] == doctest::Approx(i == j ? 1.0 : 0.0).epsilon(0.01).scale(1.0));
				}
			}
			const std::array<float, IrradianceTransferCount> closed = ProjectCubeToSkyTransfer(visible([](const glm::dvec3&)
			{
				return false;
			}));
			for (const float value : closed)
				CHECK(value == 0.0f);
			const std::array<float, IrradianceTransferCount> upper = ProjectCubeToSkyTransfer(visible([](const glm::dvec3& direction)
			{
				return direction.y > 0.0;
			}));
			CHECK(upper[GetSkyTransferIndex(0, 0)] == doctest::Approx(0.5).epsilon(0.01));
			CHECK(upper[GetSkyTransferIndex(0, 1)] == doctest::Approx(std::numbers::pi * 0.282095 * 0.488603).epsilon(0.01));
		}

		// M45: a relight capture (ProbeCaptureMode::Relight) of MakeRelightCaptureSnapshot's snapshot, the sky dark and the Sun
		// light off, sees the scene's own light alone (the emitting markers, black where the sky is), reads back each face's
		// visibility in the faces' texel order (0 on its marker, 1 where the sky is, never at a mirrored or flipped texel: the
		// depth's columns are mirrored back as the colour's), and the sky's totals, whose solid angle is the sphere's. A plain
		// capture reads neither, and a relight capture of a snapshot without a Sky is refused.
		TEST_CASE("ProbeCapture: a relight capture reads where each face sees the sky, and the sky's totals" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				ProbeCaptureSetup setup(gpu);
				RenderSnapshot snapshot = MakeProbeSnapshot(glm::vec3(0.0f));
				snapshot.Camera.ClearToSkybox = true;
				AddFaceMarkers(snapshot);
				snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Directional,
					.Color = glm::vec3(1.0f),
					.Intensity = 5.0f,
					.Direction = glm::normalize(glm::vec3(0.3f, -0.8f, -0.4f)),
					.LightAngle = 0.53f,
					.Entity = UUID(77) });
				snapshot.Sky.Enabled = true;
				snapshot.Sky.Sun = UUID(77);
				snapshot.Sky.DriveLights = false;
				ResolveSkyLights(snapshot);
				const RenderSnapshot relight = MakeRelightCaptureSnapshot(snapshot);
				CHECK(relight.Sky.Intensity == 0.0f);
				CHECK(relight.Lights[0].Color == glm::vec3(0.0f));
				CHECK(relight.Lights[0].Intensity == 5.0f);

				const Result<ProbeCubeCapture> capture = setup.GetCapture().Capture(relight, glm::vec3(0.0f), 16, ProbeCaptureMode::Relight);
				REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());
				const auto visibility = [&capture](size_t face, uint32_t column, uint32_t row)
				{
					return capture->Visibility[face][static_cast<size_t>(row) * 16 + column];
				};
				for (size_t face = 0; face < 6; ++face)
				{
					CAPTURE(face);
					REQUIRE(capture->Visibility[face].size() == 256);
					CHECK(visibility(face, 12, 12) == 0.0f);
					CHECK(visibility(face, 3, 12) == 1.0f);
					CHECK(visibility(face, 12, 3) == 1.0f);
					CHECK(visibility(face, 8, 8) == 1.0f);
					// The marker's own light, and the dark sky.
					const glm::vec4 marker = capture->Faces[face][12 * 16 + 12];
					CHECK(glm::max(marker.x, glm::max(marker.y, marker.z)) > 3.0f);
					CHECK(capture->Faces[face][8 * 16 + 8] == glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
				}
				REQUIRE(capture->SkyRadianceTotals.has_value());
				CHECK((*capture->SkyRadianceTotals)[9].x == doctest::Approx(4.0 * std::numbers::pi).epsilon(1e-3));
				CHECK((*capture->SkyRadianceTotals)[0].z > 0.0f);

				const Result<ProbeCubeCapture> plain = setup.GetCapture().Capture(snapshot, glm::vec3(0.0f), 16);
				REQUIRE_MESSAGE(plain.has_value(), plain.error().ToString());
				CHECK(plain->Visibility[0].empty());
				CHECK_FALSE(plain->SkyRadianceTotals.has_value());
				// The plain capture sees the lit sky where the relight capture saw black.
				CHECK(plain->Faces[2][8 * 16 + 8].z > 0.0f);

				RenderSnapshot skyless = snapshot;
				skyless.Sky.Enabled = false;
				const Result<ProbeCubeCapture> refused = setup.GetCapture().Capture(skyless, glm::vec3(0.0f), 16, ProbeCaptureMode::Relight);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
