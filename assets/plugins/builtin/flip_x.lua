local P={}
P.plugin_name="Flip X (Tick-wise)"
P.plugin_type="manipulate"
P.dialog_fields={}
function on_apply(notes)
    local tick_range=notes:get_selection_tick_range(true)
    if tick_range==nil then return end
    local min_tick=tick_range.min
    local max_tick=tick_range.max
    notes:for_each_selected(function(note)
        local e=note.start+note.length
        local n_s=max_tick-e+min_tick
        note.start=n_s
    end)
end
P.on_apply=on_apply
return P