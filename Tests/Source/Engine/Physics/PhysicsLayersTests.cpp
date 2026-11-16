#include "TestsPCH.h"

#include "Engine/Physics/PhysicsLayers.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Physics layers and the collision filters (Architecture §9.2 "Layers"; §9.7 "layer matrix (incl. no Sensor-Static /
// Sensor-Sensor pairs)"): the object-layer encoding, the kind rule, and the project's layer table with its matrix and masks.

namespace Engine {

	namespace {

		// Default, Ball, Track and Trigger; Ball collides with Track and Trigger, Default with Default and Ball.
		Result<PhysicsLayerTable> MakeRollingBallTable()
		{
			const std::vector<std::string> layers = { "Default", "Ball", "Track", "Trigger" };
			const std::vector<std::vector<std::string>> collisions = { { "Default", "Default" }, { "Default", "Ball" }, { "Ball", "Track" },
				{ "Trigger", "Ball" } };
			return PhysicsLayerTable::Create(layers, collisions);
		}

	}

	TEST_SUITE("Physics")
	{
		TEST_CASE("PhysicsLayers: object layers encode the layer index and the kind, and kinds pick broad-phase layers")
		{
			for (uint32_t layer = 0; layer < MaxPhysicsLayers; ++layer)
			{
				CAPTURE(layer);
				for (const PhysicsObjectKind kind :
					{ PhysicsObjectKind::Static, PhysicsObjectKind::Moving, PhysicsObjectKind::Sensor, PhysicsObjectKind::Character })
				{
					const PhysicsObjectLayer objectLayer = MakePhysicsObjectLayer(layer, kind);
					CHECK(objectLayer == ((layer << 2) | std::to_underlying(kind)));
					CHECK(GetPhysicsLayerIndex(objectLayer) == layer);
					CHECK(GetPhysicsObjectKind(objectLayer) == kind);
				}
			}
			CHECK(GetPhysicsBroadPhaseLayer(MakePhysicsObjectLayer(3, PhysicsObjectKind::Static)) == PhysicsBroadPhaseLayer::NonMoving);
			CHECK(GetPhysicsBroadPhaseLayer(MakePhysicsObjectLayer(3, PhysicsObjectKind::Moving)) == PhysicsBroadPhaseLayer::Moving);
			CHECK(GetPhysicsBroadPhaseLayer(MakePhysicsObjectLayer(3, PhysicsObjectKind::Sensor)) == PhysicsBroadPhaseLayer::Sensor);
			// M34: a character's inner body moves, so it lives in the Moving tree, where sensors find it.
			CHECK(GetPhysicsBroadPhaseLayer(MakePhysicsObjectLayer(3, PhysicsObjectKind::Character)) == PhysicsBroadPhaseLayer::Moving);
			// The largest object layer fits Jolt's 16-bit ObjectLayer.
			CHECK(MakePhysicsObjectLayer(MaxPhysicsLayers - 1, PhysicsObjectKind::Character) < 0xffffu);
		}

		TEST_CASE("PhysicsLayers: the kind rule excludes Static-Static, Sensor-Static and Sensor-Sensor pairs")
		{
			using enum PhysicsObjectKind;
			CHECK_FALSE(PhysicsObjectKindsCollide(Static, Static));
			CHECK_FALSE(PhysicsObjectKindsCollide(Sensor, Static));
			CHECK_FALSE(PhysicsObjectKindsCollide(Static, Sensor));
			CHECK_FALSE(PhysicsObjectKindsCollide(Sensor, Sensor));
			CHECK(PhysicsObjectKindsCollide(Static, Moving));
			CHECK(PhysicsObjectKindsCollide(Moving, Static));
			CHECK(PhysicsObjectKindsCollide(Moving, Moving));
			CHECK(PhysicsObjectKindsCollide(Sensor, Moving));
			CHECK(PhysicsObjectKindsCollide(Moving, Sensor));
		}

		// M34 (Docs/Decisions/0049-m34-characters-input.md decision 1): a character's inner body pairs with sensors only, so
		// the solver never resolves its contact with a Dynamic (Moving) body.
		TEST_CASE("PhysicsLayers: the kind rule pairs a Character with Sensors only")
		{
			using enum PhysicsObjectKind;
			CHECK(PhysicsObjectKindsCollide(Character, Sensor));
			CHECK(PhysicsObjectKindsCollide(Sensor, Character));
			for (const PhysicsObjectKind other : { Static, Moving, Character })
			{
				CAPTURE(std::to_underlying(other));
				CHECK_FALSE(PhysicsObjectKindsCollide(Character, other));
				CHECK_FALSE(PhysicsObjectKindsCollide(other, Character));
			}

			Result<PhysicsLayerTable> table = MakeRollingBallTable();
			REQUIRE_MESSAGE(table.has_value(), table.error().ToString());
			const uint32_t ball = 1;
			const uint32_t track = 2;
			const uint32_t trigger = 3;
			// A character on the Ball layer: the trigger sees it, the ball's Dynamic bodies and the track do not pair with it.
			CHECK(table->ShouldCollide(MakePhysicsObjectLayer(trigger, Sensor), MakePhysicsObjectLayer(ball, Character)));
			CHECK_FALSE(table->ShouldCollide(MakePhysicsObjectLayer(ball, Moving), MakePhysicsObjectLayer(ball, Character)));
			CHECK_FALSE(table->ShouldCollide(MakePhysicsObjectLayer(track, Static), MakePhysicsObjectLayer(ball, Character)));
			CHECK_FALSE(table->ShouldCollide(MakePhysicsObjectLayer(ball, Character), MakePhysicsObjectLayer(ball, Character)));
			// The matrix still applies: a Default trigger does not see a character on the Track layer.
			CHECK_FALSE(table->ShouldCollide(MakePhysicsObjectLayer(0, Sensor), MakePhysicsObjectLayer(track, Character)));
			// Object layer against broad-phase layer: only the Sensor tree.
			CHECK(table->ShouldCollide(MakePhysicsObjectLayer(ball, Character), PhysicsBroadPhaseLayer::Sensor));
			CHECK_FALSE(table->ShouldCollide(MakePhysicsObjectLayer(ball, Character), PhysicsBroadPhaseLayer::Moving));
			CHECK_FALSE(table->ShouldCollide(MakePhysicsObjectLayer(ball, Character), PhysicsBroadPhaseLayer::NonMoving));
		}

		TEST_CASE("PhysicsLayerTable: the default table is the Default layer colliding with itself")
		{
			const PhysicsLayerTable table;
			CHECK(table.GetLayerCount() == 1);
			CHECK(table.GetName(0) == "Default");
			CHECK(table.FindLayer("Default") == std::optional<uint32_t>(0));
			CHECK(table.LayersCollide(0, 0));
		}

		TEST_CASE("PhysicsLayerTable: the matrix is symmetric and the pair filter applies the kind rule")
		{
			Result<PhysicsLayerTable> table = MakeRollingBallTable();
			REQUIRE_MESSAGE(table.has_value(), table.error().ToString());
			const uint32_t ball = table->FindLayer("Ball").value_or(99);
			const uint32_t track = table->FindLayer("Track").value_or(99);
			const uint32_t trigger = table->FindLayer("Trigger").value_or(99);
			REQUIRE(ball == 1);
			REQUIRE(track == 2);
			REQUIRE(trigger == 3);
			CHECK(table->LayersCollide(ball, track));
			CHECK(table->LayersCollide(track, ball));
			CHECK(table->LayersCollide(trigger, ball));
			CHECK_FALSE(table->LayersCollide(track, track));
			CHECK_FALSE(table->LayersCollide(track, trigger));

			using enum PhysicsObjectKind;
			// A moving ball against static track and a trigger sensor: both pairs exist.
			CHECK(table->ShouldCollide(MakePhysicsObjectLayer(ball, Moving), MakePhysicsObjectLayer(track, Static)));
			CHECK(table->ShouldCollide(MakePhysicsObjectLayer(trigger, Sensor), MakePhysicsObjectLayer(ball, Moving)));
			// Sensors never pair with static geometry or other sensors, whatever the matrix says (§9.2).
			CHECK_FALSE(table->ShouldCollide(MakePhysicsObjectLayer(ball, Sensor), MakePhysicsObjectLayer(track, Static)));
			CHECK_FALSE(table->ShouldCollide(MakePhysicsObjectLayer(trigger, Sensor), MakePhysicsObjectLayer(ball, Sensor)));
			CHECK_FALSE(table->ShouldCollide(MakePhysicsObjectLayer(0, Static), MakePhysicsObjectLayer(0, Static)));
			// Object layer against broad-phase layer.
			CHECK(table->ShouldCollide(MakePhysicsObjectLayer(ball, Moving), PhysicsBroadPhaseLayer::NonMoving));
			CHECK(table->ShouldCollide(MakePhysicsObjectLayer(ball, Moving), PhysicsBroadPhaseLayer::Sensor));
			CHECK_FALSE(table->ShouldCollide(MakePhysicsObjectLayer(trigger, Sensor), PhysicsBroadPhaseLayer::NonMoving));
			CHECK_FALSE(table->ShouldCollide(MakePhysicsObjectLayer(trigger, Sensor), PhysicsBroadPhaseLayer::Sensor));
			CHECK_FALSE(table->ShouldCollide(MakePhysicsObjectLayer(track, Static), PhysicsBroadPhaseLayer::NonMoving));
		}

		TEST_CASE("PhysicsLayerTable: Create applies the project settings' rules with located errors")
		{
			// The JSON pointer of a refusal, or what went wrong instead; no assertion inside, so it can sit in a CHECK.
			const auto pointerOf = [](std::vector<std::string> layers, std::vector<std::vector<std::string>> collisions) -> std::string
			{
				Result<PhysicsLayerTable> table = PhysicsLayerTable::Create(layers, collisions);
				if (table.has_value())
					return "<created>";
				if (table.error().GetCode() != ErrorCode::Validation)
					return std::format("<{}>", table.error().GetCode());
				return table.error().GetLocation().JsonPointer.value_or("<none>");
			};
			CHECK(pointerOf({}, {}) == "/Layers");
			CHECK(pointerOf({ "Ball" }, {}) == "/Layers/0");
			CHECK(pointerOf({ "Default", "Ball", "Ball" }, {}) == "/Layers/2");
			CHECK(pointerOf({ "Default", "" }, {}) == "/Layers/1");
			CHECK(pointerOf({ "Default" }, { { "Default" } }) == "/Collisions/0");
			CHECK(pointerOf({ "Default" }, { { "Default", "Track" } }) == "/Collisions/0/1");
			std::vector<std::string> tooMany = { "Default" };
			for (uint32_t layer = 1; layer <= MaxPhysicsLayers; ++layer)
				tooMany.push_back("Layer" + std::to_string(layer));
			CHECK(pointerOf(tooMany, {}) == "/Layers");
			// Exactly MaxPhysicsLayers are accepted.
			tooMany.pop_back();
			CHECK(PhysicsLayerTable::Create(tooMany, {}).has_value());
		}

		TEST_CASE("PhysicsLayerTable: MakeMask selects the named layers and suggests a name for a typo")
		{
			Result<PhysicsLayerTable> table = MakeRollingBallTable();
			REQUIRE(table.has_value());
			const std::array<std::string_view, 2> names = { "Track", "Default" };
			const Result<PhysicsLayerMask> mask = table->MakeMask(names);
			REQUIRE(mask.has_value());
			CHECK(*mask == 0b0101u);
			CHECK(PhysicsLayerTable::MaskContains(*mask, 0));
			CHECK_FALSE(PhysicsLayerTable::MaskContains(*mask, 1));
			CHECK(PhysicsLayerTable::MaskContains(*mask, 2));
			CHECK(table->MakeMask({}).value_or(99) == 0u);
			const std::array<std::string_view, 1> typo = { "Trak" };
			const Result<PhysicsLayerMask> unknown = table->MakeMask(typo);
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::NotFound);
			CHECK(unknown.error().GetHint().contains("Track"));
		}
	}

}
