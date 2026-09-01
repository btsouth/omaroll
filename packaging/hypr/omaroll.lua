-- Omaroll shows photographs and video frames, so it opts out of Omarchy's
-- default window translucency the same way mpv, imv and Pinta do. The app
-- paints its own chrome alpha from the active theme; compositor dimming would
-- stack on top of that and wash out every thumbnail.
--
-- Drop into ~/.config/hypr/ and require it, until the rule lands upstream in
-- default/hypr/apps/system.lua.
o.window("^(io\\.github\\.tsouth89\\.omaroll)$", { tag = "-default-opacity" })
o.window("^(io\\.github\\.tsouth89\\.omaroll)$", { opacity = "1 1" })
