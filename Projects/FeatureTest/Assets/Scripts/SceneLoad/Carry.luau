--!strict
-- Module state of one scene's VM. The SceneLoad suite sets Marker in SceneLoadA and expects a fresh, unset module after
-- Scene.Load, because every loaded scene gets a new VM (§11.1).
local Carry = {}

Carry.Marker = nil :: number?
Carry.Loads = 0

return Carry
