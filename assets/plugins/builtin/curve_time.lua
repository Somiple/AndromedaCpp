local P={}
P.plugin_name="Curve Time"
P.plugin_type="manipulate"
P.dialog_fields={
    {},
    {
        id="k_const",
        {
            type="slider",
            label="k factor (0 to 1)",
            value=0.5,
            range={
                min=0.00001,
                max=0.99999
            }
        }
    },
    {
        type="label",
        label="If k is 0, then all notes will just move to the start of the selection. If k is 0.5, then notes stay where they are."
    }
}
local function pow(x,k)
    return x^k
end
local function exp_bias(x,k)
    return pow(x,math.log(k)/math.log(0.5))
end
local function remap_range(x,min,max)
    return (x-min)/(max-min)
end
local function round(num)
    return num >= 0 and math.floor(num + 0.5) or math.ceil(num - 0.5)
end
function on_apply(notes)
    local note_range=notes:get_selection_tick_range(true)
    local sel_start=note_range.min
    local sel_end=note_range.max
    local k=get_field_value("k_const")
    local exponent=math.log(k)/math.log(0.5)
    notes:for_each_selected(function (note)
        local note_start=note.start
        local note_end=note.start+note.length
        local new_note_start=pow(remap_range(note_start,sel_start,sel_end),exponent)
        new_note_start=sel_start+new_note_start*(sel_end-sel_start)
        local new_note_end=pow(remap_range(note_end,sel_start,sel_end),exponent)
        new_note_end=sel_start+new_note_end*(sel_end-sel_start)
        note.start=round(new_note_start)
        note.length=round(math.max(new_note_end-new_note_start,1))
    end)
end
P.on_apply=on_apply
return P