#include "TestsPCH.h"

#include "EditorCore/Export/ExecutableResources.h"
#include "Engine/App/ProjectSettingsApplication.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Project/ProjectSerializer.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Support/AutomationTestClient.h"
#include "Support/DefaultFields.h"
#include "Support/EditorTestFixture.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace Engine {

	namespace {

		ProjectSettings LoadSettings(const TypeRegistry& registry, std::string_view relative)
		{
			const Result<std::string> text = Test::ReadTestDataText(relative);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			ProjectLoadReport report;
			Result<ProjectSettings> settings = ProjectSerializer::LoadFromString(*text, registry, ProjectLoadOptions{}, report);
			REQUIRE_MESSAGE(settings.has_value(), settings.error().ToString());
			return std::move(*settings);
		}

		// The project the observation test opens (ADR 0022 decision 12): Tests/Data/SettingsObservation/Project/ holds every
		// file AllSettings.eproj names (Assets/Icon.png, a PNG of at least 256 x 256; the start, build and suite scenes; the
		// suite script; whatever else a consumer needs), written by Tests/Data/Generate/MakeValidationFixtures.py. It is copied
		// into the test's directory together with Tests/Data/Project/AllSettings.eproj, so the run never writes Tests/Data.
		std::filesystem::path CopyObservationProject(Test::EditorTestFixture& fixture)
		{
			const std::filesystem::path source = Test::GetTestDataPath("SettingsObservation/Project");
			const std::filesystem::path target = fixture.GetDirectory().GetPath() / "AllSettings";
			std::error_code error;
			REQUIRE_MESSAGE(std::filesystem::is_directory(source, error), "missing fixture project ", source.generic_string());
			std::filesystem::copy(source, target, std::filesystem::copy_options::recursive, error);
			REQUIRE_MESSAGE(!error, error.message());
			std::filesystem::copy_file(Test::GetTestDataPath("Project/AllSettings.eproj"), target / "AllSettings.eproj", error);
			REQUIRE_MESSAGE(!error, error.message());
			return target;
		}

	}

	TEST_SUITE("EditorCore")
	{
		// Architecture §15.5 "Settings coverage" and §15.6 gate 8 (ADR 0022 decision 12). Every leaf JSON pointer of
		// ProjectSettings must be observed through its production consumer, never by reading the settings struct back; the
		// test ends by comparing the observed pointers with the reflected leaf walk and naming each unobserved one. Stream D
		// completes the consumers below (play session getters, PlayInput, PhysicsWorld's gravity and layers, the extraction's
		// quality, the ScriptEngine and the play controller's behaviour for PauseOnError and BlockPlayOnTypeErrors, the runner,
		// the validator and the exporter) and the final comparison.
		TEST_CASE("ProjectSettings: every AllSettings.eproj value is observed by its consumer" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("SettingsObservation");
			const std::filesystem::path project = CopyObservationProject(fixture);
			Test::AutomationTestClient client(fixture.GetEditor());
			const Result<Json> opened = client.Call("project.open", Json{ { "path", FileSystem::PathToUtf8(project) } });
			REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ProjectSettings settings = LoadSettings(*registry, "Project/AllSettings.eproj");
			std::set<std::string> observed;

			ApplicationSpecification application;
			ApplyGameSettings(application, settings.Name, settings.Window, settings.Simulation);
			if (application.Name == settings.Name)
				observed.insert("/Name");
			if (application.WindowSettings.Title == settings.Window.Title)
				observed.insert("/Window/Title");
			if (application.Graphics.VSync == settings.Window.VSync)
				observed.insert("/Window/VSync");
			if (application.Loop.MaxStepsPerFrame == settings.Simulation.MaxStepsPerFrame)
				observed.insert("/Simulation/MaxStepsPerFrame");

			REQUIRE_FALSE(settings.Testing.Suites.empty());
			const ScriptingSettings suite = ApplyTestSuiteOverrides(settings.Scripting, settings.Testing.Suites[0].Overrides);
			if (suite.MemoryLimitMB == settings.Testing.Suites[0].Overrides.MemoryLimitMB)
				observed.insert("/Testing/Suites/*/Overrides/MemoryLimitMB");

			const auto resources = ResolveExecutableResources(fixture.GetEditor(), settings.Export);
			REQUIRE_MESSAGE(resources.has_value(), resources.error().ToString());
			if (resources->Company == settings.Export.Company)
				observed.insert("/Export/Company");

			CHECK(observed.contains("/Name"));
			CHECK(observed.contains("/Window/Title"));
			CHECK(observed.contains("/Window/VSync"));
			CHECK(observed.contains("/Simulation/MaxStepsPerFrame"));
			CHECK(observed.contains("/Testing/Suites/*/Overrides/MemoryLimitMB"));
			CHECK(observed.contains("/Export/Company"));
			FAIL("contract skeleton: observe every remaining leaf and compare the observed set with the reflected leaf walk");
		}

		// Tests/Data/CoverageGaps/Gate8/AllSettings.eproj leaves exactly one field at its default; Expected.json names it. The
		// walk is gate 8's own (Support/DefaultFields.h), shared with the AllSettings.eproj test.
		TEST_CASE("ProjectSettings: the gate 8 seeded-gap fixture names exactly its default field" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ProjectSettings gap = LoadSettings(*registry, "CoverageGaps/Gate8/AllSettings.eproj");
			const StructInfo* type = registry->FindStruct<ProjectSettings>();
			REQUIRE(type != nullptr);
			const Result<Json> actual = type->ToJson(&gap);
			REQUIRE(actual.has_value());
			std::vector<std::string> uncovered;
			Test::CollectDefaultFields(*type, { &*actual }, type->MakeDefaultJson(), "", uncovered);

			const Result<std::string> expectedText = Test::ReadTestDataText("CoverageGaps/Gate8/Expected.json");
			REQUIRE_MESSAGE(expectedText.has_value(), expectedText.error().ToString());
			const auto expected = JsonReader::Parse(*expectedText);
			REQUIRE(expected.has_value());
			const auto missing = JsonReader(*expected).GetMember("Missing");
			REQUIRE(missing.has_value());
			REQUIRE(missing->GetArraySize() == static_cast<size_t>(1));
			const auto only = missing->GetElement(0);
			REQUIRE(only.has_value());
			REQUIRE(uncovered.size() == 1);
			CHECK(only->ReadString() == uncovered.front());
		}
	}

}
