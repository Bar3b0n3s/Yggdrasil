--!strict
-- The shared journal of MultiScript.scene: every Tracer appends "<Label>:<event>" in the order its callbacks run, so the
-- MultiScript suite compares the order of an entity's script slots with Architecture §11.3. Module state lives in the
-- scene's VM; a new session starts an empty journal. Only the latest Capacity entries are kept, in a ring, so a long
-- session (the hardening soak plays the scene for 10,000 ticks) does not grow the script heap; reading entries the ring
-- has already dropped is an error, never a silent miss.
local Journal = {}

Journal.Capacity = 4096

-- The ring of the latest entries, and how many entries were recorded in all.
local ring: { string } = {}
local total = 0

function Journal.Record(label: string, event: string)
	total += 1
	ring[(total - 1) % Journal.Capacity + 1] = label .. ":" .. event
end

-- The number of entries recorded so far: a cursor for Since.
function Journal.Count(): number
	return total
end

-- The entries recorded after `cursor` that pass `keep`.
function Journal.Since(cursor: number, keep: (string) -> boolean): { string }
	if cursor + 1 < total - Journal.Capacity + 1 then
		error(`Journal: the entries after cursor {cursor} were dropped; only the latest {Journal.Capacity} are kept`)
	end
	local result: { string } = {}
	for index = cursor + 1, total do
		local entry = ring[(index - 1) % Journal.Capacity + 1]
		if keep(entry) then
			table.insert(result, entry)
		end
	end
	return result
end

return Journal
