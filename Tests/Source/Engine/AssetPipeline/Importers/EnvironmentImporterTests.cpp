#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/EnvironmentImporter.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/EnvironmentBaker.h"
#include "Support/AssetTestFixture.h"
#include "Support/Fuzz.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <string_view>

// EnvironmentImporter (Architecture §7.4, §8.6; Docs/Decisions/0013-m8-decisions.md decision 9): Radiance .hdr through the GPU
// bake, Unsupported without a baker, .exr refused with its hint.

namespace Engine {

	namespace {

		// The committed Studio HDRI (Resources/Environments, Poly Haven CC0, 1k).
		Buffer ReadStudio()
		{
			Result<Buffer> hdr = FileSystem::ReadFile(Test::GetRepositoryRoot() / "Resources/Environments/Studio.hdr");
			REQUIRE_MESSAGE(hdr.has_value(), hdr.error().ToString());
			return std::move(*hdr);
		}

		// Imports `bytes` as project://Assets/Environments/<name> with `settings` and `baker` (null: none).
		Result<ImportResult> ImportEnvironment(Test::AssetTestFixture& fixture, std::string_view name, std::span<const std::byte> bytes,
			const Json& settings, IEnvironmentBaker* baker)
		{
			const std::string relative = std::string("Assets/Environments/") + std::string(name);
			fixture.WriteProjectFile(relative, bytes);
			const Buffer source = fixture.ReadProjectFile(relative);
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0xe0e7000000000001ull);
			metadata.Type = AssetType::Environment;
			metadata.Importer = std::string(EnvironmentImporter::Id);
			metadata.ImporterVersion = EnvironmentImporter::Version;
			metadata.Settings = VariantValue(settings);
			ImportContext context({
				.Vfs = &fixture.GetVfs(),
				.SourcePath = fixture.ProjectPath(relative),
				.SourceBytes = source,
				.Settings = VariantValue(settings),
				.Registry = &fixture.GetRegistry(),
				.Assets = {},
				.EnvironmentBaker = baker,
				.ScriptDiagnostics = nullptr,
			});
			return EnvironmentImporter().Import(context, metadata);
		}

		Json DefaultSettings()
		{
			Json settings = Json::object();
			settings["ClampLuminance"] = false;
			settings["MaxLuminance"] = 1000.0;
			return settings;
		}

		// A flat (uncompressed) Radiance RGBE image of `width` x `height` texels of radiance 1.
		Buffer MakeFlatHdr(uint32_t width, uint32_t height)
		{
			const std::string header = std::format("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {} +X {}\n", height, width);
			const std::span<const std::byte> headerBytes = AsBytes(header);
			Buffer bytes(headerBytes.begin(), headerBytes.end());
			// 1.0 = (128 / 256) * 2^1: mantissa bytes 128, exponent byte 128 + 1.
			constexpr std::array<std::byte, 4> One = { std::byte{ 128 }, std::byte{ 128 }, std::byte{ 128 }, std::byte{ 129 } };
			for (uint32_t index = 0; index < width * height; ++index)
				bytes.insert(bytes.end(), One.begin(), One.end());
			return bytes;
		}

		// A baker that checks the decoded input it receives (the equirectangular shape and finite, non-negative texels) and
		// bakes nothing: the fuzz case's oracle for what the .hdr decoder hands on.
		class InputCheckingBaker final : public IEnvironmentBaker
		{
		public:
			Result<EnvironmentData> Bake(const EnvironmentBakeInput& input) override
			{
				CHECK(input.Width == 2 * input.Height);
				CHECK(input.Height > 0);
				CHECK(input.Texels.size() == static_cast<size_t>(input.Width) * input.Height * 3);
				CHECK(std::ranges::all_of(input.Texels, [](float texel)
				{
					return std::isfinite(texel) && texel >= 0.0f;
				}));
				++m_Bakes;
				return std::unexpected(Error(ErrorCode::Unsupported, "the fuzz case bakes nothing"));
			}

			[[nodiscard]] uint64_t GetBakeCount() const { return m_Bakes; }
		private:
			uint64_t m_Bakes = 0;
		};

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("EnvironmentImporter: claims .hdr and .exr, runs on the main thread and is registered")
		{
			const EnvironmentImporter importer;
			CHECK(importer.CanImport(".hdr"));
			CHECK(importer.CanImport(".EXR"));
			CHECK(importer.RequiresMainThread());
			CHECK(importer.GetMainType() == AssetType::Environment);
			ImporterRegistry registry;
			RegisterBuiltinImporters(registry);
			CHECK(registry.FindForExtension(".hdr") == registry.FindById(EnvironmentImporter::Id));
			Test::AssetTestFixture fixture;
			CHECK(fixture.GetRegistry().FindStruct("EnvironmentImportSettings") != nullptr);
		}

		TEST_CASE("EnvironmentImporter: without a baker the import is Unsupported with the GPU hint")
		{
			Test::AssetTestFixture fixture;
			const Result<ImportResult> imported = ImportEnvironment(fixture, "Studio.hdr", ReadStudio(), DefaultSettings(), nullptr);
			REQUIRE_FALSE(imported.has_value());
			CHECK(imported.error().GetCode() == ErrorCode::Unsupported);
			CHECK(imported.error().ToString().contains("start the editor with a GPU once to bake this environment"));
		}

		TEST_CASE("EnvironmentImporter: .exr is refused with the Poly Haven hint and broken .hdr files fail precisely")
		{
			Test::AssetTestFixture fixture;
			const Result<ImportResult> exr = ImportEnvironment(fixture, "Sky.exr", AsBytes("v/1\x01"), DefaultSettings(), nullptr);
			REQUIRE_FALSE(exr.has_value());
			CHECK(exr.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(exr.error().ToString().contains("download the .hdr variant from Poly Haven"));

			const Result<ImportResult> garbage = ImportEnvironment(fixture, "Broken.hdr", AsBytes("#?RADIANCE\nnot an image"), DefaultSettings(), nullptr);
			REQUIRE_FALSE(garbage.has_value());
			CHECK(garbage.error().GetCode() == ErrorCode::ImportFailed);

			// An image that is not a Radiance HDR (stbi_loadf would decode a PNG as linear floats), and one that is not 2:1.
			const Result<ImportResult> png = ImportEnvironment(fixture, "Renamed.hdr", Test::MakeTestPng(8, 4), DefaultSettings(), nullptr);
			REQUIRE_FALSE(png.has_value());
			CHECK(png.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(png.error().ToString().contains("not a Radiance HDR image"));
			const Result<ImportResult> square = ImportEnvironment(fixture, "Square.hdr", MakeFlatHdr(8, 8), DefaultSettings(), nullptr);
			REQUIRE_FALSE(square.has_value());
			CHECK(square.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(square.error().ToString().contains("8x8"));
			// Wider than 8,192 texels (decision 21, B): refused from the header alone, before any texel is decoded, so a
			// header without pixel data is enough.
			const Result<ImportResult> wide = ImportEnvironment(fixture, "Wide.hdr",
				AsBytes(std::string_view("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 8193 +X 16386\n")), DefaultSettings(), nullptr);
			REQUIRE_FALSE(wide.has_value());
			CHECK(wide.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(wide.error().ToString().contains("16386x8193"));
			CHECK(wide.error().GetHint() == "use the 8k or a smaller variant of the HDRI");

			// Settings outside their ranges are Validation errors of the settings, before the source is decoded.
			Json clamped = DefaultSettings();
			clamped["ClampLuminance"] = true;
			clamped["MaxLuminance"] = 0.5;
			const Result<ImportResult> invalid = ImportEnvironment(fixture, "Clamped.hdr", MakeFlatHdr(8, 4), clamped, nullptr);
			REQUIRE_FALSE(invalid.has_value());
			CHECK(invalid.error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("EnvironmentImporter: 200 seeded mutations of a Radiance .hdr never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			// The seeds: the Gradient.hdr fixture (run-length encoded) and a small flat RGBE image, decoded from memory through
			// stbi_loadf (§8.6 step 1). A decoded image reaches the baker, which checks it; a broken one is ImportFailed.
			Test::AssetTestFixture fixture;
			Result<Buffer> gradient = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Environments/Gradient.hdr"));
			REQUIRE_MESSAGE(gradient.has_value(), gradient.error().ToString());
			const std::array<Buffer, 2> seeds = { std::move(*gradient), MakeFlatHdr(16, 8) };
			const Json settings = DefaultSettings();
			const VfsPath sourcePath = fixture.ProjectPath("Assets/Environments/Fuzz.hdr");
			InputCheckingBaker baker;
			const Test::FuzzRun run = Test::RunFuzzCase(0x4D2, 200, [&fixture, &seeds, &settings, &sourcePath, &baker](Random& random, uint64_t /*index*/)
			{
				Buffer mutated = seeds[static_cast<size_t>(random.RangeInt(0, 1))];
				Test::MutateBytes(mutated, random);
				AssetMetadata metadata;
				metadata.Handle = AssetHandle(0xe0e7000000000002ull);
				metadata.Type = AssetType::Environment;
				metadata.Importer = std::string(EnvironmentImporter::Id);
				metadata.ImporterVersion = EnvironmentImporter::Version;
				metadata.Settings = VariantValue(settings);
				ImportContext context({
					.Vfs = &fixture.GetVfs(),
					.SourcePath = sourcePath,
					.SourceBytes = mutated,
					.Settings = VariantValue(settings),
					.Registry = &fixture.GetRegistry(),
					.Assets = {},
					.EnvironmentBaker = &baker,
					.ScriptDiagnostics = nullptr,
				});
				const uint64_t bakes = baker.GetBakeCount();
				const Result<ImportResult> imported = EnvironmentImporter().Import(context, metadata);
				REQUIRE_FALSE(imported.has_value());
				// The decoded image reached the baker, which refused to bake it.
				if (baker.GetBakeCount() != bakes)
				{
					CHECK(imported.error().GetCode() == ErrorCode::Unsupported);
					return true;
				}
				CHECK(imported.error().GetCode() == ErrorCode::ImportFailed);
				return false;
			});
			if (Test::IsFuzzSample(run.Range))
			{
				CHECK(run.Counts.Rejected > 0);
				CHECK(run.Counts.Accepted > 0);
			}
		}

		TEST_CASE("EnvironmentImporter: bakes and cooks the Studio HDRI deterministically" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Result<Scope<EnvironmentBaker>> baker = EnvironmentBaker::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(baker.has_value(), baker.error().ToString());
				Test::AssetTestFixture fixture;
				const Buffer studio = ReadStudio();
				const Result<ImportResult> first = ImportEnvironment(fixture, "Studio.hdr", studio, DefaultSettings(), baker->get());
				REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
				REQUIRE(first->Artifacts.size() == 1);
				CHECK(first->Artifacts.front().Type == AssetType::Environment);
				const Result<AssetRef<EnvironmentData>> environment = LoadCookedEnvironment(first->Artifacts.front().Cooked);
				REQUIRE_MESSAGE(environment.has_value(), environment.error().ToString());
				CHECK((*environment)->Skybox.FaceSize == 256); // 1k HDRI: min(1024, 1024 / 4)
				CHECK((*environment)->Specular.FaceSize == EnvironmentData::SpecularFaceSize);
				// "Importers: importing twice gives identical hashes" holds for the GPU bake on one device.
				const Result<ImportResult> second = ImportEnvironment(fixture, "Studio.hdr", studio, DefaultSettings(), baker->get());
				REQUIRE(second.has_value());
				CHECK(second->Artifacts.front().Cooked == first->Artifacts.front().Cooked);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("EnvironmentImporter: the generated flat-RGBE Gradient.hdr bakes a sky brighter above than below" * doctest::test_suite(Test::GpuSuite))
		{
			// Tests/Data/Assets/Environments/Gradient.hdr (MakeFormatFixtures.py): 32 x 16 flat RGBE scanlines, a blue sky over a
			// dark brown ground, the Formats.scene environment of Projects/FeatureTest.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Result<Scope<EnvironmentBaker>> baker = EnvironmentBaker::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(baker.has_value(), baker.error().ToString());
				Test::AssetTestFixture fixture;
				Result<Buffer> gradient = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Environments/Gradient.hdr"));
				REQUIRE_MESSAGE(gradient.has_value(), gradient.error().ToString());
				const Result<ImportResult> imported = ImportEnvironment(fixture, "Gradient.hdr", *gradient, DefaultSettings(), baker->get());
				REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
				const Result<AssetRef<EnvironmentData>> environment = LoadCookedEnvironment(imported->Artifacts.front().Cooked);
				REQUIRE_MESSAGE(environment.has_value(), environment.error().ToString());
				CHECK((*environment)->Skybox.FaceSize == 8); // min(1024, 32 / 4)
				// The band 0 coefficient is the mean radiance; the (1, -1) coefficient grows with +Y, the sky's side.
				for (int channel = 0; channel < 3; ++channel)
				{
					CAPTURE(channel);
					CHECK((*environment)->IrradianceSH9[0][channel] > 0.0f);
					CHECK((*environment)->IrradianceSH9[1][channel] > 0.0f);
				}
				CHECK((*environment)->IrradianceSH9[1].z > (*environment)->IrradianceSH9[1].x);
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
