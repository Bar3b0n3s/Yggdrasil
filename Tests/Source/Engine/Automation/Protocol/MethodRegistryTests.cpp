#include "TestsPCH.h"

#include "Engine/Automation/Protocol/MethodRegistry.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/JsonSchema.h"
#include "Support/ProtocolTestTypes.h"

namespace Engine {

	namespace {

		// The test types and methods, frozen.
		struct RegistrySetup
		{
			Scope<TypeRegistry> Types = Test::CreateProtocolTestTypes();
			Scope<MethodRegistry> Methods;

			RegistrySetup()
			{
				Methods = CreateScope<MethodRegistry>(*Types);
				Test::RegisterProtocolTestMethods(*Methods);
				Methods->Freeze();
			}
		};

	}

	static Json ParseRegistryJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	// The method named `name`, which must be registered.
	static const MethodDescriptor& RequireMethod(const MethodRegistry& methods, std::string_view name)
	{
		const MethodDescriptor* method = methods.Find(name);
		REQUIRE_MESSAGE(method != nullptr, std::string(name));
		return *method;
	}

	namespace {

		// Resolves "engine://Meshes/Cube" (a Mesh) and "Assets/Red.material" (a Material), fails "Assets/Unavailable.material"
		// with the host's own InvalidState, and resolves nothing else; records every call, so a test can see that handles
		// never reach it.
		class FakeAssetReferences final : public IAssetReferenceResolver
		{
		public:
			[[nodiscard]] Result<UUID> ResolveAssetReference(std::string_view reference, std::string_view assetTypeName) override
			{
				Calls.emplace_back(reference);
				std::string_view type;
				UUID handle;
				if (reference == "engine://Meshes/Cube")
				{
					type = "Mesh";
					handle = UUID(0x101);
				}
				else if (reference == "Assets/Red.material")
				{
					type = "Material";
					handle = UUID(0xabcdef);
				}
				else if (reference == "Assets/Unavailable.material")
				{
					return MakeError(ErrorCode::InvalidState, "asset reference '{}' needs an open project", reference);
				}
				else
				{
					return std::unexpected(Error(ErrorCode::NotFound, std::format("no asset '{}'", reference)).WithHint("did you mean 'Assets/Red.material'?"));
				}
				if (!assetTypeName.empty() && assetTypeName != type)
					return MakeError(ErrorCode::InvalidArgument, "'{}' is a {}, not a {}", reference, type, assetTypeName);
				return handle;
			}

			std::vector<std::string> Calls;
		};

	}

	// A request context of the test host for `method` with `options`, as the Dispatcher builds one.
	static Scope<MethodContext> MakeRegistryContext(const MethodRegistry& methods, Test::TestMethodHost& host, std::string_view method,
		RequestOptions options = {})
	{
		return host.CreateContext(MethodRequest{
			.Info = { .Client = 1, .ClientName = "test", .Id = Json(1), .Method = std::string(method), .TranscriptLine = std::nullopt },
			.Options = options,
			.Method = &RequireMethod(methods, method),
			.Params = Json::object(),
			.Registry = &methods,
			.PhaseMarker = nullptr,
			.NestingDepth = 0,
		});
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("MethodRegistry: registered methods are found by name and listed in name order")
		{
			RegistrySetup setup;
			const MethodDescriptor* echo = setup.Methods->Find("test.echo");
			REQUIRE(echo != nullptr);
			CHECK(echo->Domain == "test");
			CHECK(echo->Params == setup.Types->FindStruct("TestEchoParams"));
			CHECK(echo->Result == setup.Types->FindStruct("TestEchoResult"));
			CHECK_FALSE(echo->Pending);
			CHECK(echo->Specification.Mutates);
			CHECK(RequireMethod(*setup.Methods, "test.pend").Pending);
			CHECK(setup.Methods->Find("test.Echo") == nullptr);

			std::vector<std::string> names;
			for (const MethodDescriptor* method : setup.Methods->GetMethods())
				names.push_back(method->Specification.Name);
			CHECK(names == std::vector<std::string>{ "test.echo", "test.fail", "test.large", "test.pend", "test.read", "test.systemError", "test.throw" });
			CHECK(setup.Methods->GetDomains() == std::vector<std::string>{ "test" });
			const std::vector<std::string> suggestions = setup.Methods->SuggestMethodNames("test.ecko");
			REQUIRE_FALSE(suggestions.empty());
			CHECK(suggestions.front() == "test.echo");
		}

		TEST_CASE("MethodRegistry: PrepareParams takes out reserved members and checks required ones")
		{
			RegistrySetup setup;
			const MethodDescriptor& echo = RequireMethod(*setup.Methods, "test.echo");
			const Result<PreparedParams> prepared = setup.Methods->PrepareParams(echo,
				ParseRegistryJson(R"({"text":"hi","dryRun":true,"ifRevision":12})"));
			REQUIRE(prepared.has_value());
			CHECK(prepared->Params == ParseRegistryJson(R"({"text":"hi"})"));
			CHECK(prepared->Options.DryRun);
			CHECK(prepared->Options.IfRevision == 12u);

			const Result<PreparedParams> missing = setup.Methods->PrepareParams(echo, Json::object());
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE(missing.error().GetIssues().size() == 1);
			CHECK(missing.error().GetIssues()[0].JsonPointer == "/text");

			CHECK(setup.Methods->PrepareParams(echo, Json()).has_value() == false); // null reads as {}: text is still missing
			CHECK(setup.Methods->PrepareParams(echo, ParseRegistryJson("[1]")).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(setup.Methods->PrepareParams(echo, ParseRegistryJson(R"({"text":"a","dryRun":"yes"})")).error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("MethodRegistry: dryRun on a method without supportsDryRun is Unsupported and every method accepts ifRevision")
		{
			RegistrySetup setup;
			const MethodDescriptor& read = RequireMethod(*setup.Methods, "test.read");
			const Result<PreparedParams> dryRun = setup.Methods->PrepareParams(read, ParseRegistryJson(R"({"dryRun":true})"));
			REQUIRE_FALSE(dryRun.has_value());
			CHECK(dryRun.error().GetCode() == ErrorCode::Unsupported);
			// A read takes ifRevision as a precondition (§13.4), like a call that changes something only when asked.
			const Result<PreparedParams> revision = setup.Methods->PrepareParams(read, ParseRegistryJson(R"({"ifRevision":3})"));
			REQUIRE_MESSAGE(revision.has_value(), revision.error().ToString());
			CHECK(revision->Options.IfRevision == std::optional<uint64_t>(3));
			const Result<PreparedParams> negative = setup.Methods->PrepareParams(read, ParseRegistryJson(R"({"ifRevision":-1})"));
			REQUIRE_FALSE(negative.has_value());
			CHECK(negative.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(setup.Methods->PrepareParams(read, ParseRegistryJson(R"({"dryRun":false})")).has_value());
		}

		TEST_CASE("MethodRegistry: enum values are parsed case-insensitively and echoed canonically")
		{
			RegistrySetup setup;
			const MethodDescriptor& echo = RequireMethod(*setup.Methods, "test.echo");
			const Result<PreparedParams> prepared = setup.Methods->PrepareParams(echo,
				ParseRegistryJson(R"({"text":"a","shape":"sQuArE","components":{"RigidBody":{"Type":"kinematic"}}})"));
			REQUIRE(prepared.has_value());
			Json params = prepared->Params;
			CHECK(params["shape"] == Json("Square"));
			CHECK(params["components"]["RigidBody"]["Type"] == Json("Kinematic"));
		}

		TEST_CASE("MethodRegistry: Invoke resolves asset references in params through the context's resolver")
		{
			RegistrySetup setup;
			FakeAssetReferences assets;
			Test::TestHostState state;
			Test::TestMethodHost host(state);
			const MethodDescriptor& echo = RequireMethod(*setup.Methods, "test.echo");
			const auto invoke = [&](std::string_view text) -> std::pair<MethodResult, Json>
			{
				const Result<PreparedParams> prepared = setup.Methods->PrepareParams(echo, ParseRegistryJson(text));
				REQUIRE(prepared.has_value());
				Scope<MethodContext> context = host.CreateContext(MethodRequest{
					.Info = { .Client = 1, .ClientName = "test", .Id = Json(1), .Method = "test.echo", .TranscriptLine = std::nullopt },
					.Options = prepared->Options,
					.Method = &echo,
					.Params = prepared->Params,
					.Registry = setup.Methods.get(),
					.PhaseMarker = nullptr,
					.NestingDepth = 0,
				});
				MethodResult result = setup.Methods->Invoke(*context);
				return { std::move(result), context->GetParams() };
			};
			const std::string_view params = R"({"text":"a","components":{"MeshRenderer":{"Mesh":"engine://Meshes/Cube",
				"Materials":["Assets/Red.material","00000000000000AB"]}}})";

			// Without a resolver the spellings reach the strict reader, which accepts only handles.
			{
				const auto [result, read] = invoke(params);
				REQUIRE(std::holds_alternative<Error>(result));
				CHECK(std::get<Error>(result).GetCode() == ErrorCode::InvalidArgument);
				CHECK(state.Calls.empty());
			}

			// Convention 13: paths become handles before the handler runs; a handle is kept as given and never reaches the
			// resolver.
			state.AssetReferenceResolver = &assets;
			{
				const auto [result, read] = invoke(params);
				const std::string failure = std::holds_alternative<Error>(result) ? std::get<Error>(result).ToString() : std::string();
				REQUIRE_MESSAGE(std::holds_alternative<Json>(result), failure);
				CHECK(read["components"]["MeshRenderer"]["Mesh"] == Json("0000000000000101"));
				CHECK(read["components"]["MeshRenderer"]["Materials"] == ParseRegistryJson(R"(["0000000000abcdef","00000000000000AB"])"));
				CHECK(assets.Calls == std::vector<std::string>{ "engine://Meshes/Cube", "Assets/Red.material" });
				CHECK(state.Calls == std::vector<std::string>{ "handler test.echo" });
			}

			// An asset of another type is InvalidParams at its value, and the handler does not run.
			state.Calls.clear();
			{
				const auto [result, read] = invoke(R"({"text":"a","components":{"MeshRenderer":{"Mesh":"Assets/Red.material"}}})");
				REQUIRE(std::holds_alternative<Error>(result));
				const Error& error = std::get<Error>(result);
				CHECK(error.GetCode() == ErrorCode::InvalidArgument);
				REQUIRE(error.GetIssues().size() == 1);
				CHECK(error.GetIssues()[0].JsonPointer == "/components/MeshRenderer/Mesh");
				CHECK(error.GetIssues()[0].Message.contains("not a Mesh"));
				CHECK(state.Calls.empty());
			}

			// A path that names nothing is NotFound, located, with the resolver's hint.
			{
				const auto [result, read] = invoke(R"({"text":"a","components":{"MeshRenderer":{"Materials":["Assets/Red.material","Assets/Blue.material"]}}})");
				REQUIRE(std::holds_alternative<Error>(result));
				const Error& error = std::get<Error>(result);
				CHECK(error.GetCode() == ErrorCode::NotFound);
				REQUIRE(error.GetIssues().size() == 1);
				CHECK(error.GetIssues()[0].JsonPointer == "/components/MeshRenderer/Materials/1");
				CHECK(error.GetHint().contains("Assets/Red.material"));
				CHECK(state.Calls.empty());
			}

			// Any other resolver error is the host's own and keeps its code (here InvalidState), located at its value; it
			// outranks the NotFound of another reference.
			{
				const auto [result, read] =
					invoke(R"({"text":"a","components":{"MeshRenderer":{"Mesh":"engine://Meshes/Nothing","Materials":["Assets/Unavailable.material"]}}})");
				REQUIRE(std::holds_alternative<Error>(result));
				const Error& error = std::get<Error>(result);
				CHECK(error.GetCode() == ErrorCode::InvalidState);
				CHECK(error.GetMessageText().contains("needs an open project"));
				CHECK(error.GetLocation().JsonPointer.value_or(std::string()) == "/components/MeshRenderer/Materials/0");
				CHECK(state.Calls.empty());
			}
		}

		TEST_CASE("MethodRegistry: unknown params and component fields are InvalidParams with did-you-mean hints")
		{
			RegistrySetup setup;
			Test::TestHostState state;
			Test::TestMethodHost host(state);
			const MethodDescriptor& echo = RequireMethod(*setup.Methods, "test.echo");
			const Result<PreparedParams> prepared = setup.Methods->PrepareParams(echo,
				ParseRegistryJson(R"({"text":"a","cuont":2,"components":{"RigidBody":{"Mas":3,"Friction":-0.2}}})"));
			REQUIRE(prepared.has_value());
			Scope<MethodContext> context = host.CreateContext(MethodRequest{
				.Info = { .Client = 1, .ClientName = "test", .Id = Json(1), .Method = "test.echo", .TranscriptLine = std::nullopt },
				.Options = prepared->Options,
				.Method = &echo,
				.Params = prepared->Params,
				.Registry = setup.Methods.get(),
				.PhaseMarker = nullptr,
				.NestingDepth = 0,
			});
			MethodResult result = setup.Methods->Invoke(*context);
			REQUIRE(std::holds_alternative<Error>(result));
			const Error& error = std::get<Error>(result);
			CHECK(error.GetCode() == ErrorCode::InvalidArgument);
			const std::vector<ErrorIssue>& issues = error.GetIssues();
			const auto find = [&issues](std::string_view pointer) -> const ErrorIssue*
			{
				for (const ErrorIssue& issue : issues)
				{
					if (issue.JsonPointer == pointer)
						return &issue;
				}
				return nullptr;
			};
			REQUIRE(find("/cuont") != nullptr);
			CHECK(find("/cuont")->Hint.contains("count"));
			REQUIRE(find("/components/RigidBody/Mas") != nullptr);
			CHECK(find("/components/RigidBody/Mas")->Hint.contains("Mass"));
			CHECK(find("/components/RigidBody/Friction") != nullptr);
			CHECK(state.Calls.empty()); // the handler never ran
		}

		TEST_CASE("MethodRegistry: Invoke runs the handler with the parsed params and serializes its result")
		{
			RegistrySetup setup;
			Test::TestHostState state;
			Test::TestMethodHost host(state);
			const MethodDescriptor& echo = RequireMethod(*setup.Methods, "test.echo");
			Scope<MethodContext> context = host.CreateContext(MethodRequest{
				.Info = { .Client = 1, .ClientName = "test", .Id = Json(1), .Method = "test.echo", .TranscriptLine = std::nullopt },
				.Options = {},
				.Method = &echo,
				.Params = ParseRegistryJson(R"({"text":"hi","count":3,"components":{"Camera":{}}})"),
				.Registry = setup.Methods.get(),
				.PhaseMarker = nullptr,
				.NestingDepth = 0,
			});
			MethodResult result = setup.Methods->Invoke(*context);
			REQUIRE(std::holds_alternative<Json>(result));
			Json& json = std::get<Json>(result);
			CHECK(json["text"] == Json("hi"));
			CHECK(json["count"] == Json(3));
			CHECK(json["shape"] == Json("Circle"));
			CHECK(json["componentNames"] == ParseRegistryJson(R"(["Camera"])"));
			CHECK(state.Calls == std::vector<std::string>{ "handler test.echo" });
		}

		TEST_CASE("MethodRegistry: params schemas mark required members and compact schemas drop component definitions")
		{
			RegistrySetup setup;
			const MethodDescriptor& echo = RequireMethod(*setup.Methods, "test.echo");
			Json full = setup.Methods->GetParamsSchema(echo, SchemaStyle::Full);
			CHECK(full["required"] == ParseRegistryJson(R"(["text"])"));
			CHECK(full["properties"].contains("dryRun"));
			CHECK(full["properties"].contains("ifRevision"));
			CHECK(full["$defs"].contains("RigidBody"));
			REQUIRE(JsonSchema::Validate(full, ParseRegistryJson(R"({"text":"a","components":{"RigidBody":{"Mass":2}}})")).has_value());
			CHECK_FALSE(JsonSchema::Validate(full, ParseRegistryJson(R"({"text":"a","components":{"RigidBody":{"Mas":2}}})")).has_value());

			Json compact = setup.Methods->GetParamsSchema(echo, SchemaStyle::Compact);
			CHECK_FALSE(compact.contains("$defs"));
			CHECK(compact["properties"]["components"]["type"] == Json("object"));
			CHECK(compact["required"] == ParseRegistryJson(R"(["text"])"));

			const MethodDescriptor& read = RequireMethod(*setup.Methods, "test.read");
			Json readSchema = setup.Methods->GetParamsSchema(read, SchemaStyle::Full);
			CHECK_FALSE(readSchema["properties"].contains("dryRun"));
			CHECK(readSchema["properties"].contains("ifRevision"));
		}

		TEST_CASE("MethodRegistry: Describe and the catalogues report the metadata")
		{
			RegistrySetup setup;
			Json description = setup.Methods->Describe(RequireMethod(*setup.Methods, "test.echo"));
			CHECK(description["name"] == Json("test.echo"));
			CHECK(description["domain"] == Json("test"));
			CHECK(description["exposeAsTool"] == Json(true));
			CHECK(description["mutates"] == Json(true));
			CHECK(description["supportsDryRun"] == Json(true));
			CHECK(description["availableInLauncher"] == Json(false));
			CHECK(description["timeoutSeconds"] == Json(60));
			CHECK(description["examples"][0]["params"]["text"] == Json("hello"));

			Json methods = setup.Methods->BuildMethodCatalog();
			CHECK(methods["Format"] == Json("MethodCatalog"));
			CHECK(methods["Methods"].size() == setup.Methods->GetMethods().size());

			Json tools = setup.Methods->BuildToolCatalog();
			CHECK(tools["Format"] == Json("McpCatalog"));
			REQUIRE(tools["Tools"].size() == 1);
			CHECK(tools["Tools"][0]["name"] == Json("test_echo"));
			CHECK(tools["Tools"][0]["method"] == Json("test.echo"));
			CHECK_FALSE(tools["Tools"][0]["inputSchema"].contains("$defs"));
		}

		TEST_CASE("MethodRegistry: InvokeNested runs an op allowed in batches and rejects every other method before it runs")
		{
			RegistrySetup setup;
			Test::TestHostState state;
			Test::TestMethodHost host(state);
			Scope<MethodContext> batch = MakeRegistryContext(*setup.Methods, host, "test.read");

			const Result<Json> echoed = setup.Methods->InvokeNested(*batch, "test.echo", Json{ { "text", "a" } });
			REQUIRE(echoed.has_value());
			CHECK((*echoed)["text"] == Json("a"));

			const Result<Json> unknown = setup.Methods->InvokeNested(*batch, "test.ecko", Json::object());
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::NotFound);

			// An op's own errors are located relative to the op: its params are below "/params".
			const Result<Json> misspelled = setup.Methods->InvokeNested(*batch, "test.echo", Json{ { "text", "a" }, { "cuont", 1 } });
			REQUIRE_FALSE(misspelled.has_value());
			CHECK(misspelled.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE(misspelled.error().GetIssues().size() == 1);
			CHECK(misspelled.error().GetIssues()[0].JsonPointer == "/params/cuont");
			const Result<Json> failed = setup.Methods->InvokeNested(*batch, "test.fail", Json::object());
			REQUIRE_FALSE(failed.has_value());
			CHECK(failed.error().GetCode() == ErrorCode::NotFound);
			CHECK(failed.error().GetMessageText() == "nothing here");
			CHECK(failed.error().GetContexts() == std::vector<std::string>{ "while looking for something" });
			CHECK(failed.error().GetHint() == "look elsewhere");
			REQUIRE(failed.error().GetIssues().size() == 1);
			CHECK(failed.error().GetIssues()[0].JsonPointer == "/params");
			CHECK(failed.error().GetIssues()[0].Message == "nothing here");

			state.Calls.clear();
			for (const std::string_view method : { "test.pend", "test.large", "test.throw" }) // pending, or not AllowedInBatch
			{
				const Result<Json> refused = setup.Methods->InvokeNested(*batch, method, Json{ { "polls", 1 } });
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			}
			CHECK(state.Calls.empty()); // nothing ran
		}

		TEST_CASE("MethodRegistry: InvokeNested rejects reserved members in op params and applies the batch's options")
		{
			RegistrySetup setup;
			Test::TestHostState state;
			Test::TestMethodHost host(state);
			Scope<MethodContext> batch = MakeRegistryContext(*setup.Methods, host, "test.echo");

			for (const std::string_view member : { "dryRun", "ifRevision", "_meta" })
			{
				Json params{ { "text", "a" } };
				params[std::string(member)] = member == "_meta" ? Json::object() : Json(true);
				const Result<Json> refused = setup.Methods->InvokeNested(*batch, "test.echo", params);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
				REQUIRE(refused.error().GetIssues().size() == 1);
				CHECK(refused.error().GetIssues()[0].JsonPointer == "/params/" + std::string(member));
			}
			CHECK(state.Calls.empty());

			// In a dry-run batch, an op without supportsDryRun is Unsupported and one with it runs.
			Scope<MethodContext> dryBatch = MakeRegistryContext(*setup.Methods, host, "test.echo", RequestOptions{ .DryRun = true, .IfRevision = std::nullopt });
			const Result<Json> read = setup.Methods->InvokeNested(*dryBatch, "test.read", Json::object());
			REQUIRE_FALSE(read.has_value());
			CHECK(read.error().GetCode() == ErrorCode::Unsupported);
			CHECK(setup.Methods->InvokeNested(*dryBatch, "test.echo", Json{ { "text", "b" } }).has_value());
		}

		TEST_CASE("MethodNameToToolName: converts the domain dot and camelCase verbs to snake_case")
		{
			CHECK(MethodNameToToolName("entity.create") == "entity_create");
			CHECK(MethodNameToToolName("project.getSettings") == "project_get_settings");
			CHECK(MethodNameToToolName("play.waitFor") == "play_wait_for");
			CHECK(MethodNameToToolName("edit.getSelection") == "edit_get_selection");
		}

		TEST_CASE("ResolveComponentValue: resolves component names and suggests close ones")
		{
			RegistrySetup setup;
			const ResolveContext known{ .Registry = setup.Types.get(), .Owner = nullptr, .OwnerType = nullptr, .OwnerJson = nullptr, .Key = "Transform", .Schemas = nullptr };
			const Result<const FieldInfo*> field = ResolveComponentValue(known);
			REQUIRE(field.has_value());
			CHECK(*field == &setup.Types->FindComponent("Transform")->GetSelfField());

			ResolveContext unknown = known;
			unknown.Key = "Trasnform";
			const Result<const FieldInfo*> missing = ResolveComponentValue(unknown);
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			CHECK(missing.error().GetHint().contains("Transform"));
		}
	}

}
