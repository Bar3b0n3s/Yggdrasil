#include "TestsPCH.h"
#include "Engine/Native/NativeApi.h"

#include "Support/NativeTestModule.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace Engine {

	namespace {

		enum class NativeApiTestMode : int32_t
		{
			Idle = 0,
			Spin = 3
		};

		constexpr std::array<NativeEnumEntry, 2> NativeApiTestModes = { NativeEnumEntry{ .Name = "Idle", .Value = 0, .Description = "Stands still." },
			NativeEnumEntry{ .Name = "Spin", .Value = 3, .Description = "Turns." } };

		// Every kind of field, and a member the type does not register.
		struct NativeApiTestType
		{
			static constexpr std::string_view NativeTypeName = "Everything";
			bool Flag = true;
			int32_t Count = -3;
			uint32_t Mask = 5;
			float Speed = 1.5f;
			glm::vec2 Size = glm::vec2(1.0f, 2.0f);
			glm::vec3 Offset = glm::vec3(0.0f);
			glm::vec4 Weights = glm::vec4(0.25f);
			glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			glm::vec3 Tint = glm::vec3(1.0f, 0.5f, 0.25f);
			glm::vec4 Glow = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
			NativeEntity Target{};
			NativeApiTestMode Mode = NativeApiTestMode::Spin;
			uint32_t Unregistered = 7;
		};

		// The bytes a field of `kind` occupies: its member type's size.
		uint32_t NativeFieldKindSize(NativeFieldKind kind)
		{
			switch (kind)
			{
				case NativeFieldKind::Bool:   return sizeof(bool);
				case NativeFieldKind::Int32:  return sizeof(int32_t);
				case NativeFieldKind::UInt32: return sizeof(uint32_t);
				case NativeFieldKind::Float:  return sizeof(float);
				case NativeFieldKind::Vec2:   return sizeof(glm::vec2);
				case NativeFieldKind::Vec3:   return sizeof(glm::vec3);
				case NativeFieldKind::Vec4:   return sizeof(glm::vec4);
				case NativeFieldKind::Quat:   return sizeof(glm::quat);
				case NativeFieldKind::Color3: return sizeof(glm::vec3);
				case NativeFieldKind::Color4: return sizeof(glm::vec4);
				case NativeFieldKind::Entity: return sizeof(NativeEntity);
				case NativeFieldKind::Enum:   return sizeof(int32_t);
			}
			return 0;
		}

		// What a registration hands the engine, copied (the descriptors' views end with each call).
		struct RecordedField
		{
			std::string Name{};
			NativeFieldKind Kind = NativeFieldKind::Float;
			uint32_t Offset = 0;
			std::optional<double> Min{};
			std::optional<double> Max{};
			std::string Unit{};
			std::string RenamedFrom{};
			size_t EnumEntryCount = 0;
		};

		struct RecordedType
		{
			NativeTypeRole Role = NativeTypeRole::Component;
			std::string Name{};
			uint32_t Size = 0;
			uint32_t Alignment = 0;
			std::vector<std::byte> Defaults{};
			std::vector<RecordedField> Fields{};
		};

		class RecordingRegistrar final : public INativeRegistrar
		{
		public:
			void AddType(const NativeTypeDescriptor& type) override
			{
				RecordedType recorded{ .Role = type.Role, .Name = std::string(type.Name), .Size = type.Size, .Alignment = type.Alignment };
				recorded.Defaults.resize(type.Size);
				std::memcpy(recorded.Defaults.data(), type.Defaults, type.Size);
				for (const NativeFieldDescriptor& field : type.Fields)
				{
					recorded.Fields.push_back(RecordedField{ .Name = std::string(field.Name),
						.Kind = field.Kind,
						.Offset = field.Offset,
						.Min = field.Options.Min,
						.Max = field.Options.Max,
						.Unit = std::string(field.Options.Unit),
						.RenamedFrom = std::string(field.Options.RenamedFrom),
						.EnumEntryCount = field.EnumEntries.size() });
				}
				Types.push_back(std::move(recorded));
			}

			void AddSystem(const NativeSystemDescriptor& system) override
			{
				Systems.emplace_back(system.Name);
			}

			std::vector<RecordedType> Types{};
			std::vector<std::string> Systems{};
		};

	}

	TEST_SUITE("Native")
	{
		// Docs/Decisions/0030-m19-contract.md decision 26: NativeApi.h is header-only and the contract's, so its builders are
		// tested live: a type is described by its name, size, alignment, defaults and the kind and offset of each field.
		TEST_CASE("NativeApi: the builder describes a type's name, size, defaults and fields with their kinds and offsets")
		{
			RecordingRegistrar registrar;
			registrar.Add(NativeComponent<NativeApiTestType>("Holds one field of every kind.")
					.Field("Flag", &NativeApiTestType::Flag, "A bool.")
					.Field("Count", &NativeApiTestType::Count, "An int32.", { .Min = -10.0, .Max = 10.0 })
					.Field("Mask", &NativeApiTestType::Mask, "A uint32.")
					.Field("Speed", &NativeApiTestType::Speed, "A float.", { .Min = 0.0, .Unit = "m/s", .RenamedFrom = "Velocity" })
					.Field("Size", &NativeApiTestType::Size, "A vec2.")
					.Field("Offset", &NativeApiTestType::Offset, "A vec3.")
					.Field("Weights", &NativeApiTestType::Weights, "A vec4.")
					.Field("Rotation", &NativeApiTestType::Rotation, "A quaternion.")
					.ColorField("Tint", &NativeApiTestType::Tint, "A colour without alpha.")
					.ColorField("Glow", &NativeApiTestType::Glow, "A colour with alpha.")
					.Field("Target", &NativeApiTestType::Target, "An entity.")
					.EnumField("Mode", &NativeApiTestType::Mode, "An enum.", NativeApiTestModes));
			registrar.Add(NativeResource<Test::NativeTestCounter>("A resource.").Field("Ticks", &Test::NativeTestCounter::Ticks, "Ticks."));
			registrar.AddSystem({ .Name = "Spin", .Description = "Turns.", .Run = nullptr });

			REQUIRE(registrar.Types.size() == 2);
			const RecordedType& type = registrar.Types[0];
			CHECK(type.Role == NativeTypeRole::Component);
			CHECK(type.Name == "Everything");
			CHECK(type.Size == sizeof(NativeApiTestType));
			CHECK(type.Alignment == alignof(NativeApiTestType));
			// The defaults hold each registered field as a T{} holds it and zero everywhere else: the unregistered member and
			// the padding (three bytes after Flag), whose bytes in a T{} are indeterminate, so the defaults are the same bytes in
			// every registration (decision 26; the layout hash and every new value start from them).
			REQUIRE(type.Defaults.size() == sizeof(NativeApiTestType));
			NativeApiTestType defaults{ .Flag = false, .Count = 0, .Speed = 0.0f, .Mode = NativeApiTestMode::Idle, .Unregistered = 1 };
			std::memcpy(&defaults, type.Defaults.data(), sizeof(defaults));
			CHECK(defaults.Flag);
			CHECK(defaults.Count == -3);
			CHECK(defaults.Speed == 1.5f);
			CHECK(defaults.Tint == glm::vec3(1.0f, 0.5f, 0.25f));
			CHECK(defaults.Mode == NativeApiTestMode::Spin);
			CHECK(defaults.Unregistered == 0);
			std::vector<bool> registered(type.Defaults.size(), false);
			for (const RecordedField& field : type.Fields)
			{
				for (uint32_t index = 0; index < NativeFieldKindSize(field.Kind); ++index)
					registered[field.Offset + index] = true;
			}
			for (size_t index = 0; index < type.Defaults.size(); ++index)
			{
				CAPTURE(index);
				if (!registered[index])
					CHECK(type.Defaults[index] == std::byte{ 0 });
			}

			const std::vector<std::pair<std::string, NativeFieldKind>> expected = { { "Flag", NativeFieldKind::Bool }, { "Count", NativeFieldKind::Int32 },
				{ "Mask", NativeFieldKind::UInt32 }, { "Speed", NativeFieldKind::Float }, { "Size", NativeFieldKind::Vec2 }, { "Offset", NativeFieldKind::Vec3 },
				{ "Weights", NativeFieldKind::Vec4 }, { "Rotation", NativeFieldKind::Quat }, { "Tint", NativeFieldKind::Color3 },
				{ "Glow", NativeFieldKind::Color4 }, { "Target", NativeFieldKind::Entity }, { "Mode", NativeFieldKind::Enum } };
			REQUIRE(type.Fields.size() == expected.size());
			for (size_t index = 0; index < expected.size(); ++index)
			{
				CAPTURE(index);
				CHECK(type.Fields[index].Name == expected[index].first);
				CHECK(type.Fields[index].Kind == expected[index].second);
			}
			CHECK(type.Fields[0].Offset == offsetof(NativeApiTestType, Flag));
			CHECK(type.Fields[3].Offset == offsetof(NativeApiTestType, Speed));
			CHECK(type.Fields[10].Offset == offsetof(NativeApiTestType, Target));
			CHECK(type.Fields[11].Offset == offsetof(NativeApiTestType, Mode));
			CHECK(type.Fields[1].Min == -10.0);
			CHECK(type.Fields[1].Max == 10.0);
			CHECK(type.Fields[3].Unit == "m/s");
			CHECK(type.Fields[3].RenamedFrom == "Velocity");
			CHECK_FALSE(type.Fields[0].Min.has_value());
			CHECK(type.Fields[11].EnumEntryCount == 2);

			CHECK(registrar.Types[1].Role == NativeTypeRole::Resource);
			CHECK(registrar.Types[1].Name == "TestCounter");
			CHECK(registrar.Systems == std::vector<std::string>{ "Spin" });
		}

		// The test module (Support/NativeTestModule.h) registers its components, resource and systems in order, and the entry
		// points carry the interface version the module was compiled against.
		TEST_CASE("NativeApi: a module's entry point carries its interface version and registers in order")
		{
			for (const NativeModuleEntryPoint entry : { Test::GetNativeTestModule(), Test::GetNativeTestModuleV2(), Test::GetFailingNativeTestModule() })
			{
				CHECK(entry.ApiVersion == NativeApiVersion);
				REQUIRE(entry.Register != nullptr);
			}
			RecordingRegistrar registrar;
			Test::GetNativeTestModule().Register(registrar);
			REQUIRE(registrar.Types.size() == 3);
			CHECK(registrar.Types[0].Name == "TestSpinner");
			CHECK(registrar.Types[1].Name == "TestMover");
			CHECK(registrar.Types[2].Name == "TestCounter");
			CHECK(registrar.Types[2].Role == NativeTypeRole::Resource);
			CHECK(registrar.Systems == std::vector<std::string>{ "Spin", "Move", "Count" });

			RecordingRegistrar failing;
			Test::GetFailingNativeTestModule().Register(failing);
			CHECK(failing.Systems == std::vector<std::string>{ "Spin", "Move", "Count", "Fail" });
		}
	}

}
