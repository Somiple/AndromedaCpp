local P={}
P.plugin_name="Flip X"
P.plugin_type="manipulate"
P.dialog_fields={}
function on_apply(notes)
    local tick_range=notes:get_selection_tick_range(false)
    if tick_range==nil then return end
    local min_tick=tick_range.min
    local max_tick=tick_range.max
    notes:for_each_selected(function(note)
        note.start=max_tick-note.start+min_tick
    end)
end
P.on_apply=on_apply
return P