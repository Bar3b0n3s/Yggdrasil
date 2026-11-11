--!strict
-- Where the pointer must be for a World canvas's pixel (Docs/Decisions/0033-m22-contract.md decisions 49 and 50): a canvas
-- pixel (x right, y down from the canvas's top-left corner) lies in the canvas entity's local XY plane, centred on its
-- origin, at ((x - WorldWidth / 2) / PixelsPerUnit, (WorldHeight / 2 - y) / PixelsPerUnit, 0); its entity's transform
-- places it in the world and the primary camera projects it to view coordinates, the pointer's space, whose ray the UiInput
-- phase casts back onto the canvas. The UiWorld suite and the recording suite of Assets/Tests/Replays/UiWorld.replay read
-- the scene only, so their clicks and drags are plain pointer input.
local WorldPointer = {}

-- The view point (z 0) that shows `pixel` of the World canvas on `canvasEntity` through the scene's primary camera.
function WorldPointer.ViewPoint(canvasEntity: Entity, pixel: vector): vector
	local canvas = canvasEntity.UiCanvas
	assert(canvas ~= nil and canvas.RenderMode == "World", canvasEntity.Name .. " is a World canvas")
	local pixelsPerUnit = canvas.PixelsPerUnit
	local localPoint = vector.create((pixel.x - canvas.WorldWidth / 2) / pixelsPerUnit, (canvas.WorldHeight / 2 - pixel.y) / pixelsPerUnit, 0)
	local cameraEntity = Scene.GetPrimaryCamera()
	assert(cameraEntity ~= nil, "the scene has a primary camera")
	local camera = cameraEntity.Camera
	assert(camera ~= nil, "the primary camera has its Camera")
	local view = camera:WorldToScreen(canvasEntity.Transform:TransformPoint(localPoint))
	return vector.create(view.x, view.y, 0)
end

-- The view point at the fraction `along` of `element`'s width, on its horizontal centre line, `element` being an element
-- of the World canvas on `canvasEntity` (Ui.GetRect reports it in that canvas's pixels).
function WorldPointer.Along(canvasEntity: Entity, element: Entity, along: number): vector
	local rect = Ui.GetRect(element)
	assert(rect ~= nil, element.Name .. " is laid out")
	assert(rect.Canvas == canvasEntity, element.Name .. " is on " .. canvasEntity.Name)
	local pixel = vector.create(rect.Min.x + (rect.Max.x - rect.Min.x) * along, (rect.Min.y + rect.Max.y) / 2, 0)
	return WorldPointer.ViewPoint(canvasEntity, pixel)
end

return WorldPointer
