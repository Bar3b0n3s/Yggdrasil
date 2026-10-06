#include "TestsPCH.h"

#include "Engine/Automation/Protocol/MethodContext.h"

#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Support/DeathTest.h"
#include "Support/ProtocolTestTypes.h"

namespace Engine {

	namespace {

		// A context base that several hosts share, as Engine/Automation/Methods' will be (ADR 0008 decision 26).
		class SharedTestContext : public MethodContext
		{
		public:
			[[nodiscard]] bool IsHostType(TypeKey key) const override
			{
				return key == TypeKeyOf<SharedTestContext>() || MethodContext::IsHostType(key);
			}
		protected:
			SharedTestContext(TypeKey hostKey, MethodRequest request)
				: MethodContext(hostKey, std::move(request))
			{
			}
		};

		// A host's own context deriving from the shared base.
		class LeafTestContext final : public SharedTestContext
		{
		public:
			explicit LeafTestContext(MethodRequest request)
				: SharedTestContext(TypeKeyOf<LeafTestContext>(), std::move(request))
			{
			}

			[[nodiscard]] Scope<MethodContext> CreateNested(MethodRequest request) const override
			{
				return CreateScope<LeafTestContext>(std::move(request));
			}
		};

	}

	// A request for `method` (registered or not), for contexts that only exercise accessors.
	static MethodRequest MakeAccessorRequest(const MethodRegistry& methods, const MethodDescriptor& method)
	{
		return MethodRequest{
			.Info = { .Client = 1, .ClientName = "test", .Id = Json(1), .Method = method.Specification.Name, .TranscriptLine = std::nullopt },
			.Options = {},
			.Method = &method,
			.Params = Json::object(),
			.Registry = &methods,
			.PhaseMarker = nullptr,
			.NestingDepth = 0,
		};
	}

	// A context over test.echo with `params`, for the accessors below.
	static Scope<Test::TestHostContext> MakeEchoContext(const MethodRegistry& methods, Test::TestHostState& state, Json params)
	{
		const MethodDescriptor* echo = methods.Find("test.echo");
		REQUIRE(echo != nullptr);
		return CreateScope<Test::TestHostContext>(state, MethodRequest{
															 .Info = { .Client = 1, .ClientName = "test", .Id = Json(1), .Method = "test.echo", .TranscriptLine = 7 },
															 .Options = {},
															 .Method = echo,
															 .Params = std::move(params),
															 .Registry = &methods,
															 .PhaseMarker = nullptr,
															 .NestingDepth = 0,
														 });
	}

	ENGINE_DEATH_TEST("Automation/MethodContextReservedErrorDataAsserts")
	{
		Scope<TypeRegistry> types = Test::CreateProtocolTestTypes();
		MethodRegistry methods(*types);
		Test::RegisterProtocolTestMethods(methods);
		methods.Freeze();
		Test::TestHostState state;
		Scope<Test::TestHostContext> context = MakeEchoContext(methods, state, Json::object());
		context->SetErrorData("issues", Json::array());
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("MethodContext: IsHostType accepts the most-derived key, every shared base and MethodContext")
		{
			Scope<TypeRegistry> types = Test::CreateProtocolTestTypes();
			const MethodRegistry methods(*types);
			MethodDescriptor descriptor;
			descriptor.Specification.Name = "test.echo";
			descriptor.Domain = "test";

			const LeafTestContext leaf(MakeAccessorRequest(methods, descriptor));
			CHECK(leaf.GetHostKey() == TypeKeyOf<LeafTestContext>());
			CHECK(leaf.IsHostType(TypeKeyOf<LeafTestContext>()));
			CHECK(leaf.IsHostType(TypeKeyOf<SharedTestContext>()));
			CHECK(leaf.IsHostType(TypeKeyOf<MethodContext>()));
			CHECK_FALSE(leaf.IsHostType(TypeKeyOf<Test::TestHostContext>()));

			Test::TestHostState state;
			const Test::TestHostContext host(state, MakeAccessorRequest(methods, descriptor));
			CHECK(host.IsHostType(TypeKeyOf<Test::TestHostContext>()));
			CHECK(host.IsHostType(TypeKeyOf<MethodContext>()));
			CHECK_FALSE(host.IsHostType(TypeKeyOf<SharedTestContext>()));
			CHECK_FALSE(host.IsHostType(TypeKeyOf<LeafTestContext>()));

			const Scope<MethodContext> nested = leaf.CreateNested(MakeAccessorRequest(methods, descriptor));
			CHECK(nested->IsHostType(TypeKeyOf<SharedTestContext>()));
		}

		TEST_CASE("MethodContext: accessors report the request" * doctest::skip(true))
		{
			Scope<TypeRegistry> types = Test::CreateProtocolTestTypes();
			MethodRegistry methods(*types);
			Test::RegisterProtocolTestMethods(methods);
			methods.Freeze();
			Test::TestHostState state;
			Scope<Test::TestHostContext> context = MakeEchoContext(methods, state, Json{ { "text", "hi" } });

			CHECK(context->GetHostKey() == TypeKeyOf<Test::TestHostContext>());
			CHECK(context->GetRequest().ClientName == "test");
			CHECK(context->GetRequest().TranscriptLine == 7u);
			CHECK(context->GetMethod().Specification.Name == "test.echo");
			CHECK(&context->GetRegistry() == &methods);
			CHECK_FALSE(context->IsDryRun());
			CHECK(context->GetNestingDepth() == 0);
		}

		TEST_CASE("MethodContext: HasParam tells absent members from given ones" * doctest::skip(true))
		{
			Scope<TypeRegistry> types = Test::CreateProtocolTestTypes();
			MethodRegistry methods(*types);
			Test::RegisterProtocolTestMethods(methods);
			methods.Freeze();
			Test::TestHostState state;
			Scope<Test::TestHostContext> context = MakeEchoContext(methods, state, Json{ { "text", "hi" }, { "count", 1 } });

			CHECK(context->HasParam("text"));
			CHECK(context->HasParam("count")); // given with its default value
			CHECK_FALSE(context->HasParam("shape"));
		}

		TEST_CASE("MethodContext: SetErrorData collects members and rejects reserved names" * doctest::skip(true))
		{
			Scope<TypeRegistry> types = Test::CreateProtocolTestTypes();
			MethodRegistry methods(*types);
			Test::RegisterProtocolTestMethods(methods);
			methods.Freeze();
			Test::TestHostState state;
			Scope<Test::TestHostContext> context = MakeEchoContext(methods, state, Json::object());

			CHECK(context->GetErrorData().is_null());
			context->SetErrorData("failedOp", Json(3));
			context->SetErrorData("failedOp", Json(4));
			context->SetErrorData("currentRevision", Json(12));
			Json data = context->GetErrorData();
			CHECK(data["failedOp"] == Json(4));
			CHECK(data["currentRevision"] == Json(12));
			ENGINE_CHECK_DEATH("Automation/MethodContextReservedErrorDataAsserts", "reserved");
		}

		TEST_CASE("MethodContext: SerializeResult writes the registered result struct" * doctest::skip(true))
		{
			Scope<TypeRegistry> types = Test::CreateProtocolTestTypes();
			MethodRegistry methods(*types);
			Test::RegisterProtocolTestMethods(methods);
			methods.Freeze();
			Test::TestHostState state;
			Scope<Test::TestHostContext> context = MakeEchoContext(methods, state, Json::object());

			Result<Json> json = context->SerializeResult(Test::EchoResult{ .Text = "a", .Count = 2, .Shape = Test::TestShape::Square, .ComponentNames = {} });
			REQUIRE(json.has_value());
			CHECK((*json)["text"] == Json("a"));
			CHECK((*json)["shape"] == Json("Square"));
		}
	}

}
