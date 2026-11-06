-- HUD sin inclinación (o con menos): cg_hudTilt
--   0   = HUD plano, sin curvatura ni giro en perspectiva (por defecto)
--   1   = como el juego original
--   0.5 = la mitad, etc.
--
-- El HUD de IW7 se inclina de dos formas:
--   - Curvatura de cada widget: SetCurvedRenderingX / SetCurvedRenderingY (LUI.HUD.AddWidget, opciones curvedX/curvedY).
--   - Giro en perspectiva de algunos widgets (el minimapa, por ejemplo): SetXRotation / SetYRotation.
--   - Giro en el plano de la pantalla de los widgets de abajo (marcador, armas): SetZRotation, que compensa la
--     perspectiva; sin ella se ven girados de más.
-- Aquí se multiplican esos valores por cg_hudTilt. Solo en partida: los menús no se tocan. De SetZRotation solo se
-- tocan los widgets del HUD (los contenedores de LUI.HUD.AddWidget, id "...Wrapper", que es donde MPHUD pone los giros)
-- y los ángulos pequeños (hasta 20 grados), para no frenar los iconos que giran.
--
-- cg_hudTiltTop: lo mismo, solo para los widgets de arriba, que giran más que el resto: minimapa (MinimapWrapper),
-- misión activa (IntelWrapper) y avisos de arriba a la derecha (GlobalPlayerSplashWrapper). -1 = usar cg_hudTilt.
--
-- El HUD se construye al empezar la partida: si se cambia cg_hudTilt durante una partida, se aplica en la siguiente.

-- cg_hudTilt y cg_hudTiltTop los registra el cliente (ui_scripting.cpp); cg_hudTilt se guarda en la config.

local top_widgets = {
	MinimapWrapper = true,
	IntelWrapper = true,
	GlobalPlayerSplashWrapper = true,
}

local function is_top(element)
	local id = element.id
	return type(id) == "string" and (top_widgets[id] or id:lower():find("minimap", 1, true) ~= nil)
end

local function tilt_scale(element)
	if Engine.InFrontend() then
		return 1
	end

	local value = Engine.GetDvarFloat("cg_hudTilt") or 0
	if is_top(element) then
		local top = Engine.GetDvarFloat("cg_hudTiltTop") or -1
		if top >= 0 then
			value = top
		end
	end

	return math.max(0, math.min(1, value))
end

local function is_hud_wrapper(element)
	local id = element.id
	return type(id) == "string" and id:sub(-7) == "Wrapper"
end

local function scaled(name, max_angle)
	local original = LUI.UIElement[name]
	if not original then
		return
	end

	LUI.UIElement[name] = function(self, value, ...)
		if type(value) == "number" and (not max_angle or math.abs(value) <= max_angle or is_hud_wrapper(self)) then
			value = value * tilt_scale(self)
		end

		return original(self, value, ...)
	end
end

scaled("SetCurvedRenderingX")
scaled("SetCurvedRenderingY")
-- Solo giros pequeños (la inclinación del HUD). Algunos elementos se ocultan girándolos 90 grados (de canto): si se
-- escalara ese giro se verían como una línea cruzando la pantalla.
scaled("SetXRotation", 45)
scaled("SetYRotation", 45)
scaled("SetZRotation", 20)

-- Widgets de arriba más pegados al borde, con el mismo margen que los de abajo. Al quitarles inclinación quedan más
-- separados de la esquina. Desplazamiento en unidades de 1080p (negativo = izquierda / arriba), medido con
-- cg_hudTilt 0.8: si cambias la inclinación, ajústalo aquí (no hace falta compilar).
local edge_offsets = {
	MinimapWrapper = { x = -46, y = 0 },
	IntelWrapper = { x = 22, y = 0 },
	GlobalPlayerSplashWrapper = { x = 22, y = 0 },
}

-- LUI.HUD solo existe en partida (en el menú este script también se carga y no hay HUD)
local add_widget_internal = LUI.HUD and LUI.HUD.AddWidgetInternal
if add_widget_internal then
LUI.HUD.AddWidgetInternal = function(hud, wrapper, name, options, ...)
	local widget = add_widget_internal(hud, wrapper, name, options, ...)

	local offset = wrapper and edge_offsets[wrapper.id]
	if widget and offset and not Engine.InFrontend() then
		-- Igual que el temblor del HUD (LUI.HUD.Shake): el widget sigue ocupando su contenedor, desplazado
		local x = offset.x * _1080p
		local y = offset.y * _1080p
		widget:SetAnchorsAndPosition(0, 0, 0, 0, x, x, y, y)
	end

	return widget
end
end
