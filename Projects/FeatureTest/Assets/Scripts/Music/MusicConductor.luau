--!strict
-- The music of MusicA.scene and MusicB.scene (M40, Docs/Decisions/0057-m40-music.md decision 9): each scene asks for its
-- track when it starts, as World Tree's arenas do, so a track the session already plays goes on through the doorway
-- (Audio.PlayMusic keeps the voice of the clip already playing).
export type MusicConductor = ScriptInstance & {
	Track: AssetRef?,
	FadeSeconds: number,
}

local MusicConductor = {}

MusicConductor.Fields = {
	Track = Field.Asset("AudioClip"),
	FadeSeconds = Field.Number(0.5, { Min = 0, Max = 60, Tooltip = "The fade into the track, in seconds" }),
}

function MusicConductor.OnStart(self: MusicConductor)
	local track = self.Track
	if track then
		Audio.PlayMusic(track, { FadeSeconds = self.FadeSeconds })
	end
end

return Script.Define("MusicConductor", MusicConductor)
