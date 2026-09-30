local P = {}
P.plugin_name = "Arpeggio Generator"
P.plugin_type = "generate"
P.dialog_fields = {
    {
        id = "root_key",
        {
            type = "number",
            label = "Root note (0-127)",
            value = 60,
            range = {min=0, max=127}
        }
    },
    {
        id = "arp_type",
        {
            type = "dropdown",
            label = "Arpeggio type",
            value = 0,
            value_labels = {"Major","Minor","Diminished","Augmented","Major 7","Minor 7","Dominant 7"}
        }
    },
    {
        id = "length",
        {
            type = "number",
            label = "Note length (ticks)",
            value = 120,
            range = {min=1, max=1920}
        }
    },
    {
        id = "octaves",
        {
            type = "number",
            label = "Number of octaves",
            value = 1,
            range = {min=1, max=4}
        }
    },
    {
        id = "direction",
        {
            type = "dropdown",
            label = "Direction",
            value = 0,
            value_labels = {"Up","Down","Up-Down"}
        }
    },
    {}
}

-- chord intervals
local ARP_INTERVALS = {
    ["Major"] = {0,4,7},
    ["Minor"] = {0,3,7},
    ["Diminished"] = {0,3,6},
    ["Augmented"] = {0,4,8},
    ["Major 7"] = {0,4,7,11},
    ["Minor 7"] = {0,3,7,10},
    ["Dominant 7"] = {0,4,7,10}
}

function on_apply(notes)
    local root = get_field_value("root_key")
    local arp_type_index = get_field_value("arp_type")
    local arp_types = {"Major","Minor","Diminished","Augmented","Major 7","Minor 7","Dominant 7"}
    local arp_type = arp_types[arp_type_index+1]
    local intervals = ARP_INTERVALS[arp_type]
    local note_len = get_field_value("length")
    local octs = get_field_value("octaves")
    local dir_index = get_field_value("direction")
    local dir_labels = {"Up","Down","Up-Down"}
    local direction = dir_labels[dir_index+1]

    local start_tick = andromeda:get_playhead_tick_pos()

    -- build the arpeggio notes sequence
    local sequence = {}
    for o=0,octs-1 do
        for i=1,#intervals do
            local k = root + intervals[i] + o*12
            if k <= 127 then
                table.insert(sequence,k)
            end
        end
    end

    if direction == "Down" then
        local rev_seq = {}
        for i=#sequence,1,-1 do table.insert(rev_seq,sequence[i]) end
        sequence = rev_seq
    elseif direction == "Up-Down" then
        local rev_seq = {}
        for i=#sequence-1,2,-1 do table.insert(rev_seq,sequence[i]) end
        for i=1,#rev_seq do table.insert(sequence,rev_seq[i]) end
    end

    -- create notes
    local tick = start_tick
    for i=1,#sequence do
        notes:create_note(tick,note_len,0,sequence[i],100)
        tick = tick + note_len
    end
end

P.on_apply = on_apply
return P