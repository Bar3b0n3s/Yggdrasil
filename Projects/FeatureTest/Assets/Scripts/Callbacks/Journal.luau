--!strict
-- The shared callback journal of Callbacks.scene. Every Recorder appends "<Label>:<event>" in the order its callbacks
-- run, so the Callbacks suite compares the observed order with Architecture §5.7 and §11.3. Module state lives in the
-- scene's VM, which the suite shares; Scene.Load or a new session starts an empty journal.
local Journal = {}

Journal.Entries = {} :: { string }
-- Whether the per-phase callbacks ("fixed", "update", "late") are journalled; lifecycle and physics events always are.
Journal.TracePhases = true
-- The label of every live Recorder by entity, registered at OnCreate, so a contact partner is named without reading
-- the partner itself (a destroyed partner may no longer be read).
Journal.Labels = {} :: { [Entity]: string }
-- The latest collision contact each label received.
Journal.Contacts = {} :: { [string]: CollisionContact }
-- The simulation tick at which each entry was first recorded, so a suite can tell when a callback arrived.
Journal.Ticks = {} :: { [string]: number }

function Journal.Record(label: string, event: string)
	local entry = label .. ":" .. event
	table.insert(Journal.Entries, entry)
	if Journal.Ticks[entry] == nil then
		Journal.Ticks[entry] = Time.GetTick()
	end
end

function Journal.LabelOf(entity: Entity): string
	return Journal.Labels[entity] or "?"
end

-- The number of entries recorded so far: a cursor for Since.
function Journal.Count(): number
	return #Journal.Entries
end

-- The entries recorded after `cursor`, keeping only those that pass `keep` when it is given.
function Journal.Since(cursor: number, keep: ((string) -> boolean)?): { string }
	local result: { string } = {}
	for index = cursor + 1, #Journal.Entries do
		local entry = Journal.Entries[index]
		if keep == nil or keep(entry) then
			table.insert(result, entry)
		end
	end
	return result
end

-- Whether `entry` was recorded after `cursor`.
function Journal.Has(cursor: number, entry: string): boolean
	for index = cursor + 1, #Journal.Entries do
		if Journal.Entries[index] == entry then
			return true
		end
	end
	return false
end

-- The position of `entry` after `cursor`, or 0.
function Journal.Find(cursor: number, entry: string): number
	for index = cursor + 1, #Journal.Entries do
		if Journal.Entries[index] == entry then
			return index
		end
	end
	return 0
end

return Journal
