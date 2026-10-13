--!strict
-- Formats.scene (Architecture §15.5; ADR 0022 decisions 5, 9 and 16): every asset under Assets/Formats/ is loaded,
-- instantiated or played, so every importer and extension of the foreign formats, and the native .material, .scene and
-- .sfx formats, are cooked and loaded in every run mode (gate 9).

type Expected = { Path: string, Type: string }

-- Every asset under Assets/Formats/, with its type.
local ASSETS: { Expected } = {
	{ Path = "Assets/Formats/Audio/Blip.sfx", Type = "AudioClip" },
	{ Path = "Assets/Formats/Audio/Chime.sfx", Type = "AudioClip" },
	{ Path = "Assets/Formats/Audio/Tone.flac", Type = "AudioClip" },
	{ Path = "Assets/Formats/Audio/Tone.mp3", Type = "AudioClip" },
	{ Path = "Assets/Formats/Audio/Tone.ogg", Type = "AudioClip" },
	{ Path = "Assets/Formats/Audio/Tone.wav", Type = "AudioClip" },
	{ Path = "Assets/Formats/Environments/Gradient.hdr", Type = "Environment" },
	{ Path = "Assets/Formats/Fonts/Glyphs.ttf", Type = "Font" },
	{ Path = "Assets/Formats/Fonts/Inter-Regular.otf", Type = "Font" },
	{ Path = "Assets/Formats/Gltf/Box.glb", Type = "Prefab" },
	{ Path = "Assets/Formats/Gltf/Box.gltf", Type = "Prefab" },
	{ Path = "Assets/Formats/Gltf/DataUri.gltf", Type = "Prefab" },
	{ Path = "Assets/Formats/Gltf/Textured.gltf", Type = "Prefab" },
	{ Path = "Assets/Formats/Gltf/Textured.gltf#texture:0", Type = "Texture" },
	{ Path = "Assets/Formats/Materials/Matte.material", Type = "Material" },
	{ Path = "Assets/Formats/Textures/Alpha.tga", Type = "Texture" },
	{ Path = "Assets/Formats/Textures/Baseline.jpeg", Type = "Texture" },
	{ Path = "Assets/Formats/Textures/Photo.jpg", Type = "Texture" },
	{ Path = "Assets/Formats/Textures/Rgba.png", Type = "Texture" },
	{ Path = "Assets/Formats/Textures/Solid.bmp", Type = "Texture" },
	{ Path = "Assets/Scenes/Formats.scene", Type = "Scene" },
}

-- The glTF variants: binary with an embedded image, embedded (data-URI) buffers, data URIs for buffer and image, and
-- an external buffer and image.
local MODELS = { "BoxGlb", "BoxGltf", "DataUri", "Textured" }
local MODEL_PATHS = {
	BoxGlb = "Assets/Formats/Gltf/Box.glb",
	BoxGltf = "Assets/Formats/Gltf/Box.gltf",
	DataUri = "Assets/Formats/Gltf/DataUri.gltf",
	Textured = "Assets/Formats/Gltf/Textured.gltf",
}

-- The audio sources of Formats.scene and how many ticks of each clip to capture: the Tone clips are 0.1 s (WAV, FLAC),
-- 2 s (Ogg Vorbis) and 4.4 s of beeps (MP3, streamed), the sound effects 0.24 s (Chime) and 0.1 s (Blip).
local CLIPS: { { Name: string, Path: string, Ticks: number } } = {
	{ Name = "Wav", Path = "Assets/Formats/Audio/Tone.wav", Ticks = 4 },
	{ Name = "Flac", Path = "Assets/Formats/Audio/Tone.flac", Ticks = 4 },
	{ Name = "Mp3", Path = "Assets/Formats/Audio/Tone.mp3", Ticks = 32 },
	{ Name = "Ogg", Path = "Assets/Formats/Audio/Tone.ogg", Ticks = 8 },
	{ Name = "Chime", Path = "Assets/Formats/Audio/Chime.sfx", Ticks = 8 },
	{ Name = "Blip", Path = "Assets/Formats/Audio/Blip.sfx", Ticks = 4 },
}

local function find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity, `Formats.scene has an entity named {name}`)
	return entity
end

local function load(path: string): AssetRef
	local asset = Assets.Load(path)
	assert(asset, `{path} loads`)
	return asset
end

-- Whether `entity` or a descendant draws a mesh.
local function hasMesh(entity: Entity): boolean
	local renderer = entity.MeshRenderer
	if renderer and renderer.Mesh ~= nil then
		return true
	end
	for _, child in entity:GetChildren() do
		if hasMesh(child) then
			return true
		end
	end
	return false
end

return Test.Suite("Formats", function()
	Test.Case("every asset under Assets/Formats loads with its type and path", function()
		for _, expected in ASSETS do
			local asset = Assets.Load(expected.Path)
			Test.Expect(asset ~= nil, `{expected.Path} loads`)
			if asset then
				Test.Expect(Assets.IsValid(asset), expected.Path)
				Test.ExpectEqual(Assets.GetType(asset), expected.Type, expected.Path)
				Test.ExpectEqual(Assets.GetPath(asset), expected.Path)
			end
		end
	end)

	Test.Case("the native material draws the floor", function()
		local renderer = find("Floor").MeshRenderer
		assert(renderer, "Floor has a MeshRenderer")
		Test.ExpectEqual(renderer:GetMaterial(1), load("Assets/Formats/Materials/Matte.material"))
	end)

	Test.Case("every glTF variant instantiates with its meshes", function()
		for index, name in MODELS do
			local placed = find(name)
			Test.Expect(hasMesh(placed), `the scaffold's {name} instance draws a mesh`)
			local instance = Scene.Instantiate(load(MODEL_PATHS[name]), vector.create(-3 + 2 * index, 4, -3))
			Test.Expect(instance:IsValid(), name)
			Test.Expect(hasMesh(instance), `a new {name} instance draws a mesh`)
		end
	end)

	Test.Case("the fonts and the environment render", function()
		local glyphs = find("GlyphsText").Text
		local inter = find("InterText").Text
		assert(glyphs and inter, "both text entities have a Text component")
		Test.ExpectEqual(glyphs.Font, load("Assets/Formats/Fonts/Glyphs.ttf"))
		Test.ExpectEqual(inter.Font, load("Assets/Formats/Fonts/Inter-Regular.otf"))
		local environment = find("Environment").Environment
		assert(environment, "the Environment entity has an Environment component")
		Test.ExpectEqual(environment.Environment, load("Assets/Formats/Environments/Gradient.hdr"))
		Test.WaitTicks(2)
		Test.Screenshot("Formats")
	end)

	Test.Case("every audio clip plays from its source", function()
		for _, clip in CLIPS do
			local source = find(clip.Name).AudioSource
			assert(source, `{clip.Name} has an AudioSource`)
			Test.ExpectEqual(source.Clip, load(clip.Path), clip.Name)
			source:Play()
			Test.Expect(source:IsPlaying(), `{clip.Name} plays`)
			local levels = Test.CaptureAudio(clip.Ticks)
			Test.Expect(levels.Peak > 0.05, `{clip.Name} is audible (peak {levels.Peak})`)
			Test.Expect(levels.RmsLeft > 0.005 and levels.RmsRight > 0.005, `{clip.Name} reaches both channels`)
			source:Stop()
			Test.WaitTicks(1)
		end
		local silence = Test.CaptureAudio(2)
		Test.ExpectEqual(silence.Peak, 0, "every source stopped")
	end)
end)
