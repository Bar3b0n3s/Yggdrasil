#include "TestsPCH.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string_view>
#include <vector>

namespace Engine {

	static void CheckComponentScript(Test::ScriptTestFixture& fixture, std::string_view source)
	{
		const auto result = fixture.Evaluate(source);
		REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
		CHECK(result->Value.Get() == Json(true));
	}

	static void TransformCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
	{
		Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
		REQUIRE(fixture.Start());
		CheckComponentScript(fixture, R"(
local parent = Scene.CreateEntity("Parent")
parent.Transform.Translation = vector.create(10, 0, 0)
local child = Scene.CreateEntity("Child", parent)
local t = child.Transform
t.Translation = vector.create(2, 0, 0)
assert(t.WorldPosition == vector.create(12, 0, 0))
t.WorldPosition = vector.create(15, 0, 0)
assert(t.Translation == vector.create(5, 0, 0))
assert(t:TransformPoint(vector.zero) == t.WorldPosition)
assert(t:InverseTransformPoint(t.WorldPosition) == vector.zero)
assert(t:Forward() == vector.create(0, 0, -1))
assert(t:Right() == vector.create(1, 0, 0))
assert(t:Up() == vector.create(0, 1, 0))
t:Rotate(vector.create(0, 90, 0), "World")
assert(vector.magnitude(t:Forward() - vector.create(-1, 0, 0)) < 0.00001)
t:Translate(vector.create(0, 0, -2), "local")
t:Translate(vector.create(1, 0, 0), "World")
local world = t.WorldPosition
t:Translate(vector.zero)
assert(t.WorldPosition == world)
t:Rotate(vector.zero, "Local")
t:Rotate(vector.zero)
t:LookAt(t.WorldPosition + vector.create(0, 0, -1))
assert(vector.magnitude(t:TransformDirection(vector.create(0, 0, -1)) - t:Forward()) < 0.00001)
assert(not pcall(function() t:LookAt(t.WorldPosition) end))
t:Teleport(vector.create(30, 1, 2), Quat.Identity())
assert(t.WorldPosition == vector.create(30, 1, 2))
assert(t.RenderPosition == t.WorldPosition)
assert(not pcall(function() t.WorldScale = vector.one end))
return true
)");
		REQUIRE(fixture.Teleported.size() == 1);
		CHECK(fixture.Teleported[0] == fixture.GetScene().FindEntityByID(fixture.Teleported[0]).GetUUID());
		const auto coverage = fixture.GetApi().GetCoverage(mode);
		REQUIRE(coverage);
		snapshots.push_back(*coverage);
	}

	static void MaterialCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
	{
		Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
		const auto entity = fixture.GetScene().CreateEntity("Mesh");
		MeshRendererComponent renderer;
		renderer.Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
		entity.AddComponent<MeshRendererComponent>(renderer);
		REQUIRE(fixture.Start());
		CheckComponentScript(fixture, R"(
local mesh = Scene.FindByName("Mesh").MeshRenderer
local default = Assets.Load("engine://Materials/Default")
local replacement = Assets.Load("engine://Materials/Error")
assert(Assets.GetPath(mesh:GetMaterial(1)) == Assets.GetPath(default))
mesh:SetMaterial(1, replacement)
assert(Assets.GetPath(mesh:GetMaterial(1)) == Assets.GetPath(replacement))
mesh:SetMaterial(1, nil)
assert(Assets.GetPath(mesh:GetMaterial(1)) == Assets.GetPath(default))
for _, index in {0, -1, 1.5, 2, math.huge} do
assert(not pcall(function() mesh:GetMaterial(index) end))
assert(not pcall(function() mesh:SetMaterial(index, replacement) end))
end
return true
)");
		REQUIRE(entity.GetComponent<MeshRendererComponent>().Materials.size() == 1);
		CHECK_FALSE(entity.GetComponent<MeshRendererComponent>().Materials[0].GetHandle().IsValid());
		const auto coverage = fixture.GetApi().GetCoverage(mode);
		REQUIRE(coverage);
		snapshots.push_back(*coverage);
	}

	// Camera:ScreenToWorldRay, ScreenToWorldRayFrom (M34) and Camera:WorldToScreen in view coordinates (a 100 x 100 view),
	// in both projections.
	static void CameraCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
	{
		Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
		fixture.Environment.WindowSize = { 100, 100 };
		REQUIRE(fixture.Start());
		CheckComponentScript(fixture, R"(
local parent = Scene.CreateEntity("Viewer")
local camera = parent:AddComponent("Camera")
local origin, direction = camera:ScreenToWorldRay(49.5,49.5)
assert(origin == vector.zero and vector.magnitude(direction - vector.create(0,0,-1)) < 0.00001)
local screen = camera:WorldToScreen(vector.create(0,0,-5))
assert(math.abs(screen.x-49.5) < 0.00001 and math.abs(screen.y-49.5) < 0.00001)
assert(math.abs(screen.z-camera.NearClip/5) < 0.00001)
camera.Projection = "Orthographic"
origin, direction = camera:ScreenToWorldRay(49.5,49.5)
assert(origin == vector.create(0,0,-camera.NearClip) and direction == vector.create(0,0,-1))
screen = camera:WorldToScreen(vector.create(0,0,-5))
assert(math.abs(screen.z - (camera.FarClip-5)/(camera.FarClip-camera.NearClip)) < 0.00001)
origin, direction = camera:ScreenToWorldRayFrom(49.5,49.5,vector.create(1,2,3),Quat.Identity())
assert(origin == vector.create(1,2,3-camera.NearClip) and direction == vector.create(0,0,-1))
return true
)");
		const auto coverage = fixture.GetApi().GetCoverage(mode);
		REQUIRE(coverage);
		snapshots.push_back(*coverage);
	}

	namespace Test {

		void RunComponentsBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			TransformCoverage(mode, snapshots);
			MaterialCoverage(mode, snapshots);
			CameraCoverage(mode, snapshots);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ComponentBindings: Transform fast paths preserve reflection and interpolation semantics")
		{
			std::vector<ScriptApiCoverage> snapshots;
			TransformCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("ComponentBindings: material slots resolve mesh defaults and expand overrides")
		{
			std::vector<ScriptApiCoverage> snapshots;
			MaterialCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("ComponentBindings: camera rays and screen points follow the view in both projections")
		{
			std::vector<ScriptApiCoverage> snapshots;
			CameraCoverage(RunModes::Editor, snapshots);
		}

		// M34 (Docs/Decisions/0049-m34-characters-input.md decision 8; World Tree's brief 10): ScreenToWorldRayFrom projects
		// as ScreenToWorldRay does, from the pose it is given instead of the rendered one, so gameplay can aim in the fixed
		// step from a pose it keeps there. The camera entity stays at the origin; the given pose looks down from (0, 10, 10)
		// at 45 degrees, so the view's centre ray meets the ground y = 0 at (0, 0, 0).
		TEST_CASE("ComponentBindings: ScreenToWorldRayFrom builds the ray from the pose it is given, not the rendered one")
		{
			Test::ScriptTestFixture fixture({ .TestMode = true });
			fixture.Environment.WindowSize = { 100, 100 };
			REQUIRE(fixture.Start());
			CheckComponentScript(fixture, R"(
local viewer = Scene.CreateEntity("Viewer")
local camera = viewer:AddComponent("Camera")
local position = vector.create(0, 10, 10)
local rotation = Quat.FromEuler(-45, 0, 0)
local origin, direction = camera:ScreenToWorldRayFrom(49.5, 49.5, position, rotation)
assert(origin == position)
assert(vector.magnitude(direction - rotation:Rotate(vector.create(0, 0, -1))) < 0.00001)
local ground = origin + direction * (origin.y / -direction.y)
assert(vector.magnitude(ground) < 0.0001)
-- The rendered pose is the entity's (the origin), so ScreenToWorldRay differs, and equals the call from that pose.
local renderedOrigin, renderedDirection = camera:ScreenToWorldRay(10, 80)
local fromOrigin, fromDirection = camera:ScreenToWorldRayFrom(10, 80, viewer.Transform.RenderPosition, viewer.Transform.RenderRotation)
assert(renderedOrigin == fromOrigin and renderedDirection == fromDirection)
assert(renderedOrigin ~= camera:ScreenToWorldRayFrom(10, 80, position, rotation))
-- An unnormalized rotation is normalized; a position that is not finite and a pixel outside the view are errors.
local _, scaled = camera:ScreenToWorldRayFrom(49.5, 49.5, position, Quat.New(rotation.x * 2, rotation.y * 2, rotation.z * 2, rotation.w * 2))
assert(vector.magnitude(scaled - direction) < 0.00001)
assert(not pcall(camera.ScreenToWorldRayFrom, camera, 49.5, 49.5, vector.create(math.huge, 0, 0), rotation))
assert(not pcall(camera.ScreenToWorldRayFrom, camera, 100, 49.5, position, rotation))
return true
)");
		}

		// M21 (Docs/Decisions/0032-m21-contract.md decision 6, stream C): under a PixelPerfectCamera, Camera:ScreenToWorldRay
		// and WorldToScreen go through the output mapping, so a view pixel maps to the scene pixel the image shows there. A
		// 320 x 180 reference at 16 pixels per unit in a 1000 x 540 view is upscaled by 3 and letterboxed by 20 pixels left
		// and right (ComputeRenderOutputMapping), orthographic at a half height of 180 / 32 whatever the Camera's own
		// (perspective) projection. The camera at (0.3, -0.2, 10) snaps to (0.3125, -0.1875, 10), so the view's left edge is
		// x = -9.6875 and its top y = 5.4375; view pixel (69, 49), whose centre (69.5, 49.5) shows the centre of scene pixel
		// (16, 16), looks at (-9.6875 + 16.5 / 16, 5.4375 - 16.5 / 16) = (-8.65625, 4.40625). A pixel of the left letterbox
		// maps outside the scene image, beside its left edge: view pixel 5's centre is scene x (5.5 - 20) / 3 = -14.5 / 3.
		TEST_CASE("ComponentBindings: under a PixelPerfectCamera, ScreenToWorldRay and WorldToScreen map view pixels through the output mapping")
		{
			Test::ScriptTestFixture fixture({ .TestMode = true });
			fixture.Environment.WindowSize = { 1000, 540 };
			REQUIRE(fixture.Start());
			CheckComponentScript(fixture, R"(
local viewer = Scene.CreateEntity("Viewer")
viewer.Transform.Translation = vector.create(0.3, -0.2, 10)
local camera = viewer:AddComponent("Camera")
local pixelPerfect = viewer:AddComponent("PixelPerfectCamera")
pixelPerfect.ReferenceWidth = 320
pixelPerfect.ReferenceHeight = 180
pixelPerfect.PixelsPerUnit = 16
local origin, direction = camera:ScreenToWorldRay(69, 49)
assert(vector.magnitude(origin - vector.create(-8.65625, 4.40625, 10 - camera.NearClip)) < 0.0001)
assert(direction == vector.create(0, 0, -1))
local screen = camera:WorldToScreen(vector.create(-8.65625, 4.40625, 0))
assert(math.abs(screen.x - 69) < 0.0001 and math.abs(screen.y - 49) < 0.0001)
local letterbox = camera:ScreenToWorldRay(5, 49)
assert(math.abs(letterbox.x - (-9.6875 - 14.5 / 48)) < 0.0001)
-- M34: ScreenToWorldRayFrom snaps the position it is given the same way.
local from = camera:ScreenToWorldRayFrom(69, 49, vector.create(0.3, -0.2, 10), Quat.Identity())
assert(vector.magnitude(from - origin) < 0.0001)
return true
)");
		}
	}

}
