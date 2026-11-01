-- fnapp_probe.lua - drive a FujiNet app booted on an emulated Apple II with
-- a CoGS card, headless. Takes a screenshot every FN_SNAP seconds (default
-- 5) and types keys on a schedule given in FN_KEYS:
--
--   FN_KEYS="20:rob\r;35:\r;60:c"      -> at t=20 type "rob" + Enter, ...
--
-- \r is Enter, \e is Escape, \t is Tab. Snapshots go to -snapshot_directory
-- with -snapname. The MAME backend prints "cogs: http sock N done ..." for
-- every HTTP transaction, which is the pass signal for the bus.

local snap_every = tonumber(os.getenv("FN_SNAP") or "5")
local keys_env   = os.getenv("FN_KEYS") or ""
local schedule   = {}
for item in string.gmatch(keys_env, "[^;]+") do
	local t, k = string.match(item, "^(%d+):(.*)$")
	if t then
		k = k:gsub("\\r", "\r"):gsub("\\e", "\27"):gsub("\\t", "\t")
		table.insert(schedule, { t = tonumber(t), k = k, done = false })
	end
end

local last_snap = -1
local function tick()
	local now = math.floor(manager.machine.time.seconds)
	if snap_every > 0 and now ~= last_snap and now % snap_every == 0 then
		last_snap = now
		manager.machine.video:snapshot()
	end
	for _, s in ipairs(schedule) do
		if not s.done and now >= s.t then
			s.done = true
			manager.machine.natkeyboard:post(s.k)
			emu.print_info(string.format("FNAPP: t=%d typed %q", now, s.k))
		end
	end
end

emu.register_periodic(tick)
