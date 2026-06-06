-- tools : sirochro's bulk-edit extension for OFS
--
-- Install:
--   Copy this folder (extensions/tools) to:
--     %AppData%\OFS\OFS3_data\extensions\tools\
--   Then enable "tools" from the OFS "Extensions" menu.
--
-- Acts on the *current selection* of the active funscript. Pair with the
-- core 2D rectangle selection to pick a region, then use these panels.

-- ---------------------------------------------------------------------------
-- State
-- ---------------------------------------------------------------------------

SetPos = {}
SetPos.Value = 50

Optimize = {}
Optimize.Tolerance = 20
Optimize.Presets = { "None (wobble removal only)",
                     "0-100 (force endpoints)",
                     "QUATRO (4-point quartile)" }
Optimize.PresetIdx = 1   -- 1-based for ofs.Combo

-- ---------------------------------------------------------------------------
-- OFS extension entry points
-- ---------------------------------------------------------------------------

function init()
    print("tools loaded")
end

function update(delta)
    -- nothing periodic
end

function gui()
    render_set_position_panel()
    ofs.Separator()
    render_optimize_waves_panel()
end

-- ---------------------------------------------------------------------------
-- Set position
-- ---------------------------------------------------------------------------

function render_set_position_panel()
    ofs.Text("Set position")

    SetPos.Value, _ = ofs.SliderInt("Position##sp_slider", SetPos.Value, 0, 100)
    SetPos.Value, _ = ofs.InputInt("##sp_input", SetPos.Value)
    SetPos.Value = clamp(SetPos.Value, 0, 100)

    if ofs.Button("-10##sp_minus10") then
        SetPos.Value = clamp(SetPos.Value - 10, 0, 100)
    end
    ofs.SameLine()
    if ofs.Button("+10##sp_plus10") then
        SetPos.Value = clamp(SetPos.Value + 10, 0, 100)
    end

    if ofs.Button("All Btm##sp_btm") then apply_set_position(0)   end
    ofs.SameLine()
    if ofs.Button("All Mid##sp_mid") then apply_set_position(50)  end
    ofs.SameLine()
    if ofs.Button("All Top##sp_top") then apply_set_position(100) end

    if ofs.Button("Apply##sp_apply") then
        apply_set_position(SetPos.Value)
    end
end

function apply_set_position(value)
    local script = ofs.Script(ofs.ActiveIdx())
    if not script:hasSelection() then return end
    value = clamp(value, 0, 100)
    for idx, action in ipairs(script.actions) do
        if action.selected then
            action.pos = value
        end
    end
    script:commit()
end

-- ---------------------------------------------------------------------------
-- Optimize waves
-- ---------------------------------------------------------------------------

function render_optimize_waves_panel()
    ofs.Text("Optimize waves")

    Optimize.Tolerance, _ = ofs.SliderInt("Tolerance##ow_tol", Optimize.Tolerance, 0, 100)
    Optimize.Tolerance = clamp(Optimize.Tolerance, 0, 100)

    Optimize.PresetIdx, _ = ofs.Combo("Preset##ow_preset", Optimize.PresetIdx, Optimize.Presets)

    if ofs.Button("Apply##ow_apply") then
        apply_optimize_waves(Optimize.Tolerance, Optimize.PresetIdx)
    end
end

-- preset_idx: 1=None, 2=0-100, 3=QUATRO  (1-based, matches ofs.Combo)
function apply_optimize_waves(tolerance, preset_idx)
    local script = ofs.Script(ofs.ActiveIdx())
    if not script:hasSelection() then return end

    -- 1. Collect indices of selected actions in time order. The script's
    --    actions array is already sorted by time, so a single pass works.
    local sel_idx = {}
    for i, a in ipairs(script.actions) do
        if a.selected then sel_idx[#sel_idx + 1] = i end
    end
    if #sel_idx < 2 then return end

    -- 2. Snapshot positions/times of the selected actions for analysis.
    local sel = {}
    for i, ai in ipairs(sel_idx) do
        local a = script.actions[ai]
        sel[i] = { at = a.at, pos = a.pos, orig_idx = ai }
    end

    -- 3. Detect "significant" extrema. A reversal is significant only when
    --    the swing away from the running extreme exceeds tolerance, so
    --    small wobbles get absorbed and 0,60,40,100 reads as one wave.
    local keep = { 1 }
    local direction = 0      -- 0 = unknown, +1 = ascending, -1 = descending
    local run_idx = 1
    local run_pos = sel[1].pos

    for i = 2, #sel do
        local p = sel[i].pos
        if direction == 0 then
            if p > run_pos then
                direction = 1; run_idx = i; run_pos = p
            elseif p < run_pos then
                direction = -1; run_idx = i; run_pos = p
            end
        elseif direction > 0 then
            if p >= run_pos then
                run_idx = i; run_pos = p
            elseif (run_pos - p) > tolerance then
                if run_idx ~= keep[#keep] then keep[#keep + 1] = run_idx end
                direction = -1
                run_idx = i; run_pos = p
            end
        else
            if p <= run_pos then
                run_idx = i; run_pos = p
            elseif (p - run_pos) > tolerance then
                if run_idx ~= keep[#keep] then keep[#keep + 1] = run_idx end
                direction = 1
                run_idx = i; run_pos = p
            end
        end
    end
    if run_idx ~= keep[#keep] then keep[#keep + 1] = run_idx end
    if keep[#keep] ~= #sel then keep[#keep + 1] = #sel end

    -- 4. Build the output list (time, pos) per preset.
    local output = {}

    if preset_idx == 1 then
        -- None: keep significant extrema as-is.
        for _, idx in ipairs(keep) do
            output[#output + 1] = { at = sel[idx].at, pos = sel[idx].pos }
        end
    elseif preset_idx == 2 then
        -- 0-100: normalize each kept extremum to 0 or 100.
        for k, idx in ipairs(keep) do
            local a = sel[idx]
            local new_pos
            if k == 1 then
                if #keep > 1 then
                    new_pos = (a.pos < sel[keep[2]].pos) and 0 or 100
                else
                    new_pos = a.pos
                end
            elseif k == #keep then
                local prev_pos = sel[keep[k - 1]].pos
                new_pos = (a.pos > prev_pos) and 100 or 0
            else
                local prev_pos = sel[keep[k - 1]].pos
                local next_pos = sel[keep[k + 1]].pos
                new_pos = (a.pos > prev_pos and a.pos > next_pos) and 100 or 0
            end
            output[#output + 1] = { at = a.at, pos = new_pos }
        end
    else
        -- QUATRO: each 3+-point wave becomes 4 evenly time-spaced points
        -- whose profile is keyed off direction and the second sample:
        --   asc  2nd>=50: 0,75,55,100    asc  2nd<50: 0,25,45,100
        --   desc 2nd>=50: 100,75,55,0    desc 2nd<50: 100,25,45,0
        -- 2-point waves are kept as 0/100 endpoints (no wobble inserted).
        for w = 1, #keep - 1 do
            local s_idx = keep[w]
            local e_idx = keep[w + 1]
            local s_act = sel[s_idx]
            local e_act = sel[e_idx]
            local input_count = e_idx - s_idx + 1
            local ascending = e_act.pos > s_act.pos
            local out_start = ascending and 0 or 100
            local out_end   = ascending and 100 or 0

            if w == 1 then
                output[#output + 1] = { at = s_act.at, pos = out_start }
            end

            if input_count <= 2 then
                output[#output + 1] = { at = e_act.at, pos = out_end }
            else
                local second_pos = sel[s_idx + 1].pos
                local high_side = second_pos >= 50
                local p1, p2
                if high_side then p1 = 75; p2 = 55 else p1 = 25; p2 = 45 end
                local t0 = s_act.at
                local t3 = e_act.at
                local dt = (t3 - t0) / 3.0
                output[#output + 1] = { at = t0 + dt,       pos = p1 }
                output[#output + 1] = { at = t0 + 2.0 * dt, pos = p2 }
                output[#output + 1] = { at = t3,            pos = out_end }
            end
        end
    end

    -- 5. Remove the original selected actions, then add the outputs.
    --    Sort orig indices descending so removal-by-index stays valid
    --    while we iterate.
    local remove_list = {}
    for _, e in ipairs(sel) do remove_list[#remove_list + 1] = e.orig_idx end
    table.sort(remove_list, function(a, b) return a > b end)
    for _, idx in ipairs(remove_list) do
        script:markForRemoval(idx)
    end
    script:removeMarked()

    for _, o in ipairs(output) do
        script.actions:add(Action.new(o.at, o.pos, true))
    end

    script:sort()
    script:commit()
end
