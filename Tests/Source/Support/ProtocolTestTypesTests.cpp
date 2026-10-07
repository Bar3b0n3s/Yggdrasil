#include "TestsPCH.h"

#include "Support/ProtocolTestTypes.h"

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("ProtocolTestTypes: the test registry holds the components and the test structs")
		{
			const Scope<TypeRegistry> types = Test::CreateProtocolTestTypes();
			CHECK(types->IsFrozen());
			CHECK(types->FindComponent("RigidBody") != nullptr);
			CHECK(types->FindStruct<Test::EchoParams>() != nullptr);
			CHECK(types->FindStruct<Test::PendResult>() != nullptr);
			CHECK(types->FindEnum<Test::TestShape>() != nullptr);
		}

		TEST_CASE("ProtocolTestTypes: the test host reports its state, checks availability and records offloads")
		{
			Test::TestHostState state;
			state.Revision = 4;
			Test::TestMethodHost host(state);
			CHECK(host.GetMetaState().Revision == 4);
			CHECK(host.GetOffloadServerTag() == Test::TestOffloadServerTag);

			MethodDescriptor read;
			read.Specification.Name = "test.read";
			read.Specification.AvailableInLauncher = true;
			MethodDescriptor echo;
			echo.Specification.Name = "test.echo";
			CHECK(host.CheckAvailability(echo).has_value());
			state.LauncherState = true;
			CHECK(host.CheckAvailability(read).has_value());
			const Status refused = host.CheckAvailability(echo);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
			CHECK(state.Calls == std::vector<std::string>{ "available test.echo", "available test.read", "available test.echo" });

			// std::format, not std::string(tag) + "...": GCC 14 reports a bogus -Warray-bounds on that concatenation at -O2.
			const std::string fileName = std::format("{}-00000001.json", Test::TestOffloadServerTag);
			const Result<std::string> path = host.WriteOffloadedResult(fileName, "{}");
			REQUIRE(path.has_value());
			CHECK(*path == std::format("/offload/{}", fileName));
			REQUIRE(state.Offloaded.size() == 1);
			CHECK(state.Offloaded[0].first == fileName);
			const RpcRequest request = Test::MakeTestRequest(3, "test.read", Json::object());
			CHECK(request.Id == Json(3));
			CHECK(request.Method == "test.read");
		}

		TEST_CASE("ProtocolTestTypes: the test methods register with their flags")
		{
			const Scope<TypeRegistry> types = Test::CreateProtocolTestTypes();
			MethodRegistry methods(*types);
			Test::RegisterProtocolTestMethods(methods);
			methods.Freeze();
			CHECK(methods.GetMethods().size() == 7);
			const MethodDescriptor* echo = methods.Find("test.echo");
			REQUIRE(echo != nullptr);
			CHECK(echo->Specification.SupportsDryRun);
			CHECK(echo->Specification.AllowedInBatch);
			const MethodDescriptor* read = methods.Find("test.read");
			REQUIRE(read != nullptr);
			CHECK(read->Specification.AvailableInLauncher);
			const MethodDescriptor* pend = methods.Find("test.pend");
			REQUIRE(pend != nullptr);
			CHECK_FALSE(pend->Specification.AllowedInBatch);
		}
	}

}
