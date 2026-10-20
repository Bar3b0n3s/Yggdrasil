#include "TestsPCH.h"
#include "Engine/Native/NativeModule.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Random.h"
#include "Support/Fuzz.h"
#include "Support/NativeTestModule.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace {

		// Registrations that each break one rule of BuildNativeModuleTypes (decision 26).
		struct LowercaseType
		{
			static constexpr std::string_view NativeTypeName = "spinner";
			float Speed = 1.0f;
		};

		struct OutOfRangeDefault
		{
			static constexpr std::string_view NativeTypeName = "OutOfRange";
			float Speed = 5.0f;
		};

		struct TwoFields
		{
			static constexpr std::string_view NativeTypeName = "TwoFields";
			float First = 0.0f;
			float Second = 0.0f;
		};

		void RegisterLowercaseType(INativeRegistrar& registrar)
		{
			registrar.Add(NativeComponent<LowercaseType>("Its name is not PascalCase.").Field("Speed", &LowercaseType::Speed, "A speed."));
		}

		void RegisterOutOfRangeDefault(INativeRegistrar& registrar)
		{
			registrar.Add(NativeComponent<OutOfRangeDefault>("Its default is above its maximum.").Field("Speed", &OutOfRangeDefault::Speed, "A speed.", { .Max = 1.0 }));
		}

		void RegisterDuplicateField(INativeRegistrar& registrar)
		{
			registrar.Add(NativeComponent<TwoFields>("Two fields share a name.")
					.Field("First", &TwoFields::First, "The first.")
					.Field("First", &TwoFields::Second, "The second, misnamed."));
		}

		void RegisterOverlappingFields(INativeRegistrar& registrar)
		{
			registrar.Add(NativeComponent<TwoFields>("Two fields share a member.")
					.Field("First", &TwoFields::First, "The first.")
					.Field("Second", &TwoFields::First, "The first again."));
		}

		void RegisterEmptyDescription(INativeRegistrar& registrar)
		{
			registrar.Add(NativeComponent<TwoFields>("Its field has no description.").Field("First", &TwoFields::First, ""));
		}

		void RegisterSystemWithoutRun(INativeRegistrar& registrar)
		{
			registrar.AddSystem({ .Name = "Idle", .Description = "Has no function.", .Run = nullptr });
		}

		void RegisterTypeTwice(INativeRegistrar& registrar)
		{
			Test::RegisterNativeTestModule(registrar);
			registrar.Add(NativeComponent<Test::NativeTestMover>("Registered again.").Field("Speed", &Test::NativeTestMover::Speed, "A speed."));
		}

		void RegisterNothing(INativeRegistrar& /*registrar*/)
		{
		}

		// Layout 1, then the standard library throws (std::vector::at past the end), as a module's own code may.
		void RegisterThrowing(INativeRegistrar& registrar)
		{
			Test::RegisterNativeTestModule(registrar);
			const std::vector<int> empty{};
			static_cast<void>(empty.at(1));
		}

		void DoNothing(INativeStepContext& /*context*/)
		{
		}

		// Layout 1 with other descriptions only: the same layout.
		void RegisterRedescribedModule(INativeRegistrar& registrar)
		{
			registrar.Add(NativeComponent<Test::NativeTestSpinner>("Spins, said differently.")
					.Field("DegreesPerSecond", &Test::NativeTestSpinner::DegreesPerSecond, "Turn rate.", { .Min = -3600.0, .Max = 3600.0, .Unit = "deg/s" })
					.Field("Axis", &Test::NativeTestSpinner::Axis, "Turn axis.")
					.Field("Turns", &Test::NativeTestSpinner::Turns, "Turn count."));
			registrar.Add(NativeComponent<Test::NativeTestMover>("Moves, said differently.")
					.Field("Speed", &Test::NativeTestMover::Speed, "Full-input speed.", { .Min = 0.0, .Unit = "m/s" })
					.Field("Follow", &Test::NativeTestMover::Follow, "Followed entity."));
			registrar.Add(NativeResource<Test::NativeTestCounter>("Counts, said differently.")
					.Field("Ticks", &Test::NativeTestCounter::Ticks, "Tick count.")
					.Field("Distance", &Test::NativeTestCounter::Distance, "Distance moved.", { .Min = 0.0, .Unit = "m" }));
			registrar.AddSystem({ .Name = "Spin", .Description = "Spins.", .Run = &DoNothing });
			registrar.AddSystem({ .Name = "Move", .Description = "Moves.", .Run = &DoNothing });
			registrar.AddSystem({ .Name = "Count", .Description = "Counts.", .Run = &DoNothing });
		}

		[[nodiscard]] Ref<const NativeModuleTypes> RequireTypes(NativeModuleRegisterFunction registerFunction)
		{
			Result<Ref<const NativeModuleTypes>> types = BuildNativeModuleTypes({ .ApiVersion = NativeApiVersion, .Register = registerFunction }, nullptr);
			REQUIRE_MESSAGE(types.has_value(), types.error().ToString());
			return std::move(*types);
		}

		[[nodiscard]] Json ParseJson(std::string_view text)
		{
			Result<Json> json = JsonReader::Parse(text);
			REQUIRE_MESSAGE(json.has_value(), json.error().ToString());
			return std::move(*json);
		}

	}

	TEST_SUITE("Native")
	{
		// Docs/Decisions/0030-m19-contract.md decision 26 (N-B): the registration is copied into immutable types, each role in
		// registration order, components named "Native.<Name>", every field with its kind, offset and metadata.
		TEST_CASE("NativeModule: a registration builds its components, resources and systems in registration order" * doctest::skip(true))
		{
			const Ref<const NativeModuleTypes> types = RequireTypes(&Test::RegisterNativeTestModule);
			REQUIRE(types->GetComponents().size() == 2);
			const NativeTypeInfo& spinner = types->GetComponents()[0];
			CHECK(spinner.Role == NativeTypeRole::Component);
			CHECK(spinner.Index == 0);
			CHECK(spinner.Name == "TestSpinner");
			CHECK(spinner.RegistryName == "Native.TestSpinner");
			CHECK(spinner.Size == sizeof(Test::NativeTestSpinner));
			CHECK(spinner.Alignment == alignof(Test::NativeTestSpinner));
			CHECK(spinner.Defaults.size() == sizeof(Test::NativeTestSpinner));
			REQUIRE(spinner.Fields.size() == 3);
			CHECK(spinner.Fields[0].Name == "DegreesPerSecond");
			CHECK(spinner.Fields[0].Kind == NativeFieldKind::Float);
			CHECK(spinner.Fields[0].Offset == offsetof(Test::NativeTestSpinner, DegreesPerSecond));
			CHECK(spinner.Fields[0].Max == 3600.0);
			CHECK(spinner.Fields[0].Unit == "deg/s");
			CHECK(spinner.Fields[1].Kind == NativeFieldKind::Vec3);
			CHECK(spinner.Fields[2].Kind == NativeFieldKind::UInt32);
			CHECK(types->GetComponents()[1].RegistryName == "Native.TestMover");
			CHECK(types->GetComponents()[1].Index == 1);
			REQUIRE(types->GetResources().size() == 1);
			CHECK(types->GetResources()[0].Role == NativeTypeRole::Resource);
			CHECK(types->GetResources()[0].RegistryName == "TestCounter");
			REQUIRE(types->GetSystems().size() == 3);
			CHECK(types->GetSystems()[0].Name == "Spin");
			CHECK(types->GetSystems()[1].Name == "Move");
			CHECK(types->GetSystems()[2].Name == "Count");
			CHECK(types->GetSystems()[2].Run != nullptr);
			CHECK(types->FindComponent("Native.TestMover") == &types->GetComponents()[1]);
			CHECK(types->FindComponent("TestMover") == nullptr);
			CHECK(types->FindResource("TestCounter") == &types->GetResources()[0]);
			CHECK(types->FindResource("Native.TestCounter") == nullptr);
		}

		// Decision 26 (N-B): a registration that breaks a rule is refused as a whole with Validation naming what broke it.
		TEST_CASE("NativeModule: a registration that breaks a rule is refused, naming the type, field or system" * doctest::skip(true))
		{
			struct Case
			{
				NativeModuleRegisterFunction Register = nullptr;
				std::string_view Named{};
			};
			for (const Case& refused : { Case{ &RegisterLowercaseType, "spinner" }, Case{ &RegisterOutOfRangeDefault, "Speed" },
					 Case{ &RegisterDuplicateField, "First" }, Case{ &RegisterOverlappingFields, "Second" }, Case{ &RegisterEmptyDescription, "First" },
					 Case{ &RegisterSystemWithoutRun, "Idle" }, Case{ &RegisterTypeTwice, "TestMover" }, Case{ &RegisterNothing, "" } })
			{
				CAPTURE(refused.Named);
				const Result<Ref<const NativeModuleTypes>> types = BuildNativeModuleTypes({ .ApiVersion = NativeApiVersion, .Register = refused.Register }, nullptr);
				REQUIRE_FALSE(types.has_value());
				CHECK(types.error().GetCode() == ErrorCode::Validation);
				CHECK(types.error().ToString().contains(refused.Named));
			}
		}

		// Decision 26 (N-B): the registration is the module's code and NativeModule.cpp a §4.6 boundary, so an exception that
		// escapes it refuses the module with Validation naming the exception, and nothing it registered is kept; the process
		// goes on, and a later registration of a sound module succeeds.
		TEST_CASE("NativeModule: a registration that throws is refused, naming the exception" * doctest::skip(true))
		{
			const Result<Ref<const NativeModuleTypes>> types = BuildNativeModuleTypes({ .ApiVersion = NativeApiVersion, .Register = &RegisterThrowing }, nullptr);
			REQUIRE_FALSE(types.has_value());
			CHECK(types.error().GetCode() == ErrorCode::Validation);
			CHECK(types.error().ToString().contains("exception"));
			CHECK(RequireTypes(&Test::RegisterNativeTestModule)->GetComponents().size() == 2);
		}

		// Decision 26 (N-B): the interface version is checked before the registration runs.
		TEST_CASE("NativeModule: a module built against another interface version is refused" * doctest::skip(true))
		{
			const Result<Ref<const NativeModuleTypes>> newer = BuildNativeModuleTypes({ .ApiVersion = NativeApiVersion + 1, .Register = &Test::RegisterNativeTestModule }, nullptr);
			REQUIRE_FALSE(newer.has_value());
			CHECK(newer.error().GetCode() == ErrorCode::UnsupportedVersion);
			const Result<Ref<const NativeModuleTypes>> empty = BuildNativeModuleTypes({ .ApiVersion = NativeApiVersion, .Register = nullptr }, nullptr);
			REQUIRE_FALSE(empty.has_value());
			CHECK(empty.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(RequireTypes(&Test::RegisterNativeTestModule) != nullptr);
		}

		// Decision 27 (N-B): a value's canonical JSON holds every field in registration order in its kind's form, and reads back
		// to the same bytes; an Entity is 16 hex digits or null.
		TEST_CASE("NativeModule: a value converts between its bytes and its canonical JSON, every field in registration order" * doctest::skip(true))
		{
			const Ref<const NativeModuleTypes> types = RequireTypes(&Test::RegisterNativeTestModule);
			const NativeTypeInfo& spinner = types->GetComponents()[0];
			const Test::NativeTestSpinner value{ .DegreesPerSecond = 45.5f, .Axis = glm::vec3(1.0f, 0.0f, 0.0f), .Turns = 3 };
			const Json json = NativeValueToJson(spinner, std::as_bytes(std::span(&value, 1)));
			CHECK(json == ParseJson(R"({"DegreesPerSecond": 45.5, "Axis": [1, 0, 0], "Turns": 3})"));
			Test::NativeTestSpinner read{};
			const Result<NativeValueReadReport> report = NativeValueFromJson(spinner, json, "/Components/Native.TestSpinner", std::as_writable_bytes(std::span(&read, 1)));
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK_FALSE(report->Changed);
			CHECK(report->Notes.empty());
			CHECK(read.DegreesPerSecond == value.DegreesPerSecond);
			CHECK(read.Axis == value.Axis);
			CHECK(read.Turns == value.Turns);

			const NativeTypeInfo& mover = types->GetComponents()[1];
			const Test::NativeTestMover follows{ .Speed = 1.0f, .Follow = NativeEntity{ 0x5d1c9a7e33b04f12ull } };
			CHECK(NativeValueToJson(mover, std::as_bytes(std::span(&follows, 1))) == ParseJson(R"({"Speed": 1, "Follow": "5d1c9a7e33b04f12"})"));
			const Test::NativeTestMover alone{};
			CHECK(NativeValueToJson(mover, std::as_bytes(std::span(&alone, 1))) == ParseJson(R"({"Speed": 2, "Follow": null})"));

			const Json schema = GetNativeTypeSchema(spinner);
			const std::optional<JsonReader> properties = JsonReader(schema).FindMember("properties");
			REQUIRE(properties.has_value());
			CHECK(properties->FindMember("DegreesPerSecond").has_value());
			CHECK(properties->FindMember("Turns").has_value());
		}

		// Decision 27 (N-B): a stored value migrates to a changed layout by field name.
		TEST_CASE("NativeModule: canonicalization carries renamed fields, drops unknown members and resets values that do not convert" * doctest::skip(true))
		{
			const Ref<const NativeModuleTypes> types = RequireTypes(&Test::RegisterNativeTestModuleV2);
			const NativeTypeInfo& spinner = types->GetComponents()[0];
			const NativeTypeInfo& mover = types->GetComponents()[1];

			// Layout 1's spinner: DegreesPerSecond becomes Speed, Reversed takes its default, a stray member is dropped.
			const Result<NativeCanonicalValue> renamed = CanonicalizeNativeValue(spinner,
				ParseJson(R"({"DegreesPerSecond": 30, "Axis": [0, 0, 1], "Turns": 4, "Wobble": true})"), "/Components/Native.TestSpinner");
			REQUIRE_MESSAGE(renamed.has_value(), renamed.error().ToString());
			CHECK(renamed->Value.Get() == ParseJson(R"({"Speed": 30, "Axis": [0, 0, 1], "Turns": 4, "Reversed": false})"));
			CHECK(renamed->Report.Changed);
			REQUIRE(renamed->Report.Notes.size() == 1);
			CHECK(renamed->Report.Notes[0].Code == NativeFieldDroppedCode);
			CHECK(renamed->Report.Notes[0].Field == "Wobble");

			// Layout 1's mover: Follow is gone.
			const Result<NativeCanonicalValue> dropped = CanonicalizeNativeValue(mover, ParseJson(R"({"Speed": 3, "Follow": "5d1c9a7e33b04f12"})"), "/Components/Native.TestMover");
			REQUIRE_MESSAGE(dropped.has_value(), dropped.error().ToString());
			CHECK(dropped->Value.Get() == ParseJson(R"({"Speed": 3})"));
			REQUIRE(dropped->Report.Notes.size() == 1);
			CHECK(dropped->Report.Notes[0].Code == NativeFieldDroppedCode);

			// A value of another form that does not convert is reset; one that converts exactly is kept.
			const Result<NativeCanonicalValue> reset = CanonicalizeNativeValue(spinner, ParseJson(R"({"Speed": "fast", "Turns": 2.0})"), "/Components/Native.TestSpinner");
			REQUIRE_MESSAGE(reset.has_value(), reset.error().ToString());
			CHECK(reset->Value.Get() == ParseJson(R"({"Speed": 90, "Axis": [0, 1, 0], "Turns": 2, "Reversed": false})"));
			REQUIRE(reset->Report.Notes.size() == 1);
			CHECK(reset->Report.Notes[0].Code == NativeFieldResetCode);
			CHECK(reset->Report.Notes[0].Field == "Speed");

			// A canonical value is unchanged.
			const Result<NativeCanonicalValue> canonical = CanonicalizeNativeValue(mover, ParseJson(R"({"Speed": 3})"), "/Components/Native.TestMover");
			REQUIRE(canonical.has_value());
			CHECK_FALSE(canonical->Report.Changed);
			CHECK(canonical->Report.Notes.empty());

			// A value of the field's form that the field rejects is an error located at the field.
			const Result<NativeCanonicalValue> rejected = CanonicalizeNativeValue(mover, ParseJson(R"({"Speed": -1})"), "/Components/Native.TestMover");
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::Validation);
			REQUIRE_FALSE(rejected.error().GetIssues().empty());
			CHECK(rejected.error().GetIssues()[0].JsonPointer == "/Components/Native.TestMover/Speed");
			CHECK_FALSE(CanonicalizeNativeValue(mover, ParseJson("[]"), "/Components/Native.TestMover").has_value());
		}

		// Decision 31 (N-B): the layout hash tells a reload that changes the canonical JSON from one that does not.
		TEST_CASE("NativeModule: the layout hash changes with a type's layout and only with it" * doctest::skip(true))
		{
			const uint64_t first = RequireTypes(&Test::RegisterNativeTestModule)->GetLayoutHash();
			CHECK(RequireTypes(&Test::RegisterNativeTestModule)->GetLayoutHash() == first);
			CHECK(RequireTypes(&RegisterRedescribedModule)->GetLayoutHash() == first);
			CHECK(RequireTypes(&Test::RegisterNativeTestModuleV2)->GetLayoutHash() != first);
			CHECK(RequireTypes(&Test::RegisterFailingNativeTestModule)->GetLayoutHash() == first);
		}

		// Decision 27 (N-B): a native component's JSON comes from a scene file, so its reader has a fuzz target
		// (NativeComponent in Tests/Data/Fuzz/Targets.json, added by the W19.1 integration): mutated text that parses is
		// canonicalized to a value or refused with Validation, never a crash.
		TEST_CASE("NativeModule: 10,000 seeded mutations of a native component's JSON never crash and always return Result" * doctest::test_suite(Test::FuzzSuite)
			* doctest::skip(true))
		{
			const Ref<const NativeModuleTypes> types = RequireTypes(&Test::RegisterNativeTestModuleV2);
			const std::string seed = R"({"Speed": 30, "Axis": [0, 0, 1], "Turns": 4, "Reversed": true})";
			const Test::FuzzRun run = Test::RunFuzzCase(0x4E43C, 10000, [&types, &seed](Random& random, uint64_t /*index*/)
			{
				std::string mutated = seed;
				Test::MutateText(mutated, random);
				const Result<Json> json = JsonReader::Parse(mutated);
				if (!json.has_value())
					return false;
				const NativeTypeInfo& type = types->GetComponents()[random.NextBool() ? 0 : 1];
				const Result<NativeCanonicalValue> canonical = CanonicalizeNativeValue(type, *json, "/Components/Native.TestSpinner");
				if (!canonical.has_value())
				{
					CHECK(canonical.error().GetCode() == ErrorCode::Validation);
					return false;
				}
				CHECK(canonical->Value.Get().is_object());
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
			{
				CHECK(run.Counts.Rejected > 0);
				CHECK(run.Counts.Accepted > 0);
			}
		}
	}

}
