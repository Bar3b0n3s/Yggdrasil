#pragma once

#include "Engine/Native/NativeApi.h"

// Turns its entity about Axis while the Spin action is held: AngleDegrees advances by DegreesPerSecond per second of
// simulation time, wraps into [0, 360) and becomes the entity's rotation; SpinTicks counts the ticks it turned.
struct Spinner
{
	static constexpr std::string_view NativeTypeName = "Spinner";

	float DegreesPerSecond = 90.0f;
	glm::vec3 Axis = glm::vec3(0.0f, 1.0f, 0.0f);
	float AngleDegrees = 0.0f;
	uint32_t SpinTicks = 0;
};

// Counts the presses of the Bump action and raises its entity to BaseHeight + (Presses % Levels) * StepHeight, so it
// climbs one step per press and drops back to BaseHeight after Levels presses.
struct Bumper
{
	static constexpr std::string_view NativeTypeName = "Bumper";

	uint32_t Presses = 0;
	uint32_t Levels = 4;
	float BaseHeight = 0.5f;
	float StepHeight = 0.5f;
};

// The play session's totals: the ticks Spin was held and the presses of Bump. A resource, so the state hash's native
// block covers it.
struct Totals
{
	static constexpr std::string_view NativeTypeName = "Totals";

	uint32_t SpinTicks = 0;
	uint32_t Presses = 0;
};
