#include "TestsPCH.h"
#include "Engine/Native/NativeModule.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Random.h"
#include "Engine/Reflection/JsonSchema.h"
#include "Support/Fuzz.h"
#include "Support/NativeTestModule.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstring>
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

		// One field of every NativeFieldKind.
		enum class TestMode : int32_t
		{
			Idle = 0,
			Run = 3
		};

		constexpr std::array<NativeEnumEntry, 2> TestModeEntries = {
			NativeEnumEntry{ .Name = "Idle", .Value = 0, .Description = "Stands still." },
			NativeEnumEntry{ .Name = "Run", .Value = 3, .Description = "Runs." },
		};

		struct AllKinds
		{
			static constexpr std::string_view NativeTypeName = "AllKinds";
			bool Flag = true;
			int32_t Count = -2;
			uint32_t Total = 7;
			float Speed = 1.5f;
			glm::vec2 Size = glm::vec2(1.0f, 2.0f);
			glm::vec3 Offset = glm::vec3(0.0f);
			glm::vec4 Weights = glm::vec4(0.25f);
			glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			glm::vec3 Tint = glm::vec3(1.0f, 0.5f, 0.0f);
			glm::vec4 Glow = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
			NativeEntity Target{};
			TestMode Mode = TestMode::Run;
		};

		void RegisterAllKinds(INativeRegistrar& registrar)
		{
			registrar.Add(NativeComponent<AllKinds>("A field of every kind.")
					.Field("Flag", &AllKinds::Flag, "A boolean.")
					.Field("Count", &AllKinds::Count, "A signed integer.", { .Min = -10.0, .Max = 10.0 })
					.Field("Total", &AllKinds::Total, "An unsigned integer.")
					.Field("Speed", &AllKinds::Speed, "A float.", { .Min = 0.0, .Unit = "m/s" })
					.Field("Size", &AllKinds::Size, "A vec2.")
					.Field("Offset", &AllKinds::Offset, "A vec3.")
					.Field("Weights", &AllKinds::Weights, "A vec4.", { .Min = 0.0, .Max = 1.0 })
					.Field("Rotation", &AllKinds::Rotation, "A quaternion.")
					.ColorField("Tint", &AllKinds::Tint, "A linear colour.")
					.ColorField("Glow", &AllKinds::Glow, "A linear colour with alpha.")
					.Field("Target", &AllKinds::Target, "An entity.")
					.EnumField("Mode", &AllKinds::Mode, "An enum.", TestModeEntries));
		}

		// Registrations that each break one more rule of BuildNativeModuleTypes (decision 26).
		struct EntityDefault
		{
			static constexpr std::string_view NativeTypeName = "EntityDefault";
			NativeEntity Target{ 5 };
		};

		struct Flagged
		{
			static constexpr std::string_view NativeTypeName = "Flagged";
			bool Flag = false;
		};

		struct Moded
		{
			static constexpr std::string_view NativeTypeName = "Moded";
			TestMode Mode = TestMode::Idle;
		};

		constexpr std::array<NativeEnumEntry, 2> RepeatedValueEntries = {
			NativeEnumEntry{ .Name = "Idle", .Value = 0, .Description = "Stands still." },
			NativeEnumEntry{ .Name = "Still", .Value = 0, .Description = "Stands still, again." },
		};

		void RegisterRenamedFromAField(INativeRegistrar& registrar)
		{
			registrar.Add(NativeComponent<TwoFields>("A field renamed from another field's name.")
					.Field("First", &TwoFields::First, "The first.", { .RenamedFrom = "Second" })
					.Field("Second", &TwoFields::Second, "The second."));
		}

		void RegisterRenamedTwice(INativeRegistrar& registrar)
		{
			registrar.Add(NativeComponent<TwoFields>("Two fields renamed from one name.")
					.Field("First", &TwoFields::First, "The first.", { .RenamedFrom = "Old" })
					.Field("Second", &TwoFields::Second, "The second.", { .RenamedFrom = "Old" }));
		}

		void RegisterEnumWithoutEntries(INativeRegistrar& registrar)
		{
			registrar.Add(NativeComponent<Moded>("An enum without entries.").EnumField("Mode", &Moded::Mode, "A mode.", std::span<const NativeEnumEntry>()));
		}

		void RegisterEnumWithRepeatedValue(INativeRegistrar& registrar)
		{
			registrar.Add(NativeComponent<Moded>("An enum whose entries share a value.").EnumField("Mode", &Moded::Mode, "A mode.", RepeatedValueEntries));
		}

		void RegisterBoundedBool(INativeRegistrar& registrar)
		{
			registrar.Add(NativeComponent<Flagged>("A bounded boolean.").Field("Flag", &Flagged::Flag, "A flag.", { .Min = 0.0 }));
		}

		void RegisterInvertedBounds(INativeRegistrar& registrar)
		{
			registrar.Add(NativeComponent<TwoFields>("Bounds the wrong way round.").Field("First", &TwoFields::First, "The first.", { .Min = 2.0, .Max = 1.0 }));
		}

		void RegisterEntityDefault(INativeRegistrar& registrar)
		{
			registrar.Add(NativeComponent<EntityDefault>("An entity field whose default names an entity.").Field("Target", &EntityDefault::Target, "An entity."));
		}

		// Hand-written descriptors: what a builder never produces, but a registrar must still refuse.
		constexpr std::array<std::byte, 8> EightZeroBytes{};

		void RegisterMisalignedField(INativeRegistrar& registrar)
		{
			const std::array<NativeFieldDescriptor, 1> fields = { NativeFieldDescriptor{ .Name = "Skewed", .Description = "A float at offset 2.", .Kind = NativeFieldKind::Float, .Offset = 2, .Options = {}, .EnumEntries = {} } };
			registrar.AddType({ .Role = NativeTypeRole::Component, .Name = "Skew", .Description = "A misaligned field.", .Size = 8, .Alignment = 4, .Defaults = EightZeroBytes.data(), .Fields = fields });
		}

		void RegisterFieldOutsideType(INativeRegistrar& registrar)
		{
			const std::array<NativeFieldDescriptor, 1> fields = { NativeFieldDescriptor{ .Name = "Beyond", .Description = "A float past the end.", .Kind = NativeFieldKind::Float, .Offset = 8, .Options = {}, .EnumEntries = {} } };
			registrar.AddType({ .Role = NativeTypeRole::Resource, .Name = "Short", .Description = "A field outside the type.", .Size = 8, .Alignment = 4, .Defaults = EightZeroBytes.data(), .Fields = fields });
		}

		void RegisterOddAlignment(INativeRegistrar& registrar)
		{
			registrar.AddType({ .Role = NativeTypeRole::Resource, .Name = "Odd", .Description = "An alignment of 3.", .Size = 6, .Alignment = 3, .Defaults = EightZeroBytes.data(), .Fields = {} });
		}

		[[nodiscard]] std::vector<std::byte> ToBytes(const AllKinds& value)
		{
			const auto bytes = std::as_bytes(std::span(&value, 1));
			return { bytes.begin(), bytes.end() };
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
		TEST_CASE("NativeModule: a registration builds its components, resources and systems in registration order")
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
		TEST_CASE("NativeModule: a registration that breaks a rule is refused, naming the type, field or system")
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
		TEST_CASE("NativeModule: a registration that throws is refused, naming the exception")
		{
			const Result<Ref<const NativeModuleTypes>> types = BuildNativeModuleTypes({ .ApiVersion = NativeApiVersion, .Register = &RegisterThrowing }, nullptr);
			REQUIRE_FALSE(types.has_value());
			CHECK(types.error().GetCode() == ErrorCode::Validation);
			CHECK(types.error().ToString().contains("exception"));
			CHECK(RequireTypes(&Test::RegisterNativeTestModule)->GetComponents().size() == 2);
		}

		// Decision 26 (N-B): the interface version is checked before the registration runs.
		TEST_CASE("NativeModule: a module built against another interface version is refused")
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
		TEST_CASE("NativeModule: a value converts between its bytes and its canonical JSON, every field in registration order")
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
		TEST_CASE("NativeModule: canonicalization carries renamed fields, drops unknown members and resets values that do not convert")
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

		// Decision 27 (N-B): every field kind has its JSON form, reads back to the value it wrote, and the type's JSON Schema
		// (native.status) accepts the canonical JSON and refuses what the type refuses, with the engine's own validator.
		TEST_CASE("NativeModule: every field kind converts between its bytes and its canonical JSON, which the type's schema accepts")
		{
			const Ref<const NativeModuleTypes> types = RequireTypes(&RegisterAllKinds);
			const NativeTypeInfo& type = types->GetComponents()[0];
			CHECK(NativeValueToJson(type, type.Defaults)
				== ParseJson(R"({"Flag": true, "Count": -2, "Total": 7, "Speed": 1.5, "Size": [1, 2], "Offset": [0, 0, 0], "Weights": [0.25, 0.25, 0.25, 0.25],
					"Rotation": [0, 0, 0, 1], "Tint": [1, 0.5, 0], "Glow": [0, 0, 0, 1], "Target": null, "Mode": "Run"})"));

			AllKinds value{};
			value.Flag = false;
			value.Count = 9;
			value.Total = 4000000000u;
			value.Speed = 0.1f;
			value.Size = glm::vec2(-3.0f, 0.5f);
			value.Offset = glm::vec3(1.0f, -2.0f, 3.5f);
			value.Weights = glm::vec4(0.0f, 0.1f, 0.2f, 1.0f);
			value.Rotation = glm::quat(0.0f, 0.0f, 1.0f, 0.0f);
			value.Tint = glm::vec3(0.2f, 0.4f, 0.6f);
			value.Glow = glm::vec4(1.0f, 1.0f, 1.0f, 0.5f);
			value.Target = NativeEntity{ 0x5d1c9a7e33b04f12ull };
			value.Mode = TestMode::Idle;
			const Json json = NativeValueToJson(type, ToBytes(value));
			// The canonical text a scene file holds for it (floats in their short float spelling).
			CHECK(JsonWriter::Write(json, JsonStyle::Minified).value_or(std::string())
				== R"({"Flag":false,"Count":9,"Total":4000000000,"Speed":0.1,"Size":[-3,0.5],"Offset":[1,-2,3.5],"Weights":[0,0.1,0.2,1],)"
				   R"("Rotation":[0,1,0,0],"Tint":[0.2,0.4,0.6],"Glow":[1,1,1,0.5],"Target":"5d1c9a7e33b04f12","Mode":"Idle"})");

			std::vector<std::byte> bytes(type.Size);
			const Result<NativeValueReadReport> report = NativeValueFromJson(type, json, "", bytes);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK_FALSE(report->Changed);
			CHECK(report->Notes.empty());
			AllKinds read{};
			std::memcpy(&read, bytes.data(), sizeof(AllKinds));
			CHECK(read.Flag == value.Flag);
			CHECK(read.Count == value.Count);
			CHECK(read.Total == value.Total);
			CHECK(read.Speed == value.Speed);
			CHECK(read.Size == value.Size);
			CHECK(read.Offset == value.Offset);
			CHECK(read.Weights == value.Weights);
			CHECK(read.Rotation == value.Rotation);
			CHECK(read.Tint == value.Tint);
			CHECK(read.Glow == value.Glow);
			CHECK(read.Target == value.Target);
			CHECK(read.Mode == value.Mode);

			const Json schema = GetNativeTypeSchema(type);
			const Status valid = JsonSchema::Validate(schema, json);
			CHECK_MESSAGE(valid.has_value(), (valid.has_value() ? std::string() : valid.error().ToString()));
			for (const std::string_view refused : { R"({"Mode": "Walk"})", R"({"Target": "5D1C9A7E33B04F12"})", R"({"Speed": -1})", R"({"Count": 1.5})",
					 R"({"Size": [1, 2, 3]})", R"({"Unknown": 1})" })
			{
				CAPTURE(refused);
				const Status checked = JsonSchema::Validate(schema, ParseJson(refused));
				REQUIRE_FALSE(checked.has_value());
				CHECK(checked.error().GetCode() == ErrorCode::Validation);
			}
			const std::optional<JsonReader> properties = JsonReader(schema).FindMember("properties");
			REQUIRE(properties.has_value());
			const std::optional<JsonReader> speed = properties->FindMember("Speed");
			REQUIRE(speed.has_value());
			CHECK(speed->GetValue().dump().contains("m/s"));
		}

		// Decision 27 (N-B): a member of its field's form that the field rejects is an error, located at the member; one that
		// converts exactly is converted, and one that does not convert is reset with a note.
		TEST_CASE("NativeModule: a value of the field's form that it rejects is an error, and one of another form converts or is reset")
		{
			const Ref<const NativeModuleTypes> types = RequireTypes(&RegisterAllKinds);
			const NativeTypeInfo& type = types->GetComponents()[0];
			for (const std::string_view rejected : { R"({"Target": "5D1C9A7E33B04F12"})", R"({"Target": "5d1c"})", R"({"Mode": "Rnu"})", R"({"Rotation": [0, 0, 0, 2]})",
					 R"({"Speed": 1e39})", R"({"Weights": [0, 0, 2, 0]})", R"({"Count": 11})" })
			{
				CAPTURE(rejected);
				const Result<NativeCanonicalValue> canonical = CanonicalizeNativeValue(type, ParseJson(rejected), "/Components/Native.AllKinds");
				REQUIRE_FALSE(canonical.has_value());
				CHECK(canonical.error().GetCode() == ErrorCode::Validation);
				REQUIRE_FALSE(canonical.error().GetIssues().empty());
				CHECK(canonical.error().GetIssues()[0].JsonPointer.starts_with("/Components/Native.AllKinds/"));
			}
			const Result<NativeCanonicalValue> misspelled = CanonicalizeNativeValue(type, ParseJson(R"({"Mode": "Rnu"})"), "");
			REQUIRE_FALSE(misspelled.has_value());
			REQUIRE_FALSE(misspelled.error().GetIssues().empty());
			CHECK(misspelled.error().GetIssues()[0].Suggestions == std::vector<std::string>{ "Run" });

			// An entry's value converts to the entry, an integral float to an integer.
			const Result<NativeCanonicalValue> converted = CanonicalizeNativeValue(type, ParseJson(R"({"Mode": 0, "Count": 4.0, "Total": 3})"), "");
			REQUIRE_MESSAGE(converted.has_value(), converted.error().ToString());
			CHECK(converted->Report.Notes.empty());
			CHECK(JsonReader(converted->Value.Get()).FindMember("Mode")->GetValue() == "Idle");
			CHECK(JsonReader(converted->Value.Get()).FindMember("Count")->GetValue() == 4);

			for (const std::string_view reset : { R"({"Count": 2.5})", R"({"Total": -1})", R"({"Offset": [1, 2]})", R"({"Flag": 1})", R"({"Mode": 7})",
					 R"({"Target": 12})", R"({"Size": [1, "2"]})" })
			{
				CAPTURE(reset);
				const Result<NativeCanonicalValue> canonical = CanonicalizeNativeValue(type, ParseJson(reset), "");
				REQUIRE_MESSAGE(canonical.has_value(), canonical.error().ToString());
				REQUIRE(canonical->Report.Notes.size() == 1);
				CHECK(canonical->Report.Notes[0].Code == NativeFieldResetCode);
				CHECK(canonical->Value.Get() == NativeValueToJson(type, type.Defaults));
			}
		}

		// Decision 26 (N-B): the rules on renamed fields, enum entries, bounds, defaults and the layout a descriptor gives.
		TEST_CASE("NativeModule: a registration whose renames, enum entries, bounds, defaults or layout break a rule is refused")
		{
			struct Case
			{
				NativeModuleRegisterFunction Register = nullptr;
				std::string_view Named{};
			};
			for (const Case& refused : { Case{ &RegisterRenamedFromAField, "First" }, Case{ &RegisterRenamedTwice, "Old" }, Case{ &RegisterEnumWithoutEntries, "Mode" },
					 Case{ &RegisterEnumWithRepeatedValue, "Still" }, Case{ &RegisterBoundedBool, "Flag" }, Case{ &RegisterInvertedBounds, "First" },
					 Case{ &RegisterEntityDefault, "Target" }, Case{ &RegisterMisalignedField, "Skewed" }, Case{ &RegisterFieldOutsideType, "Beyond" },
					 Case{ &RegisterOddAlignment, "Odd" } })
			{
				CAPTURE(refused.Named);
				const Result<Ref<const NativeModuleTypes>> types = BuildNativeModuleTypes({ .ApiVersion = NativeApiVersion, .Register = refused.Register }, nullptr);
				REQUIRE_FALSE(types.has_value());
				CHECK(types.error().GetCode() == ErrorCode::Validation);
				CHECK(types.error().ToString().contains(refused.Named));
			}
		}

		// Decision 31 (N-B): the layout hash tells a reload that changes the canonical JSON from one that does not.
		TEST_CASE("NativeModule: the layout hash changes with a type's layout and only with it")
		{
			const uint64_t first = RequireTypes(&Test::RegisterNativeTestModule)->GetLayoutHash();
			CHECK(RequireTypes(&Test::RegisterNativeTestModule)->GetLayoutHash() == first);
			CHECK(RequireTypes(&RegisterRedescribedModule)->GetLayoutHash() == first);
			CHECK(RequireTypes(&Test::RegisterNativeTestModuleV2)->GetLayoutHash() != first);
			CHECK(RequireTypes(&Test::RegisterFailingNativeTestModule)->GetLayoutHash() == first);
		}

		// Decision 27 (N-B): a native component's JSON comes from a scene file, so its reader has a fuzz target
		// (NativeComponent in Tests/Data/Fuzz/Targets.json, added by the W19.1 integration): mutated text that parses is
		// canonicalized to a value or refused with Validation, never a crash. The name holds no apostrophe, since the
		// hardening stage quotes fuzz case names (Support/Fuzz.cpp).
		TEST_CASE("NativeModule: 10,000 seeded mutations of the JSON of a native component never crash and always return Result" * doctest::test_suite(Test::FuzzSuite))
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
