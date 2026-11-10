--!strict
-- Shared helpers of the animation suites (Animation.scene; M23, Docs/Decisions/0034-m23-contract.md decision 33).
local Animation = {}

-- The Wave clip of Skinned.gltf (one second; the import settings' events Start, Peak and End) and the Walk clip of
-- SkinnedRigs.gltf (one second; its hips move 1 m along the rig's +Z).
Animation.WAVE = "Assets/Models/Animation/Skinned.gltf#animation:0:Wave"
Animation.WALK = "Assets/Models/Animation/SkinnedRigs.gltf#animation:0:Walk"

-- The entity at `path` in the running scene; a missing one fails the calling case.
function Animation.Find(path: string): Entity
	local entity = Scene.FindByPath(path)
	if entity == nil then
		error(path .. " is not in the scene", 2)
	end
	return entity
end

-- The Animator of the entity at `path`.
function Animation.Animator(path: string): Animator
	local animator = Animation.Find(path).Animator
	if animator == nil then
		error(path .. " has no Animator", 2)
	end
	return animator
end

-- The asset at `reference`, which must load.
function Animation.Load(reference: string): AssetRef
	local asset = Assets.Load(reference)
	if asset == nil then
		error(reference .. " does not load", 2)
	end
	return asset
end

return Animation
