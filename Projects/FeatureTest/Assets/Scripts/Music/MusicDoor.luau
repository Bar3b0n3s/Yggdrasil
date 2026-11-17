--!strict
-- A doorway of MusicA.scene and MusicB.scene (M40, Docs/Decisions/0057-m40-music.md decision 12): its key, pressed, crossfades
-- to its Track and loads its Target, once, and moves the door, so the run's input reaches the state hash (Music.replay
-- presses the keys).
export type MusicDoor = ScriptInstance & {
	Key: string,
	Target: AssetRef?,
	Track: AssetRef?,
	Opened: boolean,
}

local MusicDoor = {}

MusicDoor.Fields = {
	Key = Field.String("Space", { Tooltip = "The key that opens the door" }),
	Target = Field.Asset("Scene"),
	Track = Field.Asset("AudioClip"),
}

function MusicDoor.OnCreate(self: MusicDoor)
	self.Opened = false
end

function MusicDoor.OnFixedUpdate(self: MusicDoor)
	if self.Opened or not Input.IsKeyPressed(self.Key :: any) then
		return
	end
	self.Opened = true
	self.Entity.Transform.Translation = vector.create(0, 2, 0)
	local track = self.Track
	if track then
		Audio.PlayMusic(track, { FadeSeconds = 0.5 })
	end
	local target = self.Target
	if target then
		Scene.Load(target)
	end
end

return Script.Define("MusicDoor", MusicDoor)
