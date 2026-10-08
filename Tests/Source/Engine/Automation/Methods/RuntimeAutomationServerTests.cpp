#include "TestsPCH.h"

#include "Engine/Automation/Methods/RuntimeAutomationServer.h"

#include "Engine/Asset/AssetTypeRegistration.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Automation/Methods/RegisterSharedMethods.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Engine/Session/PlaySession.h"
#include "Support/SceneTestFixture.h"

#include <nlohmann/json.hpp>

// The Runtime's automation server (Architecture §13.5 "Runtime subset"). Skipped skeletons of the M7 contract
// (Docs/Decisions/0012-m7-decisions.md decision 12): stream C implements the server and removes the skips.

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("RuntimeAutomationServer: serves the Runtime subset and only it" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			TypeRegistry registry;
			RegisterBuiltinComponents(registry);
			RegisterProjectSettingsTypes(registry);
			RegisterAssetTypes(registry);
			RegisterAutomationSharedTypes(registry);
			RegisterSharedMethodTypes(registry);
			registry.Freeze();

			PlaySessionSpecification specification;
			specification.Registry = &registry;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			EventLog events;
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("user", CreateScope<MemoryMount>()).has_value());
			Result<Scope<RuntimeAutomationServer>> server = RuntimeAutomationServer::Create(registry, events, vfs, **session, { .GameName = "Tiny" });
			REQUIRE_MESSAGE(server.has_value(), server.error().ToString());

			const MethodRegistry& methods = (*server)->GetMethods();
			for (const char* name : { "play.pause", "play.resume", "play.step", "play.state", "play.setTimeScale", "input.inject", "viewport.screenshot" })
			{
				INFO(std::string(name));
				const MethodDescriptor* method = methods.Find(name);
				REQUIRE(method != nullptr);
				CHECK(method->Specification.AvailableInRuntime);
			}
			// Editor-only methods are not served.
			CHECK(methods.Find("play.start") == nullptr);
			CHECK(methods.Find("play.stop") == nullptr);
			CHECK(methods.Find("entity.create") == nullptr);
			CHECK(methods.Find("project.export") == nullptr);
		}

		TEST_CASE("RuntimeAutomationServer: play.step in process advances the Runtime's session" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			TypeRegistry registry;
			RegisterBuiltinComponents(registry);
			RegisterProjectSettingsTypes(registry);
			RegisterAssetTypes(registry);
			RegisterAutomationSharedTypes(registry);
			RegisterSharedMethodTypes(registry);
			registry.Freeze();

			PlaySessionSpecification specification;
			specification.Registry = &registry;
			Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			EventLog events;
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("user", CreateScope<MemoryMount>()).has_value());
			Result<Scope<RuntimeAutomationServer>> server = RuntimeAutomationServer::Create(registry, events, vfs, **session, { .GameName = "Tiny" });
			REQUIRE_MESSAGE(server.has_value(), server.error().ToString());

			const ClientId client = (*server)->ConnectInProcess("test");
			(*server)->SubmitInProcess(client, RpcRequest{ .Id = Json(1), .Method = "play.pause", .Params = Json::object(), .TranscriptLine = std::nullopt });
			(*server)->SubmitInProcess(client, RpcRequest{ .Id = Json(2), .Method = "play.step", .Params = Json{ { "ticks", 5 } }, .TranscriptLine = std::nullopt });
			std::vector<Json> responses;
			for (int pump = 0; pump < 100 && responses.size() < 2; ++pump)
			{
				(*server)->Pump();
				for (Json& response : (*server)->TakeInProcessResponses(client))
					responses.push_back(std::move(response));
			}
			REQUIRE(responses.size() == 2);
			CHECK(responses[1]["result"]["tick"] == Json(5));
			CHECK((*session)->GetTick() == 5);
		}
	}

}
