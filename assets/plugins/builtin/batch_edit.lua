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
    {},
    {
        type="label",
        label="Each field supports math operations. Go crazy with how you batch-edit the notes! The letters next to the labels are the variables."
    }
}
function compile_expr(expr)
    if not expr or expr:match("^%s*$") then return nil end
    local chunk,err=load("return "..expr,"expr","t")
    if not chunk then return nil end
    return chunk
end
function on_apply(notes)
    local tick_chunk=compile_expr(get_field_value("notes_tick"))
    local gate_chunk=compile_expr(get_field_value("notes_gate"))
    local keys_chunk=compile_expr(get_field_value("notes_keys"))
    local velo_chunk=compile_expr(get_field_value("notes_velocities"))
    notes:for_each_selected(function(note)
        local env={
            t=note.start,
            g=note.length,
            k=note.key,
            v=note.velocity,
            math=math
        }
        if tick_chunk~=nil then
            setfenv(tick_chunk,env)
            local ok,result=pcall(tick_chunk)
            if ok and result~=nil then
                note.start=result
            end
        end
        if gate_chunk~=nil then
            setfenv(gate_chunk,env)
            local ok,result=pcall(gate_chunk)
            if ok and result~=nil then
                note.length=result
            end
        end
        if keys_chunk~=nil then
            setfenv(keys_chunk,env)
            local ok,result=pcall(keys_chunk)
            if ok and result~=nil then
                if result>127 then result=127 end
                if result<0 then result=0 end
                note.key=result
            end
        end
        if velo_chunk~=nil then
            setfenv(velo_chunk,env)
            local ok,result=pcall(velo_chunk)
            if ok and result~=nil then
                if result>127 then result=127 end
                if result<1 then result=1 end
                note.velocity=result
            end
        end
    end)
end
P.on_apply=on_apply
return P