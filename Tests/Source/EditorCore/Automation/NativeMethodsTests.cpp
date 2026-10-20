#include "TestsPCH.h"
#include "EditorCore/Automation/NativeMethods.h"

#include "EditorCore/Automation/ProvenanceRecorder.h"
#include "EditorCore/EditorPreferences.h"
#include "EditorCore/Native/NativeModuleHost.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/AutomationTestClient.h"
#include "Support/FakeNativeBackends.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace Engine {

	namespace {

		constexpr std::string_view SpinnerSource = "#include \"Engine/Native/NativeApi.h\"\r\n// Spinner\r\n";

		// A method test whose editor builds and loads native modules through the fakes.
		struct NativeMethodSetup
		{
			Ref<Test::FakeNativeBuildBackend> Builds = CreateRef<Test::FakeNativeBuildBackend>();
			Ref<Test::FakeNativeLibraryLoader> Loader = CreateRef<Test::FakeNativeLibraryLoader>();
			Test::AutomationFixture Fixture{ "NativeMethods", true, std::nullopt, false, Test::EditorTestNativeBackends{ .Build = Builds, .Loader = Loader } };

			NativeMethodSetup()
			{
				Builds->SetApiHeader(MakeEditorNativeModuleHostSpecification(nullptr, nullptr).ApiHeader);
			}

			[[nodiscard]] Json Call(std::string_view method, const Json& params)
			{
				Result<Json> result = Fixture.Call(method, params);
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				return std::move(*result);
			}

			[[nodiscard]] std::string CallError(std::string_view method, const Json& params)
			{
				const Json response = Fixture.Request(method, params);
				REQUIRE(response.contains("error"));
				return response["error"]["data"]["errorCode"].dump();
			}

			// The user's checkbox (the Native panel), not a method.
			void Allow()
			{
				Fixture.GetEditor().GetNativeModule().QueuePreferenceChange(true);
				Fixture.GetEditor().GetNativeModule().Update(Fixture.GetEditor());
			}
		};

	}

	TEST_SUITE("EditorCore")
	{
		// Docs/Decisions/0030-m19-contract.md decisions 29 and 32 (N-D): the preference is the user's. While it is off,
		// native.build and native.load are refused, native.status reports it, and no method takes it as a param or changes it.
		TEST_CASE("NativeMethods: native.build and native.load are refused while the preference is off, and no method changes it" * doctest::skip(true))
		{
			NativeMethodSetup setup;
			static_cast<void>(setup.Call("native.write", Json{ { "path", "Native/Spinner.cpp" }, { "source", std::string(SpinnerSource) } }));
			CHECK(setup.Call("native.status", Json::object())["allowed"] == Json(false));
			CHECK(setup.CallError("native.build", Json::object()) == "\"PermissionDenied\"");
			CHECK(setup.CallError("native.load", Json::object()) == "\"PermissionDenied\"");
			CHECK(setup.Builds->GetStartCount() == 0);

			// No method's params name the preference, and a settings patch cannot reach it.
			const Json discovered = setup.Call("rpc.discover", Json::object());
			CHECK_FALSE(discovered.dump().contains("allowNativeCode"));
			CHECK_FALSE(discovered.dump().contains("AllowNativeCode"));
			CHECK(setup.Fixture.Request("project.setSettings", Json{ { "patch", Json{ { "AllowNativeCode", true } } } }).contains("error"));
			CHECK(setup.Call("native.status", Json::object())["allowed"] == Json(false));
			const Result<EditorPreferences> stored = ReadEditorPreferences(setup.Fixture.GetEditor().GetVfs());
			REQUIRE(stored.has_value());
			CHECK_FALSE(stored->AllowNativeCode);
		}

		// Decisions 32 and 35 (N-D): native.write writes a source below Native/ as one undoable edit recorded in provenance;
		// native.read returns its exact bytes; paths outside Native/ or of other kinds are refused.
		TEST_CASE("NativeMethods: native.write records provenance and native.read returns the exact source" * doctest::skip(true))
		{
			NativeMethodSetup setup;
			const Json written = setup.Call("native.write", Json{ { "path", "Native/Spinner.cpp" }, { "source", std::string(SpinnerSource) } });
			CHECK(written["path"] == Json("Native/Spinner.cpp"));
			CHECK(written["created"] == Json(true));
			CHECK(written["undoIndex"] != Json(0));
			CHECK(setup.Call("native.read", Json{ { "path", "Native/Spinner.cpp" } })["source"] == Json(std::string(SpinnerSource)));
			const ProvenanceRecorder* provenance = setup.Fixture.GetEditor().GetProvenance();
			REQUIRE(provenance != nullptr);
			const ProvenanceEntry* entry = provenance->Find("Native/Spinner.cpp");
			REQUIRE(entry != nullptr);
			CHECK(entry->Method == "native.write");
			CHECK(ProvenanceRecorder::IsRecordedPath("Native/Systems/Move.h"));

			for (const std::string_view refused : { "Assets/Spinner.cpp", "Native/Notes.txt", "Native/../Assets/Spinner.cpp", "Spinner.cpp" })
			{
				CAPTURE(refused);
				CHECK(setup.CallError("native.write", Json{ { "path", std::string(refused) }, { "source", "" } }) == "\"InvalidArgument\"");
			}
			static_cast<void>(setup.Call("edit.undo", Json::object()));
			CHECK(setup.CallError("native.read", Json{ { "path", "Native/Spinner.cpp" } }) == "\"NotFound\"");
		}

		// Decisions 31 and 32 (N-D): with the module built and loaded, native.setComponents is one undoable edit through the
		// module's types, native.getComponents reads the result, native.status lists the module, and in play the systems run
		// and the resources are reported.
		TEST_CASE("NativeMethods: native.setComponents is one undoable edit and native.getComponents and native.status read the module" * doctest::skip(true))
		{
			NativeMethodSetup setup;
			setup.Allow();
			static_cast<void>(setup.Call("native.write", Json{ { "path", "Native/Spinner.cpp" }, { "source", std::string(SpinnerSource) } }));
			const Json built = setup.Call("native.build", Json::object());
			CHECK(built["succeeded"] == Json(true));
			const Json loaded = setup.Call("native.load", Json::object());
			CHECK(loaded["reloaded"] == Json(false));
			const Json status = setup.Call("native.status", Json::object());
			CHECK(status["allowed"] == Json(true));
			CHECK(status["loaded"] == Json(true));
			CHECK(status["loadedCurrent"] == Json(true));
			CHECK(status["sources"] == Json::array({ "Native/Spinner.cpp" }));
			REQUIRE(status["components"].size() == 2);
			CHECK(status["components"][0]["registryName"] == Json("Native.TestSpinner"));
			CHECK(status["systems"].size() == 3);
			CHECK(status["playResources"].is_null());

			static_cast<void>(setup.Call("entity.create", Json{ { "name", "Spinner" } }));
			const Json set = setup.Call("native.setComponents",
				Json{ { "entity", "/Spinner" }, { "components", Json{ { "Native.TestSpinner", Json{ { "DegreesPerSecond", 180 } } } } } });
			CHECK(set["undoIndex"] != Json(0));
			const Json expected = Json{ { "DegreesPerSecond", 180 }, { "Axis", Json::array({ 0, 1, 0 }) }, { "Turns", 0 } };
			CHECK(set["components"]["Native.TestSpinner"] == expected);
			CHECK(setup.Call("native.getComponents", Json{ { "entity", "/Spinner" } })["components"]["Native.TestSpinner"] == expected);
			CHECK(setup.CallError("native.setComponents", Json{ { "entity", "/Spinner" }, { "components", Json{ { "Native.Nope", Json::object() } } } }) == "\"NotFound\"");

			static_cast<void>(setup.Call("play.start", Json{ { "mode", "Play" }, { "lockstep", true } }));
			static_cast<void>(setup.Call("play.step", Json{ { "ticks", 2 } }));
			CHECK(setup.Call("native.status", Json::object())["playResources"]["TestCounter"]["Ticks"] == Json(2));
			CHECK(setup.Call("native.getComponents", Json{ { "entity", "/Spinner" }, { "target", "Play" } })["components"]["Native.TestSpinner"]["Turns"] == Json(2));
			CHECK(setup.CallError("native.load", Json::object()) == "\"InvalidState\"");
			static_cast<void>(setup.Call("play.stop", Json::object()));

			static_cast<void>(setup.Call("edit.undo", Json::object()));
			CHECK(setup.Call("native.getComponents", Json{ { "entity", "/Spinner" } })["components"].empty());
		}
	}

}
