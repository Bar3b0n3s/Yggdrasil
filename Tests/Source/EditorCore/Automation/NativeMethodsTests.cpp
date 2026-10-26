#include "TestsPCH.h"
#include "EditorCore/Automation/NativeMethods.h"

#include "EditorCore/Automation/ProvenanceRecorder.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorPreferences.h"
#include "EditorCore/Native/NativeModuleHost.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/AutomationTestClient.h"
#include "Support/FakeNativeBackends.h"
#include "Support/NativeTestModule.h"
#include "Support/TempDirectory.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
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
				return CallErrorData(method, params)["errorCode"].dump();
			}

			// The error's data: errorCode, detail, the location's pointer and issues.
			[[nodiscard]] Json CallErrorData(std::string_view method, const Json& params)
			{
				const Json response = Fixture.Request(method, params);
				REQUIRE_MESSAGE(response.contains("error"), response.dump());
				return response["error"]["data"];
			}

			// The user's checkbox (the Native panel), not a method.
			void Allow()
			{
				SetPreference(true);
			}

			void SetPreference(bool allowed)
			{
				Fixture.GetEditor().GetNativeModule().QueuePreferenceChange(allowed);
				Fixture.GetEditor().GetNativeModule().Update(Fixture.GetEditor());
			}

			// The preference on, the spinner's source written, the module built and loaded.
			void LoadModule()
			{
				Allow();
				static_cast<void>(Call("native.write", Json{ { "path", "Native/Spinner.cpp" }, { "source", std::string(SpinnerSource) } }));
				REQUIRE(Call("native.build", Json::object())["succeeded"] == Json(true));
				static_cast<void>(Call("native.load", Json::object()));
			}

			// entity.create {name}: the new entity's id.
			[[nodiscard]] std::string CreateEntity(std::string_view name)
			{
				return JsonReader(Call("entity.create", Json{ { "name", std::string(name) } })["entity"]["id"]).ReadString().value_or(std::string());
			}
		};

	}

	TEST_SUITE("EditorCore")
	{
		// Docs/Decisions/0030-m19-contract.md decisions 29 and 32 (N-D): the preference is the user's. While it is off,
		// native.build and native.load are refused, native.status reports it, and no method takes it as a param or changes it.
		TEST_CASE("NativeMethods: native.build and native.load are refused while the preference is off, and no method changes it")
		{
			NativeMethodSetup setup;
			static_cast<void>(setup.Call("native.write", Json{ { "path", "Native/Spinner.cpp" }, { "source", std::string(SpinnerSource) } }));
			CHECK(setup.Call("native.status", Json::object())["allowed"] == Json(false));
			CHECK(setup.CallError("native.build", Json::object()) == "\"PermissionDenied\"");
			CHECK(setup.CallError("native.load", Json::object()) == "\"PermissionDenied\"");
			CHECK(setup.Builds->GetStartCount() == 0);

			// No method's params name the preference, and a settings patch cannot reach it: the .eproj has no such member, so a
			// non-strict editor drops it as an unknown member (a --strict one refuses it), and the project's settings never
			// hold it.
			const Json discovered = setup.Call("rpc.discover", Json::object());
			CHECK_FALSE(discovered.dump().contains("allowNativeCode"));
			CHECK_FALSE(discovered.dump().contains("AllowNativeCode"));
			const Json patched = setup.Fixture.Request("project.setSettings", Json{ { "patch", Json{ { "AllowNativeCode", true } } } });
			CHECK_FALSE(patched.dump().contains("\"AllowNativeCode\":true"));
			CHECK_FALSE(setup.Call("project.getSettings", Json::object()).dump().contains("AllowNativeCode"));
			CHECK(setup.Call("native.status", Json::object())["allowed"] == Json(false));
			const Result<EditorPreferences> stored = ReadEditorPreferences(setup.Fixture.GetEditor().GetVfs());
			REQUIRE(stored.has_value());
			CHECK_FALSE(stored->AllowNativeCode);
		}

		// Decisions 32 and 35 (N-D): native.write writes a source below Native/ as one undoable edit recorded in provenance;
		// native.read returns its exact bytes; paths outside Native/ or of other kinds are refused.
		TEST_CASE("NativeMethods: native.write records provenance and native.read returns the exact source")
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
			// Rewriting the same bytes records nothing; a dry run writes nothing and records no provenance.
			const Json same = setup.Call("native.write", Json{ { "path", "Native/Spinner.cpp" }, { "source", std::string(SpinnerSource) } });
			CHECK(same["created"] == Json(false));
			CHECK(same["undoIndex"] == Json(0));
			const Json dry = setup.Call("native.write", Json{ { "path", "Native/Systems/Dry.h" }, { "source", "#pragma once\n" }, { "dryRun", true } });
			CHECK(dry["created"] == Json(true));
			CHECK(dry["undoIndex"] == Json(0));
			CHECK(setup.CallError("native.read", Json{ { "path", "Native/Systems/Dry.h" } }) == "\"NotFound\"");
			CHECK(provenance->Find("Native/Systems/Dry.h") == nullptr);

			static_cast<void>(setup.Call("edit.undo", Json::object()));
			CHECK(setup.CallError("native.read", Json{ { "path", "Native/Spinner.cpp" } }) == "\"NotFound\"");
		}

		// Decisions 31 and 32 (N-D): with the module built and loaded, native.setComponents is one undoable edit through the
		// module's types, native.getComponents reads the result, native.status lists the module, and in play the systems run
		// and the resources are reported.
		TEST_CASE("NativeMethods: native.setComponents is one undoable edit and native.getComponents and native.status read the module")
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

			// The project action the test module's Move system reads (Support/NativeTestModule.h), so its systems run in play
			// without the error an undefined action logs.
			const Json moveX = Json{ { "Type", "Axis" }, { "Positive", Json::array({ "Key.D" }) }, { "Negative", Json::array({ "Key.A" }) } };
			static_cast<void>(setup.Call("project.setSettings", Json{ { "patch", Json{ { "Input", Json{ { "Actions", Json{ { std::string(Test::NativeTestMoveAction), moveX } } } } } } } }));

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

		// Decision 32 (N-D): an Entity value takes an entity's id, a unique id prefix or its path, which resolve through the
		// context's entity resolver as decision 7 resolves EntityRef values in component data; an id of either case is kept
		// in its canonical lowercase form, and a reference that names no entity is NotFound below /components.
		TEST_CASE("NativeMethods: native.setComponents resolves entity paths, prefixes and ids in Entity values")
		{
			NativeMethodSetup setup;
			setup.LoadModule();
			const std::string target = setup.CreateEntity("Target");
			static_cast<void>(setup.CreateEntity("Mover"));
			std::string upper = target;
			std::ranges::transform(upper, upper.begin(), [](char character)
			{
				return static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
			});
			for (const std::string& reference : { std::string("/Target"), target.substr(0, 8), upper, target })
			{
				CAPTURE(reference);
				const Json set = setup.Call("native.setComponents",
					Json{ { "entity", "/Mover" }, { "components", Json{ { "Native.TestMover", Json{ { "Follow", reference } } } } } });
				CHECK(set["components"]["Native.TestMover"]["Follow"] == Json(target));
			}
			const Json missing = setup.CallErrorData("native.setComponents",
				Json{ { "entity", "/Mover" }, { "components", Json{ { "Native.TestMover", Json{ { "Follow", "/Nobody" } } } } } });
			CHECK(missing["errorCode"] == Json("NotFound"));
			CHECK(missing.dump().contains("/components/Native.TestMover/Follow"));
			CHECK(setup.Call("native.getComponents", Json{ { "entity", "/Mover" } })["components"]["Native.TestMover"]["Follow"] == Json(target));
		}

		// Decisions 27 and 32 (N-D): no prefab override records a native component, so native.setComponents refuses a member of
		// a prefab instance in the edit scene, whose next rebuild would lose the edit; the play scene takes changes for the
		// session only (undo index 0), and a patch that changes nothing records no undo step.
		TEST_CASE("NativeMethods: native.setComponents refuses a prefab instance member, changes the play scene for the session and records no unchanged edit")
		{
			NativeMethodSetup setup;
			setup.LoadModule();
			static_cast<void>(setup.CreateEntity("Spinner"));
			const Json spin = Json{ { "entity", "/Spinner" }, { "components", Json{ { "Native.TestSpinner", Json{ { "DegreesPerSecond", 45 } } } } } };
			Json dry = spin;
			dry["dryRun"] = true;
			CHECK(setup.Call("native.setComponents", dry)["undoIndex"] == Json(0));
			CHECK(setup.Call("native.getComponents", Json{ { "entity", "/Spinner" } })["components"].empty());
			CHECK(setup.Call("native.setComponents", spin)["undoIndex"] != Json(0));
			CHECK(setup.Call("native.setComponents", spin)["undoIndex"] == Json(0));

			static_cast<void>(setup.CreateEntity("Wheel"));
			const Json created = setup.Call("prefab.create", Json{ { "entity", "/Wheel" }, { "path", "Assets/Prefabs/Wheel.prefab" }, { "replaceWithInstance", true } });
			const std::string instance = JsonReader(created["instance"]["path"]).ReadString().value_or(std::string());
			REQUIRE_FALSE(instance.empty());
			const Json member = Json{ { "entity", instance }, { "components", Json{ { "Native.TestSpinner", Json::object() } } } };
			const Json refused = setup.CallErrorData("native.setComponents", member);
			CHECK(refused["errorCode"] == Json("InvalidState"));
			CHECK(refused.dump().contains("/entity"));
			CHECK(setup.Call("native.getComponents", Json{ { "entity", instance } })["components"].empty());

			static_cast<void>(setup.Call("play.start", Json{ { "mode", "Play" }, { "lockstep", true } }));
			Json transient = member;
			transient["target"] = "Play";
			CHECK(setup.Call("native.setComponents", transient)["undoIndex"] == Json(0));
			CHECK(setup.Call("native.getComponents", Json{ { "entity", instance }, { "target", "Play" } })["components"].contains("Native.TestSpinner"));
			static_cast<void>(setup.Call("play.stop", Json::object()));
			CHECK(setup.Call("native.getComponents", Json{ { "entity", instance } })["components"].empty());
		}

		// Decisions 27, 29 and 32 (N-D): a native component lives as its JSON in the scene with or without a module, so once the
		// module is unloaded (the user turned native code off) native.getComponents reads it as written among the
		// unregistered components and native.setComponents is refused; native.write is refused while a build runs.
		TEST_CASE("NativeMethods: without a loaded module native components read as written, and native.write waits for a running build")
		{
			NativeMethodSetup setup;
			setup.LoadModule();
			static_cast<void>(setup.CreateEntity("Spinner"));
			const Json set = setup.Call("native.setComponents",
				Json{ { "entity", "/Spinner" }, { "components", Json{ { "Native.TestSpinner", Json{ { "Turns", 3 } } } } } });
			const Json written = set["components"]["Native.TestSpinner"];

			EditorContext& editor = setup.Fixture.GetEditor();
			REQUIRE(editor.GetNativeModule().StartBuild(editor).has_value());
			CHECK(setup.Call("native.status", Json::object())["building"] == Json(true));
			CHECK(setup.CallError("native.write", Json{ { "path", "Native/Spinner.cpp" }, { "source", "// changed\n" } }) == "\"InvalidState\"");
			// The fake build resolves at its first poll (Support/FakeNativeBackends.h).
			const std::optional<Result<NativeBuildOutcome>> outcome = editor.GetNativeModule().PollBuild(editor);
			REQUIRE(outcome.has_value());
			CHECK(setup.Call("native.write", Json{ { "path", "Native/Spinner.cpp" }, { "source", "// changed\n" } })["created"] == Json(false));

			setup.SetPreference(false);
			const Json status = setup.Call("native.status", Json::object());
			CHECK(status["allowed"] == Json(false));
			CHECK(status["loaded"] == Json(false));
			CHECK(status["components"].empty());
			const Json read = setup.Call("native.getComponents", Json{ { "entity", "/Spinner" } });
			CHECK(read["components"].empty());
			CHECK(read["unregistered"]["Native.TestSpinner"] == written);
			CHECK(setup.CallError("native.setComponents", Json{ { "entity", "/Spinner" }, { "components", Json{ { "Native.TestSpinner", nullptr } } } }) == "\"InvalidState\"");
		}

		// Decision 32: native.write is refused while a build runs, an export's Runtime build too, which compiles the sources
		// its lint read (Scripts/BuildNative.py also fails a build whose sources changed while it ran).
		TEST_CASE("NativeMethods: native.write waits for an export's Runtime build")
		{
#if defined(ENGINE_PLATFORM_WINDOWS)
			NativeMethodSetup setup;
			setup.Allow();
			static_cast<void>(setup.Call("native.write", Json{ { "path", "Native/Spinner.cpp" }, { "source", std::string(SpinnerSource) } }));
			EditorContext& editor = setup.Fixture.GetEditor();
			setup.Builds->SetPending(true);
			const Test::TempDirectory output("NativeRuntimeBuild");
			Result<Scope<INativeBuildJob>> job = editor.GetNativeModule().StartRuntimeBuild(editor, ExportConfiguration::Release, output.GetPath() / "Build");
			REQUIRE_MESSAGE(job.has_value(), job.error().ToString());
			CHECK(editor.GetNativeModule().IsRuntimeBuildRunning());
			CHECK(setup.Call("native.status", Json::object())["building"] == Json(false));
			const Json refused = setup.CallErrorData("native.write", Json{ { "path", "Native/Spinner.cpp" }, { "source", "// changed\n" } });
			CHECK(refused["errorCode"] == Json("InvalidState"));
			CHECK(refused.dump().contains("project.export"));

			// A cancelled job counts until it resolves; a destroyed one no longer counts.
			(*job)->Cancel();
			CHECK((*job)->Poll().has_value());
			CHECK_FALSE(editor.GetNativeModule().IsRuntimeBuildRunning());
			CHECK(setup.Call("native.write", Json{ { "path", "Native/Spinner.cpp" }, { "source", "// changed\n" } })["created"] == Json(false));
			Result<Scope<INativeBuildJob>> abandoned = editor.GetNativeModule().StartRuntimeBuild(editor, ExportConfiguration::Release, output.GetPath() / "Again");
			REQUIRE(abandoned.has_value());
			CHECK(editor.GetNativeModule().IsRuntimeBuildRunning());
			abandoned->reset();
			CHECK_FALSE(editor.GetNativeModule().IsRuntimeBuildRunning());
#endif
		}

		// Decision 32 (N-D; the review of W19.2): native.build stays pending while its build runs, and whatever abandons the
		// request (a disconnect of its client, a cancel of the panel's request, or its discard) cancels the host's build, so
		// native.status no longer reports one and the next native.build starts.
		TEST_CASE("NativeMethods: a disconnect, a cancel or a discard of native.build cancels its build, and the next native.build starts")
		{
			NativeMethodSetup setup;
			setup.Allow();
			static_cast<void>(setup.Call("native.write", Json{ { "path", "Native/Spinner.cpp" }, { "source", std::string(SpinnerSource) } }));
			setup.Builds->SetPending(true);
			AutomationServer& server = setup.Fixture.GetClient().GetServer();

			const ClientId other = server.ConnectInProcess("other");
			server.SubmitInProcess(other, RpcRequest{
											  .Id = Json(1),
											  .IsNotification = false,
											  .Method = "native.build",
											  .Params = Json::object(),
											  .TranscriptLine = std::nullopt,
										  });
			server.Pump();
			server.Pump();
			CHECK(server.TakeInProcessResponses(other).empty());
			CHECK(setup.Call("native.status", Json::object())["building"] == Json(true));
			server.DisconnectInProcess(other);
			CHECK(setup.Builds->GetCancelCount() == 1);
			CHECK(setup.Call("native.status", Json::object())["building"] == Json(false));

			EditorActions actions(setup.Fixture.GetEditor(), server);
			const Result<uint64_t> cancelled = actions.Submit("native.build", Json::object());
			REQUIRE(cancelled.has_value());
			actions.Pump();
			actions.Pump();
			const Result<std::optional<Result<Json>>> pending = actions.TakeResult(*cancelled);
			REQUIRE(pending.has_value());
			CHECK_FALSE(pending->has_value());
			CHECK(setup.Call("native.status", Json::object())["building"] == Json(true));
			REQUIRE(actions.Cancel(*cancelled).has_value());
			CHECK(setup.Builds->GetCancelCount() == 2);
			const Result<std::optional<Result<Json>>> completion = actions.TakeResult(*cancelled);
			REQUIRE(completion.has_value());
			REQUIRE(completion->has_value());
			REQUIRE_FALSE(completion->value().has_value());
			CHECK(completion->value().error().GetCode() == ErrorCode::Cancelled);
			CHECK(setup.Call("native.status", Json::object())["building"] == Json(false));

			const Result<uint64_t> discarded = actions.Submit("native.build", Json::object());
			REQUIRE(discarded.has_value());
			actions.Pump();
			actions.Pump();
			CHECK(setup.Call("native.status", Json::object())["building"] == Json(true));
			REQUIRE(actions.Discard(*discarded).has_value());
			CHECK(setup.Builds->GetCancelCount() == 3);
			CHECK(setup.Call("native.status", Json::object())["building"] == Json(false));

			setup.Builds->SetPending(false);
			CHECK(setup.Call("native.build", Json::object())["succeeded"] == Json(true));
		}

		// Decisions 31 and 32 (N-D): native.load reports a reload whose layout changed with the migration's findings, each
		// naming its entity (summarized in the open scene) and its pointer below the entity.
		TEST_CASE("NativeMethods: native.load reports a changed layout's migration with each finding's entity and pointer")
		{
			NativeMethodSetup setup;
			setup.Allow();
			static_cast<void>(setup.Call("native.write", Json{ { "path", "Native/Spinner.cpp" }, { "source", std::string(SpinnerSource) } }));
			REQUIRE(setup.Call("native.build", Json::object())["succeeded"] == Json(true));
			const Json first = setup.Call("native.load", Json::object());
			CHECK(first["reloaded"] == Json(false));
			CHECK(first["layoutChanged"] == Json(true));
			REQUIRE(first["components"].size() == 2);
			CHECK(first["components"][1]["registryName"] == Json("Native.TestMover"));
			CHECK(first["components"][1]["schema"].is_object());
			REQUIRE(first["resources"].size() == 1);
			CHECK(first["resources"][0]["role"] == Json("Resource"));
			CHECK(first["systems"].size() == 3);

			static_cast<void>(setup.CreateEntity("Mover"));
			static_cast<void>(setup.Call("native.setComponents", Json{ { "entity", "/Mover" }, { "components", Json{ { "Native.TestMover", Json{ { "Speed", 3 } } } } } }));
			setup.Builds->SetModule(Test::FakeNativeModule::Layout2);
			static_cast<void>(setup.Call("native.write", Json{ { "path", "Native/Spinner.cpp" }, { "source", std::string(SpinnerSource) + "// layout 2\n" } }));
			CHECK(setup.Call("native.status", Json::object())["loadedCurrent"] == Json(false));
			REQUIRE(setup.Call("native.build", Json::object())["succeeded"] == Json(true));
			const Json reloaded = setup.Call("native.load", Json::object());
			CHECK(reloaded["reloaded"] == Json(true));
			CHECK(reloaded["layoutChanged"] == Json(true));
			CHECK(reloaded["sceneChanged"] == Json(true));
			CHECK(reloaded["undoCleared"] == Json(true));
			CHECK(reloaded["componentCount"] == Json(1));
			const auto dropped = std::ranges::find_if(reloaded["diagnostics"], [](const Json& diagnostic)
			{
				return diagnostic["code"] == Json("NATIVE_FIELD_DROPPED");
			});
			REQUIRE(dropped != reloaded["diagnostics"].end());
			CHECK((*dropped)["entity"]["path"] == Json("/Mover"));
			CHECK((*dropped)["pointer"] == Json("/Components/Native.TestMover/Follow"));
			CHECK(setup.Call("native.getComponents", Json{ { "entity", "/Mover" } })["components"]["Native.TestMover"] == Json{ { "Speed", 3 } });
		}
	}

}
