--!strict
-- Ogg Opus and the texture Compression setting in every run mode (M23 W23.1; Docs/Decisions/0034-m23-contract.md
-- decisions 13, 16 and 20): OpusAndCompression.jsonl imported the pinned Water.opus beside Formats.jsonl's clips, placed
-- its source under Formats.scene's Clips, and imported Rgba.png a second time with Compression None. The clip decodes
-- through the fixed-point Opus decoder, so its samples hash to the digest AudioDecoderTests commits in the editor and in
-- the exported Release and Dist runs alike.

local OPUS = "Assets/Formats/Audio/Water.opus"
-- Test.GetAudioClipDigest's 16 lowercase hexadecimal digits of AudioDecoderTests' 0x33F78152D7B526CD.
local OPUS_DIGEST = "33f78152d7b526cd"
-- The same PNG imported with Compression Auto (BC7, Formats.jsonl) and None (RGBA8, OpusAndCompression.jsonl).
local TEXTURES = { "Assets/Formats/Textures/Rgba.png", "Assets/Textures/Uncompressed/Rgba.png" }

local function load(path: string): AssetRef
	local asset = Assets.Load(path)
	assert(asset, `{path} loads`)
	return asset
end

return Test.Suite("Formats.OpusAndCompression", function()
	Test.Case("an Ogg Opus clip decodes bit-exact to its committed digest", function()
		local clip = load(OPUS)
		Test.ExpectEqual(Assets.GetType(clip), "AudioClip")
		Test.ExpectEqual(Test.GetAudioClipDigest(clip), OPUS_DIGEST)
	end)

	Test.Case("an Ogg Opus clip plays from its source", function()
		local entity = Scene.FindByName("Opus")
		assert(entity, "Formats.scene has the Opus source")
		local source = entity.AudioSource
		assert(source, "the Opus entity has an AudioSource")
		Test.ExpectEqual(source.Clip, load(OPUS))
		source:Play()
		Test.Expect(source:IsPlaying(), "the Opus clip plays")
		local levels = Test.CaptureAudio(32)
		Test.Expect(levels.Peak > 0.01, `the Opus clip is audible (peak {levels.Peak})`)
		Test.Expect(levels.RmsLeft > 0.001 and levels.RmsRight > 0.001, "the Opus clip reaches both channels")
		source:Stop()
		Test.WaitTicks(1)
		Test.ExpectEqual(Test.CaptureAudio(2).Peak, 0, "the Opus source stopped")
	end)

	Test.Case("a texture imported with Compression None loads like its Auto import", function()
		for _, path in TEXTURES do
			local texture = load(path)
			Test.Expect(Assets.IsValid(texture), path)
			Test.ExpectEqual(Assets.GetType(texture), "Texture", path)
			Test.ExpectEqual(Assets.GetPath(texture), path)
		end
	end)
end)
