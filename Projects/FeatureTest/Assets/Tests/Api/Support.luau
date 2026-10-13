--!strict
-- Shared helpers of the Api suites (Scripting.scene).
local Support = {}

-- The entity named `name` in the running scene; a missing one fails the calling case.
function Support.Find(name: string): Entity
	local entity = Scene.FindByName(name)
	if entity == nil then
		error(name .. " is not in the scene", 2)
	end
	return entity
end

-- Rounds a number to float, the precision of reflected float fields.
function Support.F32(value: number): number
	local scratch = buffer.create(4)
	buffer.writef32(scratch, 0, value)
	return buffer.readf32(scratch, 0)
end

-- Reads `field` of a component proxy, writes `value` through it and expects to read `value` back (within `epsilon` for
-- numbers and vectors when given). Returns the value read before the write.
function Support.RoundTrip(proxy: any, field: string, value: any, epsilon: number?): any
	local before = proxy[field]
	proxy[field] = value
	local after = proxy[field]
	if epsilon ~= nil then
		Test.ExpectNear(after, value, epsilon, field .. " reads back what was written")
	else
		Test.ExpectEqual(after, value, field .. " reads back what was written")
	end
	return before
end

-- Expects two quaternions to have equal components within `epsilon`.
function Support.ExpectQuatNear(actual: Quat, expected: Quat, epsilon: number, message: string)
	Test.ExpectNear(vector.create(actual.x, actual.y, actual.z), vector.create(expected.x, expected.y, expected.z), epsilon, message)
	Test.ExpectNear(actual.w, expected.w, epsilon, message)
end

-- Expects two colours to have equal channels within `epsilon`.
function Support.ExpectColorNear(actual: Color, expected: Color, epsilon: number, message: string)
	Test.ExpectNear(vector.create(actual.r, actual.g, actual.b), vector.create(expected.r, expected.g, expected.b), epsilon, message)
	Test.ExpectNear(actual.a, expected.a, epsilon, message)
end

return Support
