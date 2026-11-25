#include "TestsPCH.h"

#include "Engine/Physics/PhysicsVehicle.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/Value.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/SceneTestFixture.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The Vehicles category's registration (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3 and the W44.2 record; the
// W44.2 review's finding 5): WheeledVehicle and VehicleWheel after the Joints category with their requirements, the
// CharacterController exclusion the RigidBody requirement gives WheeledVehicle, every ranged field bounded by the physics
// world's constants (Physics/PhysicsVehicle.h) so the editor and the world refuse the same values, and the two enums.

namespace Engine {

	namespace {

		const FieldMeta& FindVehicleMeta(const TypeRegistry& registry, std::string_view component, std::string_view field)
		{
			const ComponentInfo* info = registry.FindComponent(component);
			REQUIRE(info != nullptr);
			const FieldInfo* fieldInfo = info->FindField(field);
			REQUIRE(fieldInfo != nullptr);
			return fieldInfo->GetMeta();
		}

		// A ranged scalar field and the world's bounds it must carry.
		struct VehicleBound
		{
			std::string_view Component;
			std::string_view Field;
			double Min = 0.0;
			double Max = 0.0;
		};

		std::vector<std::string> EnumNames(const TypeRegistry& registry, std::string_view name)
		{
			const EnumInfo* info = registry.FindEnum(name);
			REQUIRE(info != nullptr);
			std::vector<std::string> names;
			for (const EnumEntry& entry : info->GetEntries())
				names.push_back(entry.Name);
			return names;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("VehiclesRegistration: WheeledVehicle and VehicleWheel follow Joints with their requirements, and WheeledVehicle never joins a CharacterController")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo* vehicle = registry->FindComponent("WheeledVehicle");
			const ComponentInfo* wheel = registry->FindComponent("VehicleWheel");
			REQUIRE(vehicle != nullptr);
			REQUIRE(wheel != nullptr);
			CHECK(vehicle->GetCategory() == "Vehicles");
			CHECK(wheel->GetCategory() == "Vehicles");
			CHECK(vehicle->GetVersion() == 1);
			CHECK(wheel->GetVersion() == 1);
			REQUIRE(vehicle->GetRequires().size() == 2);
			CHECK(vehicle->GetRequires()[0] == registry->FindComponent("Transform"));
			CHECK(vehicle->GetRequires()[1] == registry->FindComponent("RigidBody"));
			REQUIRE(wheel->GetRequires().size() == 1);
			CHECK(wheel->GetRequires()[0] == registry->FindComponent("Transform"));

			// The category comes after Joints, WheeledVehicle first.
			const std::span<const ComponentInfo* const> components = registry->GetComponents();
			const auto at = [&components](const ComponentInfo* info)
			{
				return std::find(components.begin(), components.end(), info) - components.begin();
			};
			CHECK(at(registry->FindComponent("FixedJoint")) < at(vehicle));
			CHECK(at(vehicle) + 1 == at(wheel));

			// Departure 2 of the W44.2 record: WheeledVehicle declares no exclusion of its own, since the RigidBody it requires and
			// a CharacterController exclude each other, so the two never meet on one entity.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity character = scene.CreateEntity("Walker");
			REQUIRE(ComponentAccess::AddComponent(character, "CharacterController", nullptr).has_value());
			CHECK_FALSE(ComponentAccess::AddComponent(character, "WheeledVehicle", nullptr).has_value());
			CHECK_FALSE(ComponentAccess::AddComponent(character, "RigidBody", nullptr).has_value());
			Entity buggy = scene.CreateEntity("Buggy");
			CHECK_FALSE(ComponentAccess::AddComponent(buggy, "WheeledVehicle", nullptr).has_value());
			REQUIRE(ComponentAccess::AddComponent(buggy, "RigidBody", nullptr).has_value());
			REQUIRE(ComponentAccess::AddComponent(buggy, "WheeledVehicle", nullptr).has_value());
			CHECK_FALSE(ComponentAccess::AddComponent(buggy, "CharacterController", nullptr).has_value());
			Entity loose = scene.CreateEntity("Loose");
			CHECK(ComponentAccess::AddComponent(loose, "VehicleWheel", nullptr).has_value());
		}

		TEST_CASE("VehiclesRegistration: every ranged field is bounded by the physics world's constants")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const auto length = static_cast<double>(MaxPhysicsVehicleLength);
			const auto torque = static_cast<double>(MaxPhysicsVehicleTorque);
			const auto rpm = static_cast<double>(MaxPhysicsVehicleRpm);
			const auto ratio = static_cast<double>(MaxPhysicsVehicleRatio);
			const auto rate = static_cast<double>(MaxPhysicsVehicleRate);
			const auto time = static_cast<double>(MaxPhysicsVehicleTime);
			const auto slip = static_cast<double>(MaxPhysicsVehicleSlipRatio);
			const auto minInertia = static_cast<double>(MinPhysicsVehicleInertia);
			const auto maxInertia = static_cast<double>(MaxPhysicsVehicleInertia);
			const auto gears = static_cast<double>(MaxPhysicsVehicleGears);
			const std::vector<VehicleBound> bounds = {
				{ "WheeledVehicle", "Throttle", -1.0, 1.0 },
				{ "WheeledVehicle", "Steering", -1.0, 1.0 },
				{ "WheeledVehicle", "Brake", 0.0, 1.0 },
				{ "WheeledVehicle", "HandBrake", 0.0, 1.0 },
				{ "WheeledVehicle", "Gear", -gears, gears },
				{ "WheeledVehicle", "Clutch", 0.0, 1.0 },
				{ "WheeledVehicle", "MaxTorque", 0.0, torque },
				{ "WheeledVehicle", "MinRpm", 0.0, rpm },
				{ "WheeledVehicle", "MaxRpm", 1.0, rpm },
				{ "WheeledVehicle", "EngineInertia", minInertia, maxInertia },
				{ "WheeledVehicle", "EngineDamping", 0.0, rate },
				{ "WheeledVehicle", "SwitchTime", 0.0, time },
				{ "WheeledVehicle", "ClutchReleaseTime", 0.0, time },
				{ "WheeledVehicle", "SwitchLatency", 0.0, time },
				{ "WheeledVehicle", "ShiftUpRpm", 0.0, rpm },
				{ "WheeledVehicle", "ShiftDownRpm", 0.0, rpm },
				{ "WheeledVehicle", "ClutchStrength", 0.0, rate },
				{ "WheeledVehicle", "DifferentialLimitedSlipRatio", 0.0, slip },
				{ "WheeledVehicle", "MaxPitchRollAngle", 0.0, 180.0 },
				{ "VehicleWheel", "Radius", static_cast<double>(MinPhysicsVehicleWheelRadius), length },
				{ "VehicleWheel", "Width", 0.0, length },
				{ "VehicleWheel", "SuspensionMinLength", 0.0, length },
				{ "VehicleWheel", "SuspensionMaxLength", 0.0, length },
				{ "VehicleWheel", "SuspensionPreload", 0.0, length },
				{ "VehicleWheel", "SpringFrequency", 0.0, rate },
				{ "VehicleWheel", "SpringDamping", 0.0, rate },
				{ "VehicleWheel", "MaxSteerAngle", 0.0, 90.0 },
				{ "VehicleWheel", "MaxBrakeTorque", 0.0, torque },
				{ "VehicleWheel", "MaxHandBrakeTorque", 0.0, torque },
				{ "VehicleWheel", "Inertia", minInertia, maxInertia },
				{ "VehicleWheel", "AngularDamping", 0.0, rate },
			};

			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity buggy = scene.CreateEntity("Buggy");
			REQUIRE(ComponentAccess::AddComponent(buggy, "RigidBody", nullptr).has_value());
			REQUIRE(ComponentAccess::AddComponent(buggy, "WheeledVehicle", nullptr).has_value());
			Entity wheel = scene.CreateEntity("Wheel", buggy);
			REQUIRE(ComponentAccess::AddComponent(wheel, "VehicleWheel", nullptr).has_value());
			for (const VehicleBound& bound : bounds)
			{
				CAPTURE(bound.Component);
				CAPTURE(bound.Field);
				const FieldMeta& meta = FindVehicleMeta(*registry, bound.Component, bound.Field);
				CHECK(meta.Min == bound.Min);
				CHECK(meta.Max == bound.Max);
				// Each bound is accepted and a value past it refused, on the component's write path.
				Entity& target = bound.Component == "WheeledVehicle" ? buggy : wheel;
				const bool integer = bound.Field == "Gear";
				const auto value = [integer](double number)
				{
					return integer ? Value::FromInt32(static_cast<int32_t>(number)) : Value::FromFloat(static_cast<float>(number));
				};
				CHECK(ComponentAccess::SetFieldValue(target, bound.Component, bound.Field, value(bound.Max)).has_value());
				CHECK(ComponentAccess::SetFieldValue(target, bound.Component, bound.Field, value(bound.Min)).has_value());
				const double past = integer ? bound.Max + 1.0 : bound.Max * 2.0 + 1.0;
				const Status refused = ComponentAccess::SetFieldValue(target, bound.Component, bound.Field, value(past));
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::Validation);
				const double below = integer ? bound.Min - 1.0 : bound.Min - 1.0 - std::abs(bound.Min);
				CHECK_FALSE(ComponentAccess::SetFieldValue(target, bound.Component, bound.Field, value(below)).has_value());
			}

			// The list elements' bounds.
			const std::vector<VehicleBound> elements = {
				{ "VehicleCurveKey", "X", -1000.0, 1000.0 },
				{ "VehicleCurveKey", "Y", 0.0, rate },
				{ "VehicleDifferential", "Ratio", 0.0, ratio },
				{ "VehicleDifferential", "LeftRightSplit", 0.0, 1.0 },
				{ "VehicleDifferential", "LimitedSlipRatio", 0.0, slip },
				{ "VehicleDifferential", "EngineTorqueRatio", 0.0, 1.0 },
				{ "VehicleAntiRollBar", "Stiffness", 0.0, static_cast<double>(MaxPhysicsVehicleBarStiffness) },
			};
			for (const VehicleBound& bound : elements)
			{
				CAPTURE(bound.Component);
				CAPTURE(bound.Field);
				const StructInfo* info = registry->FindStruct(bound.Component);
				REQUIRE(info != nullptr);
				const FieldInfo* field = info->FindField(bound.Field);
				REQUIRE(field != nullptr);
				CHECK(field->GetMeta().Min == bound.Min);
				CHECK(field->GetMeta().Max == bound.Max);
			}
		}

		TEST_CASE("VehiclesRegistration: the transmission and cast enums name their values")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			CHECK(EnumNames(*registry, "VehicleTransmissionMode") == std::vector<std::string>{ "Auto", "Manual" });
			CHECK(EnumNames(*registry, "VehicleWheelCast") == std::vector<std::string>{ "Ray", "Sphere", "Cylinder" });
		}
	}

}
