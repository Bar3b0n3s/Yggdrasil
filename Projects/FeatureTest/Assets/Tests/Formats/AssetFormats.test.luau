--!strict
-- Mesh version 2 and Material version 2 in every run mode (M23 W23.2; Docs/Decisions/0034-m23-contract.md decisions 9,
-- 10 and 17): AssetFormats.jsonl imported Attributes.gltf, one mesh of three quads, the first with TEXCOORD_1 and COLOR_0
-- (cooked into the mesh's attribute stream) under the material Painted, whose base colour and normal textures sample
-- TEXCOORD_1 (its BaseColorUVSet and NormalUVSet are UV1), the second with COLOR_0 alone and the third with neither;
-- placed an instance of it in Formats.scene; and created SecondUV.material, a native material with every UV set UV1,
-- which a built-in quad without the stream draws (the shared default stream's (0, 0) and opaque white). The Formats
-- suite's screenshot of Formats.scene renders both. It also imported Gradient.exr, the OpenEXR copy of Gradient.hdr's
-- texels, twice (decision 14): with EnvironmentImportSettings.Compression Auto (BC6H cubes) into Assets/Formats, which
-- the Formats suite loads with every format there, and with None (RGBA16F cubes) into Assets/Environments/Uncompressed.

local GLTF = "Assets/Formats/Gltf/Attributes.gltf"
local MATERIAL = "Assets/Formats/Materials/SecondUV.material"
-- Formats.scene's own environment, and Gradient.exr imported with Compression Auto and None.
local HDR = "Assets/Formats/Environments/Gradient.hdr"
local EXR: { { Path: string, Screenshot: string } } = {
	{ Path = "Assets/Formats/Environments/Gradient.exr", Screenshot = "AssetFormatsExrAuto" },
	{ Path = "Assets/Environments/Uncompressed/Gradient.exr", Screenshot = "AssetFormatsExrNone" },
}

local function load(path: string): AssetRef
	local asset = Assets.Load(path)
	assert(asset, `{path} loads`)
	return asset
end

local function find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity, `Formats.scene has an entity named {name}`)
	return entity
end

-- The first MeshRenderer with a mesh on `entity` or below it, depth first.
local function findRenderer(entity: Entity): MeshRenderer?
	local renderer = entity.MeshRenderer
	if renderer and renderer.Mesh ~= nil then
		return renderer
	end
	for _, child in entity:GetChildren() do
		local found = findRenderer(child)
		if found then
			return found
		end
	end
	return nil
end

return Test.Suite("Formats.AssetFormats", function()
	Test.Case("a glTF with TEXCOORD_1 and COLOR_0 imports its mesh, materials and textures", function()
		Test.ExpectEqual(Assets.GetType(load(GLTF)), "Prefab")
		Test.ExpectEqual(Assets.GetType(load(`{GLTF}#mesh:0:Painted`)), "Mesh")
		for _, key in { "material:0:Painted", "material:1:Plain", "material:2:Bare" } do
			Test.ExpectEqual(Assets.GetType(load(`{GLTF}#{key}`)), "Material", key)
		end
		for _, key in { "texture:0", "texture:1:normal" } do
			Test.ExpectEqual(Assets.GetType(load(`{GLTF}#{key}`)), "Texture", key)
		end
	end)

	Test.Case("the glTF's instance draws its mesh with its second UV set's material", function()
		local renderer = findRenderer(find("Attributes"))
		assert(renderer, "the Attributes instance draws a mesh")
		Test.ExpectEqual(renderer.Mesh, load(`{GLTF}#mesh:0:Painted`))
		Test.ExpectEqual(renderer:GetMaterial(1), load(`{GLTF}#material:0:Painted`))
		local instance = Scene.Instantiate(load(GLTF), vector.create(-6, 4, -3))
		Test.Expect(instance:IsValid(), "a new Attributes instance")
		Test.Expect(findRenderer(instance) ~= nil, "a new Attributes instance draws a mesh")
	end)

	Test.Case("a native material with every UV set UV1 draws on a mesh without the attribute stream", function()
		local renderer = find("SecondUV").MeshRenderer
		assert(renderer, "SecondUV has a MeshRenderer")
		Test.ExpectEqual(Assets.GetType(load(MATERIAL)), "Material")
		Test.ExpectEqual(renderer:GetMaterial(1), load(MATERIAL))
	end)

	Test.Case("an OpenEXR environment imported with Compression Auto and None lights the scene", function()
		local environment = find("Environment").Environment
		assert(environment, "the Environment entity has an Environment component")
		Test.ExpectEqual(environment.Environment, load(HDR))
		for _, exr in EXR do
			local asset = load(exr.Path)
			Test.ExpectEqual(Assets.GetType(asset), "Environment", exr.Path)
			environment.Environment = asset
			Test.WaitTicks(2)
			Test.ExpectEqual(environment.Environment, asset, exr.Path)
			Test.Screenshot(exr.Screenshot)
		end
		environment.Environment = load(HDR)
	end)
end)
