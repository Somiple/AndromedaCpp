local P={}
P.plugin_name="Batch Note Edit"
P.plugin_type="manipulate"
P.dialog_fields={
    {
        type="label",
        label="Manipulates multiple notes at once."
    },
    {
        id="notes_tick",
        {
            type="textedit",
            label="Ticks (t)",
            value=""
        }
    },
    {
        id="notes_gate",
        {
            type="textedit",
            label="Gates (g)",
            value=""
        }
    },
    {
        id="notes_keys",
        {
            type="textedit",
            label="Keys (k)",
            value=""
        }
    },
    {
        id="notes_velocities",
        {
            type="textedit",
            label="Velocities (v)",
            value=""
        }
    },
    {
        id="notes_channels",
        {
            type="textedit",
            label="Channels (c)",
            value=""
        }
    },
    {},
    {
        type="label",
        label="Each field supports math operations. Go crazy with how you batch-edit the notes! The letters next to the labels are the variables."
    }
}
function compile_expr(expr)
    if not expr or expr:match("^%s*$") then return nil end
    local chunk, err=load( "return function(t,g,k,v,c,math) return "..expr.." end","expr","t")
    if not chunk then return nil end
    local ok,fn=pcall(chunk)
    if not ok then return nil end
    return fn
end
function on_apply(notes)
    local tick_chunk=compile_expr(get_field_value("notes_tick"))
    local gate_chunk=compile_expr(get_field_value("notes_gate"))
    local keys_chunk=compile_expr(get_field_value("notes_keys"))
    local velo_chunk=compile_expr(get_field_value("notes_velocities"))
    local chan_chunk=compile_expr(get_field_value("notes_channels"))
    notes:for_each_selected_all(function(note)
        local t=note.start
        local g=note.length
        local k=note.key
        local v=note.velocity
        local c=note.channel

        if tick_chunk~=nil then
            local ok,result=pcall(tick_chunk,t,g,k,v,c,math)
            if ok and result~=nil then
                note.start=result
            end
        end
        if gate_chunk~=nil then
            local ok,result=pcall(gate_chunk,t,g,k,v,c,math)
            if ok and result~=nil then
                note.length=result
            end
        end
        if keys_chunk~=nil then
            local ok,result=pcall(keys_chunk,t,g,k,v,c,math)
            if ok and result~=nil then
                if result>127 then result=127 end
                if result<0 then result=0 end
                note.key=result
            end
        end
        if velo_chunk~=nil then
            local ok,result=pcall(velo_chunk,t,g,k,v,c,math)
            if ok and result~=nil then
                if result>127 then result=127 end
                if result<1 then result=1 end
                note.velocity=result
            end
        end
        if chan_chunk~=nil then
            local ok,result=pcall(chan_chunk,t,g,k,v,c,math)
            if ok and result~=nil then
                if result>15 then result=15 end
                if result<0 then result=0 end
                note.channel=result
            end
        end
    end)
end
P.on_apply=on_apply
return P