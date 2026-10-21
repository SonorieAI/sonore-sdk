-- SPDX-License-Identifier: Apache-2.0
-- Sonore SDK -- the half of scripts/reaper-host-test.mjs that runs INSIDE
-- REAPER. The node script writes a session description (CFG, a Lua table)
-- and a two-line launcher that loads it and then this file; REAPER runs the
-- launcher as its startup ReaScript. Everything this measures goes into one
-- JSON file the node script reads back -- REAPER's own console is a window,
-- and a window is something a test has to close.
--
-- Built as a small state machine on reaper.defer rather than one straight
-- run, because two of the things it checks only happen BETWEEN script
-- slices: a CLAP parameter set from here reaches the plugin on its next
-- process() (or flush) call, and an editor only paints, and its page only
-- talks to the host, while REAPER's message loop runs. A script that set a
-- value and read it back in the same breath measured itself.
--
-- Every stage is wrapped in pcall and its error recorded, and the results
-- file is rewritten after each stage, so a REAPER that dies part way still
-- leaves the node side a file that says how far it got.

local CFG = CFG
local results = { session = CFG.session, stage = "start", errors = {}, installed = {},
                  tracks = {}, editor = {}, version = reaper.GetAppVersion() }

-- ── JSON out ────────────────────────────────────────────────────────────────
local function isArray(t)
  local n = 0
  for k in pairs(t) do
    if type(k) ~= "number" then return false end
    n = n + 1
  end
  return n == #t
end
local function enc(v)
  local t = type(v)
  if t == "nil" then return "null" end
  if t == "boolean" then return v and "true" or "false" end
  if t == "number" then
    if v ~= v or v == math.huge or v == -math.huge then return "null" end
    if math.type and math.type(v) == "integer" then return tostring(v) end
    return string.format("%.17g", v)
  end
  if t == "string" then
    return '"' .. v:gsub('[%c"\\]', function(c)
      if c == '"' then return '\\"' end
      if c == "\\" then return "\\\\" end
      return string.format("\\u%04x", c:byte())
    end) .. '"'
  end
  if t == "table" then
    local parts = {}
    if next(v) == nil then return "[]" end
    if isArray(v) then
      for _, x in ipairs(v) do parts[#parts + 1] = enc(x) end
      return "[" .. table.concat(parts, ",") .. "]"
    end
    for k, x in pairs(v) do parts[#parts + 1] = enc(tostring(k)) .. ":" .. enc(x) end
    return "{" .. table.concat(parts, ",") .. "}"
  end
  return "null"
end
local function flush()
  local f = io.open(CFG.out, "w")
  if f then f:write(enc(results)) f:close() end
end
local function fail(where, err)
  results.errors[#results.errors + 1] = where .. ": " .. tostring(err)
  flush()
end
local function stage(name, fn, ...)
  results.stage = name
  flush()
  local ok, err = pcall(fn, ...)
  if not ok then fail(name, err) end
  return ok
end

-- ── base64, for the VST3 state chunk ────────────────────────────────────────
local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local function b64enc(data)
  return ((data:gsub(".", function(x)
    local r, b = "", x:byte()
    for i = 8, 1, -1 do r = r .. (b % 2 ^ i - b % 2 ^ (i - 1) > 0 and "1" or "0") end
    return r
  end) .. "0000"):gsub("%d%d%d?%d?%d?%d?", function(x)
    if #x < 6 then return "" end
    local c = 0
    for i = 1, 6 do c = c + (x:sub(i, i) == "1" and 2 ^ (6 - i) or 0) end
    return B64:sub(c + 1, c + 1)
  end) .. ({ "", "==", "=" })[#data % 3 + 1])
end
local function b64dec(data)
  data = data:gsub("[^" .. B64 .. "=]", "")
  return (data:gsub(".", function(x)
    if x == "=" then return "" end
    local r, f = "", (B64:find(x, 1, true) - 1)
    for i = 6, 1, -1 do r = r .. (f % 2 ^ i - f % 2 ^ (i - 1) > 0 and "1" or "0") end
    return r
  end):gsub("%d%d%d?%d?%d?%d?%d?%d?", function(x)
    if #x ~= 8 then return "" end
    local c = 0
    for i = 1, 8 do c = c + (x:sub(i, i) == "1" and 2 ^ (8 - i) or 0) end
    return string.char(c)
  end))
end

-- ── Helpers ─────────────────────────────────────────────────────────────────
local proj = 0
local function now() return reaper.time_precise() end

-- The 2048 MIDI-CC pseudo-parameters REAPER gives a VST3 instrument
-- ("CC 7 Ch1", "Mod Wheel Ch3") mirror incoming MIDI and are not state: they
-- are not read. REAPER's own Bypass, Wet and Delta are, and come last.
local function ccParam(name) return name:match(" Ch%d+$") ~= nil end

local function readParams(tr, fx, limit)
  local out = {}
  local n = reaper.TrackFX_GetNumParams(tr, fx)
  for p = 0, math.min(n, limit or 48) - 1 do
    local _, name = reaper.TrackFX_GetParamName(tr, fx, p, "")
    if not ccParam(name) then
      local v, mn, mx = reaper.TrackFX_GetParam(tr, fx, p)
      local _, text = reaper.TrackFX_GetFormattedParamValue(tr, fx, p, "")
      out[#out + 1] = { i = p, name = name, value = v, min = mn, max = mx,
                        norm = reaper.TrackFX_GetParamNormalized(tr, fx, p), text = text }
    end
  end
  return out
end

local function named(tr, fx, key)
  local ok, v = reaper.TrackFX_GetNamedConfigParm(tr, fx, key)
  if ok then return v end
  return nil
end

local function fxInfo(tr, fx)
  local _, name = reaper.TrackFX_GetFXName(tr, fx, "")
  local _, ins, outs = reaper.TrackFX_GetIOSize(tr, fx)
  local presetIndex, presetCount = reaper.TrackFX_GetPresetIndex(tr, fx)
  local _, presetName = reaper.TrackFX_GetPreset(tr, fx, "")
  return { index = fx, name = name, ident = named(tr, fx, "fx_ident"), type = named(tr, fx, "fx_type"),
           ins = ins, outs = outs, pdc = tonumber(named(tr, fx, "pdc") or ""),
           presetIndex = presetIndex, presetCount = presetCount, presetName = presetName,
           params = readParams(tr, fx) }
end

local function trackByName(name)
  for i = 0, reaper.CountTracks(proj) - 1 do
    local tr = reaper.GetTrack(proj, i)
    local _, n = reaper.GetSetMediaTrackInfo_String(tr, "P_NAME", "", false)
    if n == name then return tr end
  end
  return nil
end

-- ── Building the session ────────────────────────────────────────────────────
local built = {} -- name -> { tr = track, spec = spec, rec = results entry }

local function addMidi(tr, notes, length)
  local item = reaper.CreateNewMIDIItemInProj(tr, 0, length, false)
  local take = item and reaper.GetActiveTake(item)
  if not take then error("no MIDI take") end
  for _, n in ipairs(notes) do
    reaper.MIDI_InsertNote(take, false, false, reaper.MIDI_GetPPQPosFromProjTime(take, n[1]),
                           reaper.MIDI_GetPPQPosFromProjTime(take, n[2]), 0, n[3], n[4], true)
  end
  reaper.MIDI_Sort(take)
end

local function addMedia(tr, file)
  reaper.SetOnlyTrackSelected(tr)
  reaper.SetEditCurPos(0, false, false)
  local before = reaper.CountTrackMediaItems(tr)
  reaper.InsertMedia(file, 0)
  local item = reaper.GetTrackMediaItem(tr, before)
  if not item then error("InsertMedia put nothing on the track: " .. file) end
  reaper.SetMediaItemInfo_Value(item, "D_POSITION", 0)
  reaper.SetMediaItemInfo_Value(item, "D_FADEINLEN", 0)
  reaper.SetMediaItemInfo_Value(item, "D_FADEOUTLEN", 0)
  reaper.SetMediaItemInfo_Value(item, "D_VOL", 1)
end

-- The sampler's file, handed to it the way a session hands it: as the state
-- blob REAPER stores and gives back. The blob itself (an SNRS header, the
-- parameters, and a StateBag naming the file) is built by the node side, so
-- the reference render loads exactly the same bytes.
local function injectState(tr, fx, b64)
  local kind = named(tr, fx, "fx_type") or ""
  if kind:find("VST3") then
    local chunk = named(tr, fx, "vst_chunk")
    if not chunk then error("no vst_chunk to replace") end
    local raw = b64dec(chunk)
    -- REAPER's VST3 chunk: component size, a flag, the component state (our
    -- SNRS blob), then the controller state. Only the component part changes.
    local oldLen = string.unpack("<I4", raw, 1)
    local snrs = b64dec(b64)
    local fresh = string.pack("<I4", #snrs) .. raw:sub(5, 8) .. snrs .. raw:sub(9 + oldLen)
    if not reaper.TrackFX_SetNamedConfigParm(tr, fx, "vst_chunk", b64enc(fresh)) then
      error("vst_chunk refused")
    end
  elseif kind:find("LV2") then
    -- An LV2 plugin's state is in the track chunk too, but as one block per
    -- property the plugin stored through state:interface --
    --   <X <key URI> <type URI> <flags>
    --   base64 lines
    --   >
    -- inside <STATE ... >. The SDK stores one property, urn:sonorie:state:blob
    -- (an atom:Chunk holding the same SNRS bytes), so only its lines change.
    local ok, chunk = reaper.GetTrackStateChunk(tr, "", false)
    if not ok then error("no track chunk") end
    local lines = {}
    for i = 1, #b64, 128 do lines[#lines + 1] = b64:sub(i, i + 127) end
    local replaced, count = chunk:gsub("(<X urn:sonorie:state:blob [^\r\n]*\r?\n).-(\r?\n%s*>)",
                                       function(head, tail)
      return head .. table.concat(lines, "\n") .. tail
    end, 1)
    if count ~= 1 then error("no urn:sonorie:state:blob property in the LV2 chunk") end
    if not reaper.SetTrackStateChunk(tr, replaced, false) then error("SetTrackStateChunk refused") end
  else
    -- CLAP has no named chunk: the state lives in the track's chunk, between
    -- <STATE and its closing bracket, as base64 lines.
    local ok, chunk = reaper.GetTrackStateChunk(tr, "", false)
    if not ok then error("no track chunk") end
    local lines = {}
    for i = 1, #b64, 128 do lines[#lines + 1] = b64:sub(i, i + 127) end
    local replaced, count = chunk:gsub("(<STATE\r?\n).-(\r?\n%s*>)", function(head, tail)
      return head .. table.concat(lines, "\n") .. tail
    end, 1)
    if count ~= 1 then error("no <STATE> block in the CLAP chunk") end
    if not reaper.SetTrackStateChunk(tr, replaced, false) then error("SetTrackStateChunk refused") end
  end
end

local function buildTrack(spec)
  local idx = reaper.CountTracks(proj)
  reaper.InsertTrackAtIndex(idx, false)
  local tr = reaper.GetTrack(proj, idx)
  reaper.GetSetMediaTrackInfo_String(tr, "P_NAME", spec.name, true)
  if spec.channels then reaper.SetMediaTrackInfo_Value(tr, "I_NCHAN", spec.channels) end
  if spec.mainSend == false then reaper.SetMediaTrackInfo_Value(tr, "B_MAINSEND", 0) end
  local rec = { name = spec.name, fx = {}, errors = {} }
  results.tracks[#results.tracks + 1] = rec
  built[spec.name] = { tr = tr, spec = spec, rec = rec }
  if spec.media then addMedia(tr, spec.media) end
  if spec.midi then addMidi(tr, spec.midi, CFG.length) end
  for k, f in ipairs(spec.fx or {}) do
    local fx = reaper.TrackFX_AddByName(tr, f.query, false, -1)
    local entry = { query = f.query, added = fx }
    rec.fx[k] = entry
    if fx >= 0 then
      for key, v in pairs(fxInfo(tr, fx)) do entry[key] = v end
      if f.state then
        local ok, err = pcall(injectState, tr, fx, f.state)
        entry.stateInjected = ok
        if not ok then rec.errors[#rec.errors + 1] = "state: " .. tostring(err) end
      end
      -- {index, position 0..1}: through TrackFX_SetParam in the range
      -- TrackFX_GetParam reports, which is the plain range for CLAP and 0..1
      -- for VST3.
      for _, s in ipairs(f.set or {}) do
        local _, mn, mx = reaper.TrackFX_GetParam(tr, fx, s[1])
        reaper.TrackFX_SetParam(tr, fx, s[1], mn + s[2] * (mx - mn))
      end
      -- The last preset in REAPER's own menu, picked the way a user picks it;
      -- which one that is goes back by name, since REAPER may sort the list.
      if f.lastPreset then
        local _, count = reaper.TrackFX_GetPresetIndex(tr, fx)
        if count and count > 0 then
          entry.presetSet = reaper.TrackFX_SetPresetByIndex(tr, fx, count - 1)
          local _, picked = reaper.TrackFX_GetPreset(tr, fx, "")
          entry.presetPicked = picked
        end
      end
      if f.envelope then
        local e = f.envelope
        local env = reaper.GetFXEnvelope(tr, fx, e.param, true)
        if not env then
          rec.errors[#rec.errors + 1] = "no envelope for parameter " .. e.param
        else
          local _, mn, mx = reaper.TrackFX_GetParam(tr, fx, e.param)
          reaper.DeleteEnvelopePointRange(env, -1, CFG.length + 10)
          for _, pt in ipairs(e.points) do
            reaper.InsertEnvelopePoint(env, pt[1], mn + pt[2] * (mx - mn), 1, 0, false, true)
          end
          reaper.Envelope_SortPoints(env)
          entry.envelopePoints = reaper.CountEnvelopePoints(env)
        end
      end
    end
  end
end

local function buildSends()
  for _, spec in ipairs(CFG.tracks) do
    if spec.sendTo then
      local src, dst = built[spec.name], built[spec.sendTo.track]
      if src and dst then
        local s = reaper.CreateTrackSend(src.tr, dst.tr)
        reaper.SetTrackSendInfo_Value(src.tr, 0, s, "I_SRCCHAN", 0)
        reaper.SetTrackSendInfo_Value(src.tr, 0, s, "I_DSTCHAN", spec.sendTo.dstChan or 2)
        reaper.SetTrackSendInfo_Value(src.tr, 0, s, "D_VOL", 1)
      end
    end
  end
end

-- A project already on disk (the reopen session): adopt its tracks.
local function adoptTracks()
  for i = 0, reaper.CountTracks(proj) - 1 do
    local tr = reaper.GetTrack(proj, i)
    local _, name = reaper.GetSetMediaTrackInfo_String(tr, "P_NAME", "", false)
    local rec = { name = name, fx = {}, errors = {} }
    results.tracks[#results.tracks + 1] = rec
    built[name] = { tr = tr, spec = { name = name }, rec = rec }
    for fx = 0, reaper.TrackFX_GetCount(tr) - 1 do
      local entry = fxInfo(tr, fx)
      entry.added = fx
      if (entry.type or ""):find("VST3") then
        local chunk = named(tr, fx, "vst_chunk")
        entry.stateBase64 = chunk
      end
      rec.fx[#rec.fx + 1] = entry
    end
    if reaper.TrackFX_GetCount(tr) > 0 then
      local ok, chunk = reaper.GetTrackStateChunk(tr, "", false)
      if ok then
        -- The LV2 property first: its <X ...> header sits inside <STATE too,
        -- and taken as CLAP's base64 it would decode to noise.
        local lv2 = chunk:match("<X urn:sonorie:state:blob [^\r\n]*\r?\n(.-)\r?\n%s*>")
        if lv2 then
          rec.lv2State = lv2:gsub("%s", "")
        else
          local st = chunk:match("<STATE\r?\n(.-)\r?\n%s*>")
          if st then rec.clapState = st:gsub("%s", "") end
        end
      end
    end
  end
end

-- ── After the values have landed ────────────────────────────────────────────
local function readBack()
  for _, b in pairs(built) do
    for k, entry in ipairs(b.rec.fx) do
      if entry.added and entry.added >= 0 then
        entry.after = readParams(b.tr, entry.added)
        entry.pdcAfter = tonumber(named(b.tr, entry.added, "pdc") or "")
      end
    end
    if CFG.dumpChunks then
      local ok, chunk = reaper.GetTrackStateChunk(b.tr, "", false)
      if ok then b.rec.chunk = chunk end
    end
  end
end

-- A switch that changes the plugin's latency, thrown while the transport is
-- stopped. What REAPER's delay compensation says is then watched: first for
-- a few seconds stopped, then (if it has not moved yet) for a few seconds of
-- playback, because a format with no way to call the host from the audio
-- thread may only be heard once the host asks again.
local toggled = {}
local function toggles()
  for _, spec in ipairs(CFG.tracks) do
    for k, f in ipairs(spec.fx or {}) do
      if f.toggle then
        local b = built[spec.name]
        local fx = b.rec.fx[k].added
        if fx and fx >= 0 then
          local entry = b.rec.fx[k]
          entry.pdcBeforeToggle = tonumber(named(b.tr, fx, "pdc") or "")
          reaper.TrackFX_SetParamNormalized(b.tr, fx, f.toggle[1], f.toggle[2])
          toggled[#toggled + 1] = { tr = b.tr, fx = fx, entry = entry, param = f.toggle[1] }
        end
      end
    end
  end
end

local function readToggled(field)
  local moved = true
  for _, t in ipairs(toggled) do
    local pdc = tonumber(named(t.tr, t.fx, "pdc") or "")
    t.entry[field] = pdc
    if pdc == t.entry.pdcBeforeToggle then moved = false end
    local _, text = reaper.TrackFX_GetFormattedParamValue(t.tr, t.fx, t.param, "")
    t.entry.toggleText = text
  end
  return moved
end

local function watchToggles(done)
  if #toggled == 0 then done() return end
  local t0, playing = now(), false
  local function tick()
    local ok, moved = pcall(readToggled, playing and "pdcPlaying" or "pdcStopped")
    if not ok then
      fail("latency switch", moved)
      if playing then reaper.OnStopButton() end
      done()
      return
    end
    if not playing and (moved or now() - t0 > 4) then
      for _, t in ipairs(toggled) do t.entry.secondsStopped = now() - t0 end
      if moved then done() return end
      reaper.OnPlayButton()
      playing, t0 = true, now()
    elseif playing and (moved or now() - t0 > 3) then
      reaper.OnStopButton()
      for _, t in ipairs(toggled) do t.entry.secondsPlaying = now() - t0 end
      done()
      return
    end
    reaper.defer(tick)
  end
  reaper.defer(tick)
end

local function render()
  if not CFG.render or #CFG.render == 0 then return end
  for i = 0, reaper.CountTracks(proj) - 1 do reaper.SetTrackSelected(reaper.GetTrack(proj, i), false) end
  local count = 0
  for _, name in ipairs(CFG.render) do
    local tr = (built[name] and built[name].tr) or trackByName(name)
    if tr then reaper.SetTrackSelected(tr, true) count = count + 1 end
  end
  results.renderTracks = count
  reaper.GetSetProjectInfo_String(proj, "RENDER_FILE", CFG.renderDir, true)
  reaper.GetSetProjectInfo_String(proj, "RENDER_PATTERN", "$track", true)
  -- WAV, 32-bit float: the renders are compared with our own host's float
  -- output, and a fixed-point file would be measuring its own quantiser.
  reaper.GetSetProjectInfo_String(proj, "RENDER_FORMAT", "ZXZhdyAAAQ==", true)
  -- Stems of the selected tracks only (3), each multichannel track to a file
  -- as wide as the track (&4) -- the splitter's aux buses are channels 3..6.
  reaper.GetSetProjectInfo(proj, "RENDER_SETTINGS", 3 | 4, true)
  reaper.GetSetProjectInfo(proj, "RENDER_BOUNDSFLAG", 0, true)
  reaper.GetSetProjectInfo(proj, "RENDER_STARTPOS", 0, true)
  reaper.GetSetProjectInfo(proj, "RENDER_ENDPOS", CFG.length, true)
  reaper.GetSetProjectInfo(proj, "RENDER_SRATE", CFG.srate, true)
  reaper.GetSetProjectInfo(proj, "RENDER_CHANNELS", 2, true)
  reaper.GetSetProjectInfo(proj, "RENDER_DITHER", 0, true)
  reaper.GetSetProjectInfo(proj, "RENDER_TAILFLAG", 0, true)
  reaper.GetSetProjectInfo(proj, "RENDER_ADDTOPROJ", 0, true)
  local t0 = now()
  -- File: Render project, using the most recent render settings, auto-close
  -- render dialog. Synchronous: it returns once the files are written.
  reaper.Main_OnCommand(42230, 0)
  results.renderSeconds = now() - t0
end

-- ── The editor session ──────────────────────────────────────────────────────
-- Open, wait for the window, (for the probe: wait for its page to drive a
-- parameter through the bridge), close, wait for it to go; a few times each.
local edQueue, edState = {}, nil
local function editorSetup()
  for _, e in ipairs(CFG.editor or {}) do
    local b = built[e.track]
    local entry = b and b.rec.fx[1]
    if entry and entry.added and entry.added >= 0 then
      for c = 1, e.cycles do edQueue[#edQueue + 1] = { e = e, tr = b.tr, fx = entry.added, cycle = c } end
    end
  end
end
local function editorStep()
  -- returns true while there is more to do
  if not edState then
    local job = table.remove(edQueue, 1)
    if not job then return false end
    reaper.TrackFX_Show(job.tr, job.fx, 3) -- floating window
    edState = { job = job, phase = "opening", t = now(),
                rec = { track = job.e.track, cycle = job.cycle } }
    results.editor[#results.editor + 1] = edState.rec
    flush()
    return true
  end
  local s, job = edState, edState.job
  local elapsed = now() - s.t
  if s.phase == "opening" then
    local hwnd = reaper.TrackFX_GetFloatingWindow(job.tr, job.fx)
    if hwnd and elapsed > 0.8 then
      s.rec.opened = true
      s.rec.openSeconds = elapsed
      s.phase = job.e.watchParam and job.cycle == 1 and "bridge" or "closing"
      s.t = now()
    elseif elapsed > 6 then
      s.rec.opened = false
      s.phase = "closing"
      s.t = now()
    end
  elseif s.phase == "bridge" then
    -- {param, normalized value[, second param, its value]}: the second pair
    -- is optional, for a format whose page cannot reach it (LV2: see the
    -- editor checks in the node script).
    local w = job.e.watchParam
    local v = reaper.TrackFX_GetParamNormalized(job.tr, job.fx, w[1])
    local g = w[3] and reaper.TrackFX_GetParamNormalized(job.tr, job.fx, w[3]) or 0
    if math.abs(v - w[2]) < 1e-3 and (not w[3] or math.abs(g - w[4]) < 1e-3) then
      s.rec.bridge = true
      s.rec.bridgeSeconds = elapsed
      s.phase = "closing"
      s.t = now()
    elseif elapsed > 10 then
      s.rec.bridge = false
      s.rec.bridgeValues = { v, g }
      s.phase = "closing"
      s.t = now()
    end
  elseif s.phase == "closing" then
    if not s.closeAsked then
      reaper.TrackFX_Show(job.tr, job.fx, 2)
      s.closeAsked = true
      s.t = now()
    elseif now() - s.t > 0.5 then
      s.rec.closed = reaper.TrackFX_GetFloatingWindow(job.tr, job.fx) == nil
      edState = nil
      flush()
    end
  end
  return true
end

-- ── The run ─────────────────────────────────────────────────────────────────
local function quit()
  results.stage = "done"
  flush()
  local d = io.open(CFG.done, "w")
  if d then d:write("done") d:close() end
  reaper.Main_OnCommand(40004, 0) -- File: Quit REAPER
end

local function waitThen(seconds, fn)
  local t0 = now()
  local function tick()
    if now() - t0 < seconds then reaper.defer(tick) return end
    fn()
  end
  reaper.defer(tick)
end

local function finish()
  if CFG.saveAs then stage("save", function() reaper.Main_SaveProjectEx(proj, CFG.saveAs, 0) end) end
  quit()
end

local function afterToggles()
  stage("render", render)
  if CFG.editor then
    stage("editor setup", editorSetup)
    local function loop()
      local ok, more = pcall(editorStep)
      if not ok then fail("editor", more) finish() return end
      if more then reaper.defer(loop) else finish() end
    end
    reaper.defer(loop)
  else
    finish()
  end
end

local function afterLanding()
  stage("read back", readBack)
  stage("toggle", toggles)
  results.stage = "latency switch"
  flush()
  watchToggles(afterToggles)
end

local function start()
  if CFG.open then
    stage("open", function()
      reaper.Main_openProject("noprompt:" .. CFG.open)
      adoptTracks()
    end)
  else
    stage("project", function()
      reaper.GetSetProjectInfo(proj, "PROJECT_SRATE", CFG.srate, true)
      reaper.GetSetProjectInfo(proj, "PROJECT_SRATE_USE", 1, true)
      reaper.SetCurrentBPM(proj, 120, false)
    end)
    stage("scan", function()
      local i = 0
      while true do
        local ok, name, ident = reaper.EnumInstalledFX(i)
        if not ok then break end
        if name:find("Sonore", 1, true) then results.installed[#results.installed + 1] = { name = name, ident = ident } end
        i = i + 1
      end
    end)
    for _, spec in ipairs(CFG.tracks) do stage("track " .. spec.name, buildTrack, spec) end
    stage("sends", buildSends)
  end
  waitThen(CFG.settle or 1.5, afterLanding)
end

start()
