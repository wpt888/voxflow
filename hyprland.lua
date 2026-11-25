-- Hyprland 0.55+ / Lua. Add this block to your Hyprland config.
do
    local voxflow = '"$HOME/.local/bin/voxflow"'
    local dictation_key = "code:108" -- Physical Right Alt, including AltGr layouts.
    if dictation_key == "code:108" and o and o.bind_hold then
        hl.unbind("ALT + Alt_R") -- Omarchy's optional Right Alt dictation binding.
    end
    hl.bind(dictation_key, hl.dsp.exec_cmd(voxflow .. " press"),
        { ignore_mods = true, transparent = true, description = "Start VoxFlow dictation" })
    hl.bind(dictation_key, hl.dsp.exec_cmd(voxflow .. " release"),
        { ignore_mods = true, transparent = true, release = true, locked = true,
          dont_inhibit = true, non_consuming = true, description = "Transcribe with VoxFlow" })
    hl.bind("SUPER + SHIFT + Escape", hl.dsp.exec_cmd(voxflow .. " cancel"),
        { description = "Cancel VoxFlow dictation" })
    -- Right Alt can still contribute Alt or Mod5 while the user holds it.
    for _, modifier in ipairs({ "ALT", "MOD5" }) do
        hl.bind("SUPER + SHIFT + " .. modifier .. " + Escape", hl.dsp.exec_cmd(voxflow .. " cancel"))
    end
end
