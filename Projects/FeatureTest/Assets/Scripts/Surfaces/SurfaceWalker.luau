--!strict
-- Surfaces.scene's Rider (Docs/Decisions/0053-m36-contact-surfaces.md): walks along X at the MoveX axis times Speed, so a
-- replay moves it by input while the belt under it carries it.
export type SurfaceWalker = ScriptInstance & {
	Speed: number,
}

local SurfaceWalker = {}

SurfaceWalker.Fields = {
	Speed = Field.Number(2, { Min = 0 }),
}

function SurfaceWalker.OnFixedUpdate(self: SurfaceWalker, dt: number)
	local character = self.Entity.CharacterController
	if character ~= nil then
		character:Move(vector.create(Input.GetAxis("MoveX") * self.Speed, 0, 0))
	end
end

return Script.Define("SurfaceWalker", SurfaceWalker)
