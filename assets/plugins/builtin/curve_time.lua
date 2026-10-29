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
local function round(num)
    return num >= 0 and math.floor(num + 0.5) or math.ceil(num - 0.5)
end
local function curve_time(x,k)
    x=math.max(0,math.min(1,x))
    k=math.max(0.00001,math.min(0.99999,k))
    return (x*k)/(x*k+(1-x)*(1-k))
end
local function remap_tick(tick,sel_start,sel_end,k)
    local range=sel_end-sel_start
    if range<=0 then
        return sel_start
    end
    local x=(tick-sel_start)/range
    return sel_start+curve_time(x,k)*range
end
function on_apply(notes)
    local note_range=notes:get_selection_tick_range(true)
    local sel_start=note_range.min
    local sel_end=note_range.max
    local k=get_field_value("k_const")
    if sel_end<=sel_start then return end
    notes:for_each_selected(function (note)
        local old_start=note.start
        local old_end=note.start+note.length
        local new_start=round(remap_tick(old_start,sel_start,sel_end,k))
        local new_end=round(remap_tick(old_end,sel_start,sel_end,k))
        note.start=new_start
        note.length=math.max(new_end-new_start,1)
    end)
end
P.on_apply=on_apply
return P