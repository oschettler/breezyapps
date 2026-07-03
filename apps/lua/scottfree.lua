-- scottfree.lua: Scott Adams Adventure Interpreter
-- Usage: lua scottfree.lua <datafile.dat>

-------------------------------------------------------------------------------
-- Tokenizer
-------------------------------------------------------------------------------

local function tokenize(filename)
    local f = io.open(filename, "r")
    if not f then
        error("Cannot open file: " .. filename)
    end
    local content = f:read("*a")
    f:close()

    local tokens = {}
    local i = 1
    local len = #content

    while i <= len do
        local c = content:sub(i, i)
        if c == '"' then
            -- String token: read until next "
            local j = i + 1
            local s = {}
            while j <= len do
                local cc = content:sub(j, j)
                if cc == '"' then
                    j = j + 1
                    break
                end
                s[#s+1] = cc
                j = j + 1
            end
            tokens[#tokens+1] = { kind = "string", value = table.concat(s) }
            i = j
        elseif c:match("%d") or (c == '-' and i < len and content:sub(i+1,i+1):match("%d")) then
            -- Number token
            local j = i + 1
            while j <= len and content:sub(j,j):match("[%d]") do
                j = j + 1
            end
            tokens[#tokens+1] = { kind = "number", value = tonumber(content:sub(i, j-1)) }
            i = j
        else
            i = i + 1
        end
    end

    return tokens
end

-------------------------------------------------------------------------------
-- Data Loader
-------------------------------------------------------------------------------

local function load_game(filename)
    local tokens = tokenize(filename)
    local pos = 1

    local function next_num()
        while pos <= #tokens and tokens[pos].kind ~= "number" do
            pos = pos + 1
        end
        if pos > #tokens then error("Expected number but ran out of tokens") end
        local v = tokens[pos].value
        pos = pos + 1
        return v
    end

    local function next_str()
        while pos <= #tokens and tokens[pos].kind ~= "string" do
            pos = pos + 1
        end
        if pos > #tokens then error("Expected string but ran out of tokens") end
        local v = tokens[pos].value
        pos = pos + 1
        return v
    end

    local game = {}

    -- Header: 12 integers
    local hdr = {}
    for i = 1, 12 do hdr[i] = next_num() end
    game.hdr = hdr
    game.num_items      = hdr[2]
    game.num_actions    = hdr[3]
    game.num_words      = hdr[4]
    game.num_rooms      = hdr[5]
    game.max_carry      = hdr[6]
    game.starting_room  = hdr[7]
    game.num_treasures  = hdr[8]
    game.word_length    = hdr[9]
    game.light_time_max = hdr[10]
    game.num_messages   = hdr[11]
    game.treasure_room  = hdr[12]

    -- Actions: (num_actions+1) * 8 integers
    game.actions = {}
    for i = 0, game.num_actions do
        local a = {}
        for j = 1, 8 do a[j] = next_num() end
        game.actions[i] = a
    end

    -- Vocabulary: (num_words+1) * 2 strings (verb, noun pairs)
    game.verbs = {}
    game.nouns = {}
    for i = 0, game.num_words do
        game.verbs[i] = next_str()
        game.nouns[i] = next_str()
    end

    -- Rooms: (num_rooms+1) entries: 6 exits + 1 string
    game.rooms = {}
    for i = 0, game.num_rooms do
        local r = { exits = {} }
        for d = 1, 6 do r.exits[d] = next_num() end
        r.desc = next_str()
        game.rooms[i] = r
    end

    -- Messages: (num_messages+1) strings
    game.messages = {}
    for i = 0, game.num_messages do
        game.messages[i] = next_str()
    end

    -- Items: (num_items+1) entries: 1 string + 1 integer
    game.items = {}
    for i = 0, game.num_items do
        local it = {}
        it.desc = next_str()
        -- Parse auto-get noun from /NOUN/ suffix
        it.auto_noun = it.desc:match("/([^/]+)/$")
        if it.auto_noun then
            it.display_desc = it.desc:match("^(.+)/[^/]+/$")
        else
            it.display_desc = it.desc
        end
        it.start_room = next_num()
        game.items[i] = it
    end

    -- Action comments: (num_actions+1) strings (skip)
    for i = 0, game.num_actions do
        next_str()
    end

    -- Footer: 3 integers
    game.version        = next_num()
    game.adventure_num  = next_num()
    game.checksum       = next_num()

    return game
end

-------------------------------------------------------------------------------
-- Game State
-------------------------------------------------------------------------------

local function init_state(game)
    local st = {}
    st.current_room = game.starting_room
    st.item_locations = {}
    for i = 0, game.num_items do
        local loc = game.items[i].start_room
        -- 255 in file means carried (-1)
        if loc == 255 then loc = -1 end
        st.item_locations[i] = loc
    end
    st.flags = {}
    for i = 0, 31 do st.flags[i] = false end
    st.counters = {}
    for i = 0, 15 do st.counters[i] = 0 end
    st.current_counter = 0
    st.alt_rooms = {}
    for i = 0, 5 do st.alt_rooms[i] = 0 end
    st.light_time = game.light_time_max
    st.game_over = false
    st.need_look = true
    return st
end

-------------------------------------------------------------------------------
-- Vocabulary Lookup
-------------------------------------------------------------------------------

local function match_word(input, word, wlen)
    if not word or word == "" then return false end
    local w = word
    if w:sub(1,1) == "*" then w = w:sub(2) end
    local ilen = math.min(#input, wlen)
    local wlen2 = math.min(#w, wlen)
    return input:sub(1,ilen):upper() == w:sub(1,wlen2):upper()
end

local function find_verb(game, word)
    if not word or word == "" then return nil end
    local wlen = game.word_length
    for i = 0, game.num_words do
        local v = game.verbs[i]
        if v and v ~= "" and v:sub(1,1) ~= "*" then
            if match_word(word, v, wlen) then
                return i
            end
        else
            -- synonym: shares same index as previous, check anyway
            if v and v:sub(1,1) == "*" then
                if match_word(word, v:sub(2), wlen) then
                    -- find the base index (last non-* before this)
                    for j = i-1, 0, -1 do
                        if game.verbs[j] and game.verbs[j]:sub(1,1) ~= "*" then
                            return j
                        end
                    end
                    return i
                end
            end
        end
    end
    return nil
end

local function find_noun(game, word)
    if not word or word == "" then return nil end
    local wlen = game.word_length
    for i = 0, game.num_words do
        local n = game.nouns[i]
        if n and n ~= "" then
            local base = n
            if base:sub(1,1) == "*" then base = base:sub(2) end
            if match_word(word, base, wlen) then
                -- return canonical index (first non-synonym)
                if n:sub(1,1) == "*" then
                    for j = i-1, 0, -1 do
                        if game.nouns[j] and game.nouns[j] ~= "" and game.nouns[j]:sub(1,1) ~= "*" then
                            return j
                        end
                    end
                end
                return i
            end
        end
    end
    return nil
end

-------------------------------------------------------------------------------
-- Room Description / LOOK
-------------------------------------------------------------------------------

local DIRS = {"North","South","East","West","Up","Down"}

local function find_active_lamp(game, st)
    for i = 0, game.num_items do
        local it = game.items[i]
        local lit = false
        if it.auto_noun and it.auto_noun:upper():find("LAMP") then
            lit = true
        elseif it.desc:upper():find("WITH LIGHT") or it.desc:upper():find("LAMP.*LIT") then
            lit = true
        end
        if lit then
            local loc = st.item_locations[i]
            if loc == -1 or loc == st.current_room then
                return i
            end
        end
    end
    return nil
end

local function is_dark(game, st)
    if not st.flags[15] then return false end
    return find_active_lamp(game, st) == nil
end

local function look(game, st)
    if is_dark(game, st) then
        print("It's too dark to see!")
        return
    end

    local room = game.rooms[st.current_room]
    local desc = room.desc
    -- Room descriptions starting with * mean clear screen
    if desc:sub(1,1) == "*" then
        print("----------------------------------------")
        desc = desc:sub(2)
    end
    print("I'm in " .. desc)
    print("")

    -- List items in this room
    local items_here = {}
    for i = 0, game.num_items do
        if st.item_locations[i] == st.current_room then
            local d = game.items[i].display_desc
            if d and d ~= "" then
                items_here[#items_here+1] = d
            end
        end
    end
    if #items_here > 0 then
        print("Visible items:")
        for _, d in ipairs(items_here) do
            print("  " .. d)
        end
        print("")
    end

    -- Show exits
    local exits = {}
    for d = 1, 6 do
        if room.exits[d] ~= 0 then
            exits[#exits+1] = DIRS[d]
        end
    end
    if #exits > 0 then
        print("Exits: " .. table.concat(exits, "  "))
    else
        print("Exits: none")
    end
    print("")
end

-------------------------------------------------------------------------------
-- Input Parser
-------------------------------------------------------------------------------

local function parse_input(game, line)
    line = line:match("^%s*(.-)%s*$")  -- trim
    if line == "" then return nil, nil end

    local parts = {}
    for w in line:gmatch("%S+") do parts[#parts+1] = w end

    local verb_word = parts[1]
    local noun_word = parts[2]

    local verb_idx = find_verb(game, verb_word)
    local noun_idx = nil
    if noun_word then
        noun_idx = find_noun(game, noun_word)
    end

    return verb_idx, noun_idx, verb_word, noun_word
end

-------------------------------------------------------------------------------
-- Condition Checker
-------------------------------------------------------------------------------

local function check_condition(game, st, cond, params)
    local code = cond % 20
    local val  = math.floor(cond / 20)

    if code == 0 then
        -- Par: collect parameter value (val), always true
        params[#params+1] = val
        return true
    elseif code == 1 then
        return st.item_locations[val] == -1
    elseif code == 2 then
        return st.item_locations[val] == st.current_room
    elseif code == 3 then
        return st.item_locations[val] == -1 or st.item_locations[val] == st.current_room
    elseif code == 4 then
        return st.current_room == val
    elseif code == 5 then
        return st.item_locations[val] ~= st.current_room
    elseif code == 6 then
        return st.item_locations[val] ~= -1
    elseif code == 7 then
        return st.current_room ~= val
    elseif code == 8 then
        return st.flags[val] == true
    elseif code == 9 then
        return st.flags[val] ~= true
    elseif code == 10 then
        -- Carrying something
        for i = 0, game.num_items do
            if st.item_locations[i] == -1 then return true end
        end
        return false
    elseif code == 11 then
        -- Not carrying anything
        for i = 0, game.num_items do
            if st.item_locations[i] == -1 then return false end
        end
        return true
    elseif code == 12 then
        return st.item_locations[val] ~= st.current_room
    elseif code == 13 then
        return st.item_locations[val] ~= 0
    elseif code == 14 then
        return st.item_locations[val] == 0
    elseif code == 15 then
        return st.counters[st.current_counter] <= val
    elseif code == 16 then
        return st.counters[st.current_counter] >= val
    elseif code == 17 then
        return st.item_locations[val] == game.items[val].start_room
    elseif code == 18 then
        return st.item_locations[val] ~= game.items[val].start_room
    elseif code == 19 then
        return st.counters[st.current_counter] == val
    end
    return true
end

local function check_conditions(game, st, action)
    local params = {}
    for c = 2, 6 do
        local cond = action[c]
        if cond ~= 0 then
            if not check_condition(game, st, cond, params) then
                return false, {}
            end
        end
    end
    return true, params
end

-- Collect Par parameters (code=0) from condition slots
local function collect_params(action)
    local params = {}
    for c = 2, 6 do
        local cond = action[c]
        if cond ~= 0 and (cond % 20) == 0 then
            params[#params+1] = math.floor(cond / 20)
        end
    end
    return params
end

-------------------------------------------------------------------------------
-- Action Executor
-------------------------------------------------------------------------------

local function carry_count(game, st)
    local n = 0
    for i = 0, game.num_items do
        if st.item_locations[i] == -1 then n = n + 1 end
    end
    return n
end

local function count_stored_treasures(game, st)
    local count = 0
    for i = 0, game.num_items do
        if game.items[i].desc:sub(1,1) == "*" then
            if st.item_locations[i] == game.treasure_room then
                count = count + 1
            end
        end
    end
    return count
end

local function get_inventory_lines(game, st)
    local carried = {}
    for i = 0, game.num_items do
        if st.item_locations[i] == -1 then
            carried[#carried+1] = game.items[i].display_desc
        end
    end
    return carried
end

local function find_item_by_noun(game, st, noun_idx, require_room)
    if not noun_idx then return nil end
    for i = 0, game.num_items do
        local it = game.items[i]
        if it.auto_noun then
            local n_idx = find_noun(game, it.auto_noun)
            if n_idx == noun_idx then
                if require_room then
                    if st.item_locations[i] == st.current_room then
                        return i
                    end
                else
                    if st.item_locations[i] == -1 then
                        return i
                    end
                end
            end
        end
    end
    return nil
end

-- Returns: "continue" (chain next), "done", or "gameover"
local function execute_action_codes(game, st, action, noun_idx, output_buf)
    local params = collect_params(action)
    local par_pos = 1

    local function get_par()
        local v = params[par_pos] or 0
        par_pos = par_pos + 1
        return v
    end

    local function print_msg(s)
        output_buf[#output_buf+1] = s
    end

    -- Decode 4 action codes from action_pair1 and action_pair2
    local codes = {}
    local ap1 = action[7]
    local ap2 = action[8]
    codes[1] = math.floor(ap1 / 150)
    codes[2] = ap1 % 150
    codes[3] = math.floor(ap2 / 150)
    codes[4] = ap2 % 150

    local result = "done"

    for _, code in ipairs(codes) do
        if code == 0 then
            -- no-op
        elseif code >= 1 and code <= 51 then
            print_msg(game.messages[code] or "")
        elseif code == 52 then
            -- GET item by noun
            local item_i = find_item_by_noun(game, st, noun_idx, true)
            if not item_i then
                print_msg("I don't see that here.")
            elseif carry_count(game, st) >= game.max_carry then
                print_msg("I'm carrying too much.")
            else
                st.item_locations[item_i] = -1
                print_msg("OK.")
            end
        elseif code == 53 then
            -- DROP item by noun
            local item_i = find_item_by_noun(game, st, noun_idx, false)
            if not item_i then
                print_msg("I'm not carrying that.")
            else
                st.item_locations[item_i] = st.current_room
                print_msg("OK.")
            end
        elseif code == 54 then
            local dest = get_par()
            st.current_room = dest
            st.need_look = true
        elseif code == 55 then
            local item_i = get_par()
            st.item_locations[item_i] = 0
        elseif code == 56 then
            st.flags[15] = true
        elseif code == 57 then
            st.flags[15] = false
        elseif code == 58 then
            local flag_n = get_par()
            st.flags[flag_n] = true
        elseif code == 59 then
            local item_i = get_par()
            st.item_locations[item_i] = 0
        elseif code == 60 then
            local flag_n = get_par()
            st.flags[flag_n] = false
        elseif code == 61 then
            -- DEAD
            print_msg("I'm dead...")
            st.flags[15] = false
            st.current_room = game.num_rooms  -- last room = dead room
            st.need_look = true
        elseif code == 62 then
            local item_i = get_par()
            local dest   = get_par()
            st.item_locations[item_i] = dest
        elseif code == 63 then
            print_msg("The game is over.")
            st.game_over = true
            result = "gameover"
        elseif code == 64 or code == 76 then
            st.need_look = true
        elseif code == 65 then
            local count = count_stored_treasures(game, st)
            print_msg(string.format("I've stored %d of %d treasures.", count, game.num_treasures))
        elseif code == 66 then
            local carried = get_inventory_lines(game, st)
            if #carried == 0 then
                print_msg("I'm not carrying anything.")
            else
                print_msg("I'm carrying:")
                for _, d in ipairs(carried) do
                    print_msg("  " .. d)
                end
            end
        elseif code == 67 then
            st.flags[0] = true
        elseif code == 68 then
            st.flags[0] = false
        elseif code == 69 then
            -- Refill lamp
            st.light_time = game.light_time_max
            st.flags[16] = false
        elseif code == 70 then
            print_msg("----------------------------------------")
        elseif code == 71 then
            -- SAVE - handled externally by returning special marker
            print_msg("[SAVE]")
        elseif code == 72 then
            local item_a = get_par()
            local item_b = get_par()
            local tmp = st.item_locations[item_a]
            st.item_locations[item_a] = st.item_locations[item_b]
            st.item_locations[item_b] = tmp
        elseif code == 73 then
            result = "continue"
        elseif code == 74 then
            -- SUPERGET ignores carry limit
            local item_i = find_item_by_noun(game, st, noun_idx, true)
            if item_i then
                st.item_locations[item_i] = -1
                print_msg("OK.")
            else
                print_msg("I don't see that here.")
            end
        elseif code == 75 then
            local item_a = get_par()
            local item_b = get_par()
            st.item_locations[item_a] = st.item_locations[item_b]
        elseif code == 77 then
            st.counters[st.current_counter] = st.counters[st.current_counter] - 1
        elseif code == 78 then
            print_msg(tostring(st.counters[st.current_counter]))
        elseif code == 79 then
            st.counters[st.current_counter] = get_par()
        elseif code == 80 then
            local tmp = st.current_room
            st.current_room = st.alt_rooms[0]
            st.alt_rooms[0] = tmp
            st.need_look = true
        elseif code == 81 then
            st.current_counter = get_par()
        elseif code == 82 then
            st.counters[st.current_counter] = st.counters[st.current_counter] + get_par()
        elseif code == 83 then
            st.counters[st.current_counter] = st.counters[st.current_counter] - get_par()
        elseif code == 84 then
            -- print noun (no newline) -- just append to last output
            if noun_idx then
                local n = game.nouns[noun_idx] or ""
                if n:sub(1,1) == "*" then n = n:sub(2) end
                if #output_buf > 0 then
                    output_buf[#output_buf] = output_buf[#output_buf] .. n
                else
                    output_buf[#output_buf+1] = n
                end
            end
        elseif code == 85 then
            if noun_idx then
                local n = game.nouns[noun_idx] or ""
                if n:sub(1,1) == "*" then n = n:sub(2) end
                print_msg(n)
            end
        elseif code == 86 then
            print_msg("")
        elseif code == 87 then
            local alt = get_par()
            local tmp = st.current_room
            st.current_room = st.alt_rooms[alt] or 0
            st.alt_rooms[alt] = tmp
            st.need_look = true
        elseif code >= 88 then
            local alt = code - 88
            st.alt_rooms[alt] = st.current_room
        end
    end

    return result
end

-------------------------------------------------------------------------------
-- Run Actions
-------------------------------------------------------------------------------

local function flush_output(buf)
    for _, line in ipairs(buf) do
        print(line)
    end
end

-- Try to execute actions matching verb/noun; return true if any fired
local function run_actions(game, st, verb_idx, noun_idx)
    local fired = false
    local i = 0
    local chain_depth = 0
    while i <= game.num_actions do
        local action = game.actions[i]
        local av = math.floor(action[1] / 150)
        local an = action[1] % 150

        local matches = false
        if av == verb_idx then
            if an == 0 or an == noun_idx then
                matches = true
            end
        end

        if matches then
            local ok, _ = check_conditions(game, st, action)
            if ok then
                local buf = {}
                local result = execute_action_codes(game, st, action, noun_idx, buf)
                flush_output(buf)
                fired = true
                if result == "continue" then
                    chain_depth = chain_depth + 1
                    if chain_depth > game.num_actions then break end
                    i = i + 1
                elseif result == "gameover" then
                    return fired
                else
                    return fired
                end
            else
                i = i + 1
            end
        else
            i = i + 1
        end
    end
    return fired
end

-- Run automatic actions (verb=0) each turn
local function run_automatic_actions(game, st)
    local i = 0
    while i <= game.num_actions do
        local action = game.actions[i]
        local av = math.floor(action[1] / 150)
        local an = action[1] % 150

        if av == 0 then
            local fire = (an == 0) or (math.random(100) <= an)
            if fire then
                local ok, _ = check_conditions(game, st, action)
                if ok then
                    local buf = {}
                    local result = execute_action_codes(game, st, action, nil, buf)
                    flush_output(buf)
                    if result == "continue" then
                        i = i + 1
                    elseif result == "gameover" then
                        return
                    else
                        i = i + 1
                    end
                else
                    i = i + 1
                end
            else
                i = i + 1
            end
        else
            i = i + 1
        end
    end
end

-------------------------------------------------------------------------------
-- Built-in GO direction
-------------------------------------------------------------------------------

local DIR_NAMES_UPPER = {
    ["NORTH"]=1, ["N"]=1,
    ["SOUTH"]=2, ["S"]=2,
    ["EAST"]=3,  ["E"]=3,
    ["WEST"]=4,  ["W"]=4,
    ["UP"]=5,    ["U"]=5,
    ["DOWN"]=6,  ["D"]=6,
}

local function do_go(game, st, dir_idx)
    local room = game.rooms[st.current_room]
    local dest = room.exits[dir_idx]
    if dest == 0 then
        print("I can't go that way.")
    else
        st.current_room = dest
        st.need_look = true
    end
end

-------------------------------------------------------------------------------
-- Save / Load
-------------------------------------------------------------------------------

local function save_game(game, st, filename)
    local f = io.open(filename, "w")
    if not f then
        print("Can't save: couldn't open " .. filename)
        return
    end
    f:write(tostring(st.current_room) .. "\n")
    -- item locations
    for i = 0, game.num_items do
        f:write(tostring(st.item_locations[i] or 0) .. "\n")
    end
    f:write("---\n")
    -- flags
    for i = 0, 31 do
        f:write((st.flags[i] and "1" or "0") .. "\n")
    end
    f:write("---\n")
    -- counters
    for i = 0, 15 do
        f:write(tostring(st.counters[i] or 0) .. "\n")
    end
    f:write(tostring(st.current_counter) .. "\n")
    f:write("---\n")
    -- alt_rooms
    for i = 0, 5 do
        f:write(tostring(st.alt_rooms[i] or 0) .. "\n")
    end
    f:write(tostring(st.light_time) .. "\n")
    f:close()
    print("Game saved.")
end

local function load_save(game, st, filename)
    local f = io.open(filename, "r")
    if not f then return false end
    local lines = {}
    for line in f:lines() do
        lines[#lines+1] = line
    end
    f:close()

    local pos = 1
    local function next_line()
        local l = lines[pos]; pos = pos + 1
        return l
    end

    st.current_room = tonumber(next_line())
    for i = 0, game.num_items do
        local l = next_line()
        if l == "---" then break end
        st.item_locations[i] = tonumber(l) or 0
    end
    -- skip to next ---
    while lines[pos] ~= "---" and pos <= #lines do pos = pos + 1 end
    pos = pos + 1

    for i = 0, 31 do
        local l = next_line()
        if l == "---" then break end
        st.flags[i] = (l == "1")
    end
    while lines[pos] ~= "---" and pos <= #lines do pos = pos + 1 end
    pos = pos + 1

    for i = 0, 15 do
        local l = next_line()
        if l == "---" then break end
        st.counters[i] = tonumber(l) or 0
    end
    st.current_counter = tonumber(next_line()) or 0
    while lines[pos] ~= "---" and pos <= #lines do pos = pos + 1 end
    pos = pos + 1

    for i = 0, 5 do
        local l = next_line()
        if l == "---" then break end
        st.alt_rooms[i] = tonumber(l) or 0
    end
    local lt = next_line()
    if lt and lt ~= "---" then
        st.light_time = tonumber(lt) or 0
    end

    st.need_look = true
    return true
end

-------------------------------------------------------------------------------
-- Main Game Loop
-------------------------------------------------------------------------------

local function readline()
    io.write("> ")
    io.flush()
    local line = io.read("*l")
    if not line then
        -- fallback: read char by char
        local chars = {}
        local max_iters = 4096
        for _ = 1, max_iters do
            local c = io.read(1)
            if not c or c == "\n" or c == "" then break end
            chars[#chars+1] = c
        end
        line = table.concat(chars)
    end
    return line
end

local function main()
    local datfile = arg and arg[1]
    if not datfile then
        print("Usage: lua scottfree.lua <datafile.dat>")
        os.exit(1)
    end

    print("Loading " .. datfile .. "...")
    local game = load_game(datfile)
    local st = init_state(game)

    local savefile = datfile .. ".sav"
    local f = io.open(savefile, "r")
    if f then
        f:close()
        print("Save file found. Restore? (Y/N)")
        local ans = readline()
        if ans:upper():sub(1,1) == "Y" then
            if load_save(game, st, savefile) then
                print("Game restored.")
            end
        end
    end

    print("")
    print("Scott Free Lua - Adventure " .. (game.adventure_num or "?"))
    print("Type HELP for hints, QUIT to quit, SAVE/LOAD to save/load.")
    print("")

    while not st.game_over do
        -- Run automatic actions
        run_automatic_actions(game, st)

        if st.game_over then break end

        -- LOOK if needed
        if st.need_look then
            st.need_look = false
            look(game, st)
        end

        -- Decrement light timer if a lamp is carried
        local lamp_item = find_active_lamp(game, st)
        if lamp_item ~= nil and st.item_locations[lamp_item] == -1 then
            st.light_time = st.light_time - 1
            if st.light_time <= 0 then
                print("Your light has run out!")
                st.flags[16] = true
            elseif st.light_time <= 25 then
                print("Your light is getting dim.")
            end
        end

        -- Get input
        local line = readline()
        if not line then break end
        line = line:match("^%s*(.-)%s*$")

        if line == "" then
            -- re-prompt
        elseif line:upper() == "QUIT" or line:upper() == "Q" then
            print("Goodbye!")
            break
        elseif line:upper() == "SAVE" then
            save_game(game, st, savefile)
        elseif line:upper() == "LOAD" then
            if load_save(game, st, savefile) then
                print("Game loaded.")
            else
                print("No save file found.")
            end
        elseif line:upper() == "HELP" then
            print("Commands: GO N/S/E/W/U/D, GET <item>, DROP <item>,")
            print("          LOOK, INVENTORY (or I), SCORE, SAVE, LOAD, QUIT")
        elseif line:upper() == "LOOK" or line:upper() == "L" then
            look(game, st)
        elseif line:upper() == "INVENTORY" or line:upper() == "I" then
            local carried = get_inventory_lines(game, st)
            if #carried == 0 then
                print("I'm not carrying anything.")
            else
                print("I'm carrying:")
                for _, d in ipairs(carried) do
                    print("  " .. d)
                end
            end
        elseif line:upper() == "SCORE" then
            local count = count_stored_treasures(game, st)
            print(string.format("I've stored %d of %d treasures.", count, game.num_treasures))
        else
            -- Parse verb/noun
            local verb_idx, noun_idx, verb_word, noun_word = parse_input(game, line)

            -- Check for GO direction as special case
            local handled = false

            -- Direct direction input (N, S, E, W, U, D, NORTH, etc.)
            local dir_idx = DIR_NAMES_UPPER[line:upper()]
            if dir_idx then
                local go_verb = find_verb(game, "GO")
                local dir_noun = find_noun(game, line)
                if go_verb then
                    local fired = run_actions(game, st, go_verb, dir_noun)
                    if not fired then
                        do_go(game, st, dir_idx)
                    end
                else
                    do_go(game, st, dir_idx)
                end
                handled = true
            end

            if not handled then
                if not verb_idx then
                    if verb_word then
                        print("I don't understand the word '" .. verb_word .. "'.")
                    else
                        print("I don't understand that.")
                    end
                else
                    -- Check if it's a GO command
                    local verb_name = game.verbs[verb_idx] or ""
                    if verb_name:sub(1,1) == "*" then verb_name = verb_name:sub(2) end

                    if match_word("GO", verb_name, game.word_length) or verb_idx == 1 then
                        -- GO direction
                        if noun_word then
                            local d = DIR_NAMES_UPPER[noun_word:upper()]
                            if d then
                                local fired = run_actions(game, st, verb_idx, noun_idx)
                                if not fired then
                                    do_go(game, st, d)
                                end
                            else
                                local fired = run_actions(game, st, verb_idx, noun_idx)
                                if not fired then
                                    print("Go where?")
                                end
                            end
                        else
                            print("Go where?")
                        end
                    else
                        -- General action
                        local fired = run_actions(game, st, verb_idx, noun_idx)
                        if not fired then
                            -- Built-in GET/DROP fallback
                            local vname_up = verb_name:upper():sub(1, game.word_length)
                            local get_pfx = ("GET"):sub(1, game.word_length)
                            local tak_pfx = ("TAK"):sub(1, game.word_length)
                            local dro_pfx = ("DRO"):sub(1, game.word_length)
                            if vname_up == get_pfx or vname_up == tak_pfx then
                                if not noun_idx then
                                    print("Get what?")
                                else
                                    local item_i = find_item_by_noun(game, st, noun_idx, true)
                                    if not item_i then
                                        print("I don't see that here.")
                                    elseif carry_count(game, st) >= game.max_carry then
                                        print("I'm carrying too much.")
                                    else
                                        st.item_locations[item_i] = -1
                                        print("OK.")
                                    end
                                end
                            elseif vname_up == dro_pfx then
                                if not noun_idx then
                                    print("Drop what?")
                                else
                                    local item_i = find_item_by_noun(game, st, noun_idx, false)
                                    if not item_i then
                                        print("I'm not carrying that.")
                                    else
                                        st.item_locations[item_i] = st.current_room
                                        print("OK.")
                                    end
                                end
                            else
                                print("I can't do that.")
                            end
                        end
                    end
                end
            end
        end
    end
end

main()
