-- wrk script: POST realistic TelemetryEvent JSON to /publish.
--
--   wrk -t4 -c100 -d10s --latency -s telemetry.lua http://127.0.0.1:8080/publish -- 1024
--
-- The trailing argument is the target body size in bytes. Bodies larger than
-- a plain event (~120 B) are padded with an extra "note" field, which the
-- gateway accepts and ignores, so every request is valid telemetry.

local base = '{"vehicle_id":"van-%05d","lat":51.5074,"lng":-0.1278,' ..
             '"speed":32.5,"status":"moving","timestamp":1726310400%s}'
local padding = ""
local counter = 0
local headers = { ["Content-Type"] = "application/json" }

function init(args)
  local target = tonumber(args[1] or "0") or 0
  local plain = string.format(base, 0, "")
  local overhead = #',"note":""'
  if target > #plain + overhead then
    padding = ',"note":"' .. string.rep("x", target - #plain - overhead) .. '"'
  end
  -- Give each wrk thread its own range of vehicle ids.
  counter = math.random(0, 100000)
end

function request()
  counter = counter + 1
  return wrk.format("POST", nil, headers, string.format(base, counter % 2000, padding))
end
