#include "TestsPCH.h"

#include "EditorCore/Export/ExecutableResources.h"

#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Export/Exporter.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Graphics/Image.h"
#include "Support/EditorTestFixture.h"
#include "Support/WaitUntil.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace Engine {

	namespace {

		// Writes a PNG of `width` x `height` pixels to the project file `path`.
		void WriteIconPng(EditorContext& editor, std::string_view path, uint32_t width, uint32_t height)
		{
			const Result<Image> image = CreateImage(width, height, nvrhi::Format::RGBA8_UNORM);
			REQUIRE(image.has_value());
			const Result<Buffer> png = EncodePng(*image);
			REQUIRE(png.has_value());
			const Result<VfsPath> file = VfsPath::Create("project", path);
			REQUIRE(file.has_value());
			REQUIRE(editor.WriteProjectFile(*file, std::span<const std::byte>(*png)).has_value());
		}

		void WriteProjectText(EditorContext& editor, std::string_view path, std::string_view text)
		{
			const Result<VfsPath> file = VfsPath::Create("project", path);
			REQUIRE(file.has_value());
			REQUIRE(editor.WriteProjectFile(*file, AsBytes(text)).has_value());
		}

		void CheckRefusedAt(const Result<ExecutableResources>& resources, std::string_view pointer)
		{
			REQUIRE_FALSE(resources.has_value());
			INFO(resources.error().ToString());
			CHECK(resources.error().GetCode() == ErrorCode::Validation);
			CHECK(resources.error().GetLocation().JsonPointer == std::optional<std::string>(std::string(pointer)));
			CHECK_FALSE(resources.error().GetHint().empty());
		}

	}

	TEST_SUITE("EditorCore")
	{
		// ADR 0022 decision 12: the first consumer of Export.Icon, Version and Company (gate 8); M15 patches them into the
		// executable.
		TEST_CASE("ExecutableResources: the export settings' version and company are validated and returned")
		{
			Test::EditorTestFixture fixture("ExecutableResources");
			fixture.CreateAndOpenProject();
			ExportSettings settings;
			settings.Version = "2.3.4";
			settings.Company = "Fixture Studio";
			const auto resources = ResolveExecutableResources(fixture.GetEditor(), settings);
			REQUIRE_MESSAGE(resources.has_value(), resources.error().ToString());
			CHECK(resources->VersionMajor == 2);
			CHECK(resources->VersionMinor == 3);
			CHECK(resources->VersionPatch == 4);
			CHECK(resources->Company == "Fixture Studio");
			CHECK(resources->Icon.empty());

			for (const char* version : { "2.3", "2.3.4.5", "v2.3.4", "2.03.4", "2.3.65536", "-1.0.0", "", "1..0", "1.0.", " 1.0.0", "+1.0.0", "1.0.0x" })
			{
				CAPTURE(version);
				settings.Version = version;
				const auto invalid = ResolveExecutableResources(fixture.GetEditor(), settings);
				REQUIRE_FALSE(invalid.has_value());
				CHECK(invalid.error().GetCode() == ErrorCode::Validation);
				CHECK(invalid.error().ToString().find("/Export/Version") != std::string::npos);
			}

			// The bounds of each part: 0 and 65535 are valid.
			settings.Version = "0.65535.10";
			const auto bounds = ResolveExecutableResources(fixture.GetEditor(), settings);
			REQUIRE_MESSAGE(bounds.has_value(), bounds.error().ToString());
			CHECK(bounds->VersionMajor == 0);
			CHECK(bounds->VersionMinor == 65535);
			CHECK(bounds->VersionPatch == 10);

			settings.Version = "1.0.0";
			settings.Icon = "Assets/Missing.png";
			const auto missingIcon = ResolveExecutableResources(fixture.GetEditor(), settings);
			REQUIRE_FALSE(missingIcon.has_value());
			CHECK(missingIcon.error().GetCode() == ErrorCode::Validation);
			CHECK(missingIcon.error().ToString().find("/Export/Icon") != std::string::npos);
		}

		TEST_CASE("ExecutableResources: an icon must be a project PNG of at least 256 x 256 pixels")
		{
			Test::EditorTestFixture fixture("ExecutableIcon");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			WriteIconPng(editor, "Assets/Icon.png", 256, 300);
			WriteIconPng(editor, "Assets/Narrow.png", 255, 512);
			WriteIconPng(editor, "Assets/Short.png", 512, 128);
			WriteProjectText(editor, "Assets/Text.png", "not a PNG");

			ExportSettings settings;
			settings.Icon = "Assets/Icon.png";
			const auto valid = ResolveExecutableResources(editor, settings);
			REQUIRE_MESSAGE(valid.has_value(), valid.error().ToString());
			CHECK(valid->Icon == "Assets/Icon.png");
			CHECK(valid->VersionMajor == 1);
			CHECK(valid->VersionMinor == 0);
			CHECK(valid->VersionPatch == 0);
			CHECK(valid->Company.empty());

			for (const char* icon : { "Assets/Narrow.png", "Assets/Short.png", "Assets/Text.png", "Assets/Missing.png", "../Outside.png", "/Assets/Icon.png" })
			{
				CAPTURE(icon);
				settings.Icon = icon;
				CheckRefusedAt(ResolveExecutableResources(editor, settings), "/Export/Icon");
			}
		}

		TEST_CASE("ExecutableResources: resolving without an open project is InvalidState")
		{
			Test::EditorTestFixture fixture("ExecutableNoProject");
			const auto resources = ResolveExecutableResources(fixture.GetEditor(), ExportSettings{});
			REQUIRE_FALSE(resources.has_value());
			CHECK(resources.error().GetCode() == ErrorCode::InvalidState);
		}

		// The exporter resolves the resources in its Validate step, before it writes anything (§14.2 step 1).
		TEST_CASE("ExecutableResources: an export with an invalid version or icon fails validation at the setting and writes nothing")
		{
			Test::EditorTestFixture fixture("ExecutableExport");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			WriteIconPng(editor, "Assets/Small.png", 64, 64);
			// The settings accept any "digits.digits.digits"; the executable's version resource holds at most 65535 per part.
			const Result<Json> patch = JsonReader::Parse(R"({"Export":{"Version":"1.0.65536","Icon":"Assets/Small.png"}})");
			REQUIRE(patch.has_value());
			Result<Scope<ProjectSettingsCommand>> command = ProjectSettingsCommand::CreateFromPatch(editor, *patch, "Export labels");
			REQUIRE_MESSAGE(command.has_value(), command.error().ToString());
			REQUIRE(editor.Execute(std::move(*command)).has_value());

			ExportSpecification specification;
			specification.BinaryRoot = fixture.GetProjectRoot();
			Result<Scope<Exporter>> exporter = Exporter::Start(editor, specification);
			REQUIRE_MESSAGE(exporter.has_value(), exporter.error().ToString());
			std::optional<Result<ExportReport>> outcome;
			REQUIRE(Test::WaitUntil([&exporter, &outcome]()
			{
				outcome = (*exporter)->Poll();
				return outcome.has_value();
			}));
			REQUIRE_FALSE(outcome->has_value());
			const Error& error = outcome->error();
			INFO(error.ToString());
			CHECK(error.GetCode() == ErrorCode::Validation);
			std::vector<std::string> pointers;
			for (const ErrorIssue& issue : error.GetIssues())
				pointers.push_back(issue.JsonPointer);
			// The version is checked before the icon, and a project without a start scene also fails at /StartScene.
			CHECK(pointers == std::vector<std::string>{ "/Export/Version", "/StartScene" });
			CHECK((*exporter)->GetPhase() == ExportPhase::Validate);
			std::error_code exists;
			CHECK_FALSE(std::filesystem::exists(fixture.GetProjectRoot() / "Build", exists));

			const Result<Json> version = JsonReader::Parse(R"({"Export":{"Version":"1.0.0"}})");
			REQUIRE(version.has_value());
			Result<Scope<ProjectSettingsCommand>> fixVersion = ProjectSettingsCommand::CreateFromPatch(editor, *version, "Export version");
			REQUIRE(fixVersion.has_value());
			REQUIRE(editor.Execute(std::move(*fixVersion)).has_value());
			Result<Scope<Exporter>> again = Exporter::Start(editor, specification);
			REQUIRE_MESSAGE(again.has_value(), again.error().ToString());
			outcome.reset();
			REQUIRE(Test::WaitUntil([&again, &outcome]()
			{
				outcome = (*again)->Poll();
				return outcome.has_value();
			}));
			REQUIRE_FALSE(outcome->has_value());
			std::vector<std::string> iconPointers;
			for (const ErrorIssue& issue : outcome->error().GetIssues())
				iconPointers.push_back(issue.JsonPointer);
			CHECK(iconPointers == std::vector<std::string>{ "/Export/Icon", "/StartScene" });
		}

		// M15 (Docs/Decisions/0024-m15-contract.md decision 4, stream A): icon images are square, so a non-square Export.Icon
		// is refused rather than padded. The 256 x 300 icon the case above accepts joins its refused list when this lands.
		TEST_CASE("ExecutableResources: a non-square icon fails validation at /Export/Icon" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ExecutableSquareIcon");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			WriteIconPng(editor, "Assets/Tall.png", 256, 300);
			WriteIconPng(editor, "Assets/Square.png", 512, 512);
			ExportSettings settings;
			settings.Icon = "Assets/Tall.png";
			CheckRefusedAt(ResolveExecutableResources(editor, settings), "/Export/Icon");
			settings.Icon = "Assets/Square.png";
			const auto square = ResolveExecutableResources(editor, settings);
			REQUIRE_MESSAGE(square.has_value(), square.error().ToString());
			CHECK(square->Icon == "Assets/Square.png");
		}

		TEST_CASE("ExecutableResources: the resource set holds every icon size reduced from Export.Icon and the version strings" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ExecutableResourceSet");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			WriteIconPng(editor, "Assets/Icon.png", 512, 512);
			ExportSettings settings;
			settings.Icon = "Assets/Icon.png";
			settings.Version = "2.3.4";
			settings.Company = "Fixture Studio";
			const auto resources = ResolveExecutableResources(editor, settings);
			REQUIRE_MESSAGE(resources.has_value(), resources.error().ToString());

			const Result<ExecutableResourceSet> set = BuildExecutableResourceSet(editor, *resources, "Tetromino");
			REQUIRE_MESSAGE(set.has_value(), set.error().ToString());
			REQUIRE(set->Icons.size() == ExecutableIconSizes.size());
			const Result<VfsPath> iconPath = VfsPath::Create("project", "Assets/Icon.png");
			REQUIRE(iconPath.has_value());
			const Result<Buffer> iconBytes = editor.GetVfs().ReadFile(*iconPath);
			REQUIRE(iconBytes.has_value());
			const Result<Image> decoded = DecodePng(*iconBytes);
			REQUIRE(decoded.has_value());
			for (size_t index = 0; index < ExecutableIconSizes.size(); ++index)
			{
				CAPTURE(ExecutableIconSizes[index]);
				CHECK(set->Icons[index].Size == ExecutableIconSizes[index]);
				const Result<Image> expected = DownscaleImage(*decoded, ExecutableIconSizes[index]);
				REQUIRE(expected.has_value());
				const Result<Buffer> png = EncodePng(*expected);
				REQUIRE(png.has_value());
				CHECK(set->Icons[index].Png == *png);
			}
			CHECK(set->Version.Major == 2);
			CHECK(set->Version.Minor == 3);
			CHECK(set->Version.Patch == 4);
			CHECK(set->Version.CompanyName == "Fixture Studio");
			CHECK(set->Version.ProductName == "Tetromino");
#if defined(ENGINE_PLATFORM_WINDOWS)
			CHECK(set->Version.OriginalFilename == "Tetromino.exe");
#else
			CHECK(set->Version.OriginalFilename == "Tetromino");
#endif

			// Without an icon: no images, the same version.
			ExecutableResources unlabelled = *resources;
			unlabelled.Icon.clear();
			const Result<ExecutableResourceSet> plain = BuildExecutableResourceSet(editor, unlabelled, "Tetromino");
			REQUIRE(plain.has_value());
			CHECK(plain->Icons.empty());
			CHECK(plain->Version.CompanyName == "Fixture Studio");
		}
	}

}
