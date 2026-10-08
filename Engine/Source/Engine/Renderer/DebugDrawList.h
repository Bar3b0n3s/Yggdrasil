#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

// The debug draw list (Architecture §8.10): world-space primitives drawn as 1 px lines after tonemapping (DebugRenderer) and
// labels drawn as SDF text (TextRenderer), each with a colour, a duration in simulation seconds and a depth mode. It is
// plain CPU data in the RenderSnapshot (RenderSnapshot::DebugDraw), filled by scripts (Debug.*, M13), collider
// visualization (built from the components, so it works in edit mode, M11), light and camera gizmos (M9, M10) and the
// editor. This header is the second of the two Renderer headers Scene may include (§3 rule 3, the SnapshotHeaders rule of
// Scripts/ModuleRules.json): it includes nothing beyond Core, glm and the standard library, and no NVRHI.
//
// A list is a value type (copyable, movable, comparable), thread-compatible, deterministic: commands keep the order they
// were added in, and the renderer draws them in that order, so a deterministic producer gives an identical image. Values
// are not validated here (producers that take external input validate it, M13's bindings with located errors); the
// renderer skips a primitive with a non-finite value. Frozen by the M8 contract (Docs/Decisions/0013-m8-decisions.md
// decision 6) and implemented by it, because M11's collider visualization and M13's scripts emit into it.

namespace Engine {

	// How a primitive meets the scene's depth (§8.10).
	enum class DebugDepthMode : uint8_t
	{
		Tested, // hidden behind nearer scene geometry (SceneDepth, GreaterOrEqual)
		OnTop   // always visible
	};

	// A segment from From to To.
	struct DebugLine
	{
		glm::vec3 From = glm::vec3(0.0f);
		glm::vec3 To = glm::vec3(0.0f);

		bool operator==(const DebugLine&) const = default;
	};

	// A segment from Origin along Direction (any non-zero length; the drawer normalizes it) for Length metres.
	struct DebugRay
	{
		glm::vec3 Origin = glm::vec3(0.0f);
		glm::vec3 Direction = glm::vec3(0.0f, 0.0f, -1.0f);
		float Length = 1.0f;

		bool operator==(const DebugRay&) const = default;
	};

	// An oriented box: its 12 edges.
	struct DebugBox
	{
		glm::vec3 Center = glm::vec3(0.0f);
		glm::vec3 HalfExtents = glm::vec3(0.5f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // unit; identity: the box is axis-aligned

		bool operator==(const DebugBox&) const = default;
	};

	// A sphere: three great circles in the world XY, YZ and ZX planes.
	struct DebugSphere
	{
		glm::vec3 Center = glm::vec3(0.0f);
		float Radius = 0.5f;

		bool operator==(const DebugSphere&) const = default;
	};

	// A capsule around the segment Start-End (the centres of its two hemispheres; equal points draw a sphere).
	struct DebugCapsule
	{
		glm::vec3 Start = glm::vec3(0.0f, -0.5f, 0.0f);
		glm::vec3 End = glm::vec3(0.0f, 0.5f, 0.0f);
		float Radius = 0.5f;

		bool operator==(const DebugCapsule&) const = default;
	};

	// A segment from From to To with an arrowhead at To whose barbs are HeadSize metres long.
	struct DebugArrow
	{
		glm::vec3 From = glm::vec3(0.0f);
		glm::vec3 To = glm::vec3(0.0f, 0.0f, -1.0f);
		float HeadSize = 0.1f;

		bool operator==(const DebugArrow&) const = default;
	};

	// A view volume by its eight world-space corners: the near rectangle (indices 0-3) then the far rectangle (4-7), each in
	// the order (-x, -y), (+x, -y), (+x, +y), (-x, +y) of normalized device coordinates. Explicit corners rather than an
	// inverse view-projection, because a perspective camera's far plane is at infinity (§8.3); the gizmo that draws a camera
	// chooses the far distance it shows.
	struct DebugFrustum
	{
		std::array<glm::vec3, 8> Corners{};

		bool operator==(const DebugFrustum&) const = default;
	};

	// A label at a world position (§8.10 Text3D): screen-aligned SDF text in the Default font, centred on the projected
	// point, Size pixels per em at the 1080p reference height (scaled by viewport height / 1080 like screen text).
	struct DebugText
	{
		glm::vec3 Position = glm::vec3(0.0f);
		std::string Text{}; // UTF-8; glyphs the font lacks are skipped
		float Size = 16.0f;

		bool operator==(const DebugText&) const = default;
	};

	// One primitive (§8.10: Line, Ray, Box, Sphere, Capsule, Arrow, Frustum, Text3D).
	using DebugShape = std::variant<DebugLine, DebugRay, DebugBox, DebugSphere, DebugCapsule, DebugArrow, DebugFrustum, DebugText>;

	// One command of the list.
	struct DebugDrawCommand
	{
		DebugShape Shape{};
		glm::vec4 Color = glm::vec4(1.0f); // linear RGB and alpha (the renderer blends with straight alpha and encodes, §8.9)
		// Simulation seconds the command stays in the list (see DebugDrawList::Advance); 0 draws it in one extraction.
		float Duration = 0.0f;
		DebugDepthMode Depth = DebugDepthMode::Tested;
		// Simulation seconds left; starts at Duration and is only changed by Advance.
		float Remaining = 0.0f;

		bool operator==(const DebugDrawCommand&) const = default;
	};

	class DebugDrawList
	{
	public:
		// The most commands a list holds; Add beyond it drops the command and counts it (GetDroppedCount), so a runaway
		// producer cannot exhaust memory with the list (the drawing side bounds the tessellated vertices separately:
		// DebugRenderer.h's MaxDebugLineVertices).
		static constexpr size_t MaxCommands = 65536;

		DebugDrawList() = default;

		// Each appends one command with the given colour (linear RGBA), duration (simulation seconds; negative or non-finite
		// values are stored as 0) and depth mode. Values are stored as given otherwise (see the file comment).
		void AddLine(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color, float duration = 0.0f,
			DebugDepthMode depth = DebugDepthMode::Tested);
		void AddRay(const glm::vec3& origin, const glm::vec3& direction, float length, const glm::vec4& color, float duration = 0.0f,
			DebugDepthMode depth = DebugDepthMode::Tested);
		void AddBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, const glm::vec4& color,
			float duration = 0.0f, DebugDepthMode depth = DebugDepthMode::Tested);
		void AddSphere(const glm::vec3& center, float radius, const glm::vec4& color, float duration = 0.0f,
			DebugDepthMode depth = DebugDepthMode::Tested);
		void AddCapsule(const glm::vec3& start, const glm::vec3& end, float radius, const glm::vec4& color, float duration = 0.0f,
			DebugDepthMode depth = DebugDepthMode::Tested);
		void AddArrow(const glm::vec3& from, const glm::vec3& to, float headSize, const glm::vec4& color, float duration = 0.0f,
			DebugDepthMode depth = DebugDepthMode::Tested);
		void AddFrustum(const std::array<glm::vec3, 8>& corners, const glm::vec4& color, float duration = 0.0f,
			DebugDepthMode depth = DebugDepthMode::Tested);
		void AddText(const glm::vec3& position, std::string text, float size, const glm::vec4& color, float duration = 0.0f,
			DebugDepthMode depth = DebugDepthMode::Tested);

		// Appends `command` with Remaining set to its (sanitized) Duration.
		void Add(DebugDrawCommand command);

		// Appends every command of `other` in its order, keeping their Remaining times (an extraction merges the scene's
		// persistent list with the commands built for the view, M11's colliders among them). Commands past MaxCommands are
		// dropped and counted.
		void Append(const DebugDrawList& other);

		// Ages the list by `deltaSeconds` (>= 0 and finite, asserted) of simulation time: first removes every command whose
		// Remaining is <= 0, then subtracts `deltaSeconds` from the others. The owner of a persistent list calls it once per
		// simulation step before that step's producers add commands, so a command is extracted at least once and stays for
		// at least its Duration (a 0-duration command lives exactly until the next step). Keeps the order of the rest.
		void Advance(float deltaSeconds);

		// Removes every command and resets the dropped count.
		void Clear();

		[[nodiscard]] std::span<const DebugDrawCommand> GetCommands() const { return m_Commands; }
		[[nodiscard]] size_t GetSize() const { return m_Commands.size(); }
		[[nodiscard]] bool IsEmpty() const { return m_Commands.empty(); }
		// Commands Add and Append dropped at MaxCommands since construction or the last Clear.
		[[nodiscard]] size_t GetDroppedCount() const { return m_Dropped; }

		bool operator==(const DebugDrawList&) const = default;
	private:
		std::vector<DebugDrawCommand> m_Commands;
		size_t m_Dropped = 0;
	};

}
