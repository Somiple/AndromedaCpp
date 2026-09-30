local P={}
P.plugin_name="Humanize"
P.plugin_type="manipulate"
P.dialog_fields={
    {
        type="label",
        label="Start Range Settings"
    },
    {
        type="label"
    },
    {
        id="start_range_min",
        {
            type="number",
            label="Start Range (-)",
            value=0,
            range={
                min=-60,
                max=60
            }
        }
    },
    {
        id="start_range_max",
        {
            type="number",
            label="Start Range (+)",
            value=60,
            range={
                min=-60,
                max=60
            }
        }
    },
    {},
    {
        type="label",
        label="Length Range Settings"
    },
    {
        type="label"
    },
    {
        id="length_range_min",
        {
            type="number",
            label="Length Range % (-)",
            value=90,
            range={
                min=50,
                max=200
            }
        }
    },
    {
        id="length_range_max",
        {
            type="number",
            label="Length Range % (+)",
            value=110,
            range={
                min=50,
                max=200
            }
        }
    },
    {},
    {
        type="label",
        label="Velocity Range Settings"
    },
    {
        type="label"
    },
    {
        id="vel_range_min",
        {
            type="number",
            label="Velocity Range % (-)",
            value=90,
            range={
                min=50,
                max=200
            }
        }
    },
    {
        id="vel_range_max",
        {
            type="number",
            label="Velocity Range % (+)",
            value=100,
            range={
                min=50,
                max=200
            }
        }
    }
}
function on_apply(notes)
    local start_range_min=get_field_value("start_range_min")
    local start_range_max=get_field_value("start_range_max")
    local length_range_min=get_field_value("length_range_min")
    local length_range_max=get_field_value("length_range_max")
    local vel_range_min=get_field_value("vel_range_min")
    local vel_range_max=get_field_value("vel_range_max")
    if start_range_min>start_range_max then
        local tmp=start_range_min
        start_range_min=start_range_max
        start_range_max=tmp
    end
    if length_range_min>length_range_max then
        local tmp=length_range_min
        length_range_min=length_range_max
        length_range_max=tmp
    end
    if vel_range_min>vel_range_max then
        local tmp=vel_range_min
        vel_range_min=vel_range_max
        vel_range_max=tmp
    end
    notes:for_each_selected(function(note)
        local rnd_start=math.random(start_range_min,start_range_max)
        local new_start=math.max(note.start+rnd_start,0)
        note.start=new_start
        local rnd_length=math.random(length_range_min,length_range_max)
        local new_length=note.length*(rnd_length/100.0)
        note.length=new_length
        local rnd_vel=math.random(vel_range_min,vel_range_max)
        local new_vel=note.velocity*(rnd_vel/100.0)
        note.velocity=new_vel
    end)
end
P.on_apply=on_apply
return P