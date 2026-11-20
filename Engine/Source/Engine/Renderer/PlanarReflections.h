#pragma once

#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The planar reflections' CPU part (M42, Docs/Decisions/0059-m42-planar-reflections.md decision 3; Architecture §8.3,
// "Planar reflections"): what a view of a RenderSnapshot needs to render each PlanarReflectionItem as a mirror view. Pure
// functions of their arguments, thread-safe, no GPU state.
//   - The plane. A reflection's world plane is its item's local plane (the point Normal * Offset, the normal Normal,
//     normalized) taken through World, the normal through World's inverse transpose, so a non-uniform scale is handled
//     (ComputePlanarReflectionPlane). Its positive side is the reflecting side.
//   - The mirror camera. With the plane's reflection R (ComputePlanarReflectionMatrix) and the view's camera V, a mirror
//     view renders with View = S * V * R, where S flips view-space X (diag(-1, 1, 1, 1)): V * R alone has a negative
//     determinant, which would turn every draw's winding and face test, while S * V * R is rigid, so every pipeline and face
//     rule works unchanged and the stored image is the reflection mirrored left for right. Its position is R times the
//     camera's; its projection is the camera's own (unjittered: CameraData holds no jitter) for the same aspect, its
//     viewport the reflection's size; it has no history and no jitter (MakePlanarReflectionCamera). The main view samples
//     the reflection at a shaded point's projection by the mirror camera's view-projection, which on the plane is the main
//     view's pixel mirrored, exact under TAA's jitter and the scissor.
//   - The clip. A mirror view's vertex shaders clip at ClipPlane (ViewConstants::ClipPlane, SV_ClipDistance0): the plane
//     moved PlanarReflectionClipBias towards its reflecting side, so neither the glass nor its backing shows
//     (ComputePlanarReflectionClipPlane). Every other view's ClipPlane is (0, 0, 0, 1).
//   - The tests a reflection passes to be drawn: in view (a draw of its entity survived the main view's culling: the
//     renderer's), facing (the camera on the reflecting side: IsPlanarReflectionFacing), and drawable (a Lit view outside the
//     wireframe with a camera, SSR's condition: the renderer's). The candidates rank by the screen area of their drawn glass,
//     largest first, ties by entity UUID, so captures are reproducible; the first MaxPlanarReflectionsPerView are drawn, the
//     rest dropped (RankPlanarReflections).
//   - The cull. A mirror view draws the snapshot's mesh items but the reflection's own entity, culled against the mirror
//     camera with its projection cropped to the glass's screen rectangle (MakePlanarReflectionCullCamera), and against the
//     clip plane (IsBehindPlanarReflectionClipPlane).
//   - The directional cascade. A mirror view chooses the main view's cascade by the main camera's view depth of the point,
//     dot(ComputeCascadeDepthPlane(camera), (p, 1)) (ViewConstants::CascadeDepthPlane).

namespace Engine {

	// The most planar reflections one view draws (an engine constant; each renderer applies it on its own: the editor's Scene
	// and Game views and each capture).
	inline constexpr uint32_t MaxPlanarReflectionsPerView = 4;
	// How far a mirror view's clip plane lies from the mirror's plane towards its reflecting side, in metres, so that neither
	// the glass nor its backing shows in the reflection; geometry this close to the plane is clipped (Backlog §6).
	inline constexpr float PlanarReflectionClipBias = 0.01f;

	// A world plane: the unit Normal points out of the reflecting side, and dot(Normal, p) + Distance is the signed distance
	// of the point p from it.
	struct PlanarReflectionPlane
	{
		glm::vec3 Normal = glm::vec3(0.0f, 0.0f, 1.0f);
		float Distance = 0.0f;

		// (Normal, Distance), the form of ViewConstants' planes.
		[[nodiscard]] glm::vec4 ToVector() const { return glm::vec4(Normal, Distance); }
		[[nodiscard]] float SignedDistance(const glm::vec3& point) const { return glm::dot(Normal, point) + Distance; }

		bool operator==(const PlanarReflectionPlane&) const = default;
	};

	// A screen rectangle in a view's normalized device coordinates, each bound within [-1, 1] and Min below Max on both
	// axes (+X right, +Y up).
	struct PlanarReflectionScreenRect
	{
		glm::vec2 Min = glm::vec2(-1.0f);
		glm::vec2 Max = glm::vec2(1.0f);

		// Its area in pixels of a viewport of `width` x `height`.
		[[nodiscard]] float GetPixelArea(uint32_t width, uint32_t height) const;

		bool operator==(const PlanarReflectionScreenRect&) const = default;
	};

	// The world plane of `item`: the local point normalize(Normal) * Offset and the local normal Normal taken through World
	// (the normal through its inverse transpose, normalized), computed in double precision. Empty when Normal is zero or not
	// finite, Offset or World is not finite, or World's upper 3 x 3 is singular: such a reflection is never drawn.
	[[nodiscard]] std::optional<PlanarReflectionPlane> ComputePlanarReflectionPlane(const PlanarReflectionItem& item);

	// The reflection R in `plane`: p -> p - 2 * plane.SignedDistance(p) * Normal, an involution (R * R = I) whose upper 3 x 3
	// has the determinant -1.
	[[nodiscard]] glm::mat4 ComputePlanarReflectionMatrix(const PlanarReflectionPlane& plane);

	// A mirror view's clip plane: `plane` moved PlanarReflectionClipBias towards its reflecting side, (Normal, Distance -
	// PlanarReflectionClipBias). A point is kept where dot(clipPlane, (p, 1)) >= 0.
	[[nodiscard]] glm::vec4 ComputePlanarReflectionClipPlane(const PlanarReflectionPlane& plane);

	// Whether `camera` sees `plane` from its reflecting side: for a perspective camera its Position lies on the positive side
	// (a camera on the plane does not); for an orthographic one its view direction (the camera's local -Z, from View) points
	// against Normal.
	[[nodiscard]] bool IsPlanarReflectionFacing(const CameraData& camera, const PlanarReflectionPlane& plane);

	// The mirror camera of `camera` in `plane` for a reflection of `width` x `height` (both >= 1, asserted): View = S *
	// camera.View * R, Position = R * camera.Position, camera's projection, kind, field of view, size, clip range, clear
	// colour and entity, the viewport `width` x `height`, and no history, jitter or delta time.
	[[nodiscard]] CameraData MakePlanarReflectionCamera(const CameraData& camera, const PlanarReflectionPlane& plane, uint32_t width, uint32_t height);

	// The size of a reflection of the scene resolution `sceneWidth` x `sceneHeight` at `resolutionScale` (clamped to
	// [MinPlanarReflectionResolutionScale, MaxPlanarReflectionResolutionScale]; a non-finite scale takes the default):
	// ceil(size * scale) on each axis, at least 1.
	[[nodiscard]] glm::uvec2 ComputePlanarReflectionSize(uint32_t sceneWidth, uint32_t sceneHeight, float resolutionScale);

	// The screen rectangle of `bounds` (local space) at `world` in `camera`'s view, clipped to the view: the bounds of its
	// eight corners' projections. A box that reaches behind a perspective camera's near plane takes the whole view (a
	// conservative rectangle). Empty for empty bounds, a box outside the view on one side, or a rectangle of zero area.
	[[nodiscard]] std::optional<PlanarReflectionScreenRect> ComputePlanarReflectionScreenRect(const Aabb& bounds, const glm::mat4& world,
		const CameraData& camera);

	// The camera a mirror view culls with: `reflectionCamera` (MakePlanarReflectionCamera's) with its projection cropped to
	// `mainRect`, the glass's screen rectangle in the main view (ComputePlanarReflectionScreenRect), mirrored into the
	// mirror camera's coordinates (X negated), so IsOutsideView keeps exactly the items seen through the glass's rectangle.
	[[nodiscard]] CameraData MakePlanarReflectionCullCamera(const CameraData& reflectionCamera, const PlanarReflectionScreenRect& mainRect);

	// `rect` (the main view's) mirrored into the mirror camera's coordinates: X negated, the bounds swapped.
	[[nodiscard]] PlanarReflectionScreenRect MirrorPlanarReflectionScreenRect(const PlanarReflectionScreenRect& rect);

	// Whether every corner of `bounds` (local space) at `world` lies behind `clipPlane` (dot(clipPlane, (p, 1)) < 0), so a
	// mirror view clips all of it. Empty bounds are never behind.
	[[nodiscard]] bool IsBehindPlanarReflectionClipPlane(const Aabb& bounds, const glm::mat4& world, const glm::vec4& clipPlane);

	// The plane whose dot product with (p, 1) is `camera`'s view depth of the world point p (-(View * p).z), by which a
	// mirror view chooses the main view's directional shadow cascade.
	[[nodiscard]] glm::vec4 ComputeCascadeDepthPlane(const CameraData& camera);

	// One reflection that passed the tests to be drawn (in view, facing, drawable).
	struct PlanarReflectionCandidate
	{
		size_t Item = 0; // its index in RenderSnapshot::PlanarReflections
		UUID Entity{};
		float ScreenArea = 0.0f; // the screen area of its drawn glass, in scene pixels (PlanarReflectionScreenRect::GetPixelArea)
	};

	// The candidates a view draws and drops.
	struct PlanarReflectionSelection
	{
		std::vector<size_t> Slots{}; // the drawn candidates' Item, slot k's at index k: by ScreenArea, largest first, ties by Entity
		uint32_t Dropped = 0;        // the candidates past `cap`
	};

	// Ranks `candidates` by ScreenArea, largest first, ties by ascending Entity (then Item), and keeps the first `cap` (at most
	// MaxPlanarReflectionsPerView, asserted). A non-finite area ranks last.
	[[nodiscard]] PlanarReflectionSelection RankPlanarReflections(std::span<const PlanarReflectionCandidate> candidates,
		uint32_t cap = MaxPlanarReflectionsPerView);

}
