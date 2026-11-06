-- Opciones de vídeo avanzadas > Varios, debajo del FOV:
--   "Zoom afectado por el FOV" (cg_fovAdsAffected)
--   "Tamaño del arma" (cg_fovViewmodel: pequeño 80, normal 65, grande 55)

-- Slider del FOV (com_fovUserScale, sistema original): el máximo pasa a 1,6923 (FOV 110 sobre la base de 65), el
-- límite que ya aplica el juego (0x140898C10). El número del slider original es escala * 80 (aproximado); se sustituye
-- por un slider igual que muestra el FOV real (cg_fov * escala).
local function raise_fov_slider_max()
	if SliderBounds and SliderBounds.FOVUserScale then
		SliderBounds.FOVUserScale.Max = 110 / 65
	end
end
raise_fov_slider_max()

local dvar_name = "cg_fovAdsAffected"

local function build_ads_affected_button(controller)
	local button = MenuBuilder.BuildRegisteredType("GenericArrowButton", {
		controllerIndex = controller
	})
	button.id = "FOVAdsAffected"
	button:SetAnchorsAndPosition(0, 0, 0, 1, 0, 0, 0, _1080p * 50, 0)
	button.buttonDescription = "Con la opción activada, el zoom al apuntar se escala con el campo de visión elegido."
	button.Title:setText(ToUpperCase("Zoom afectado por el campo de vision"), 0)

	local refresh = function()
		LUI.AddUIArrowTextButtonLogic(button, controller, {
			labels = {
				Engine.Localize("LUA_MENU_OFF"),
				Engine.Localize("LUA_MENU_ON")
			},
			action = function(index)
				Engine.SetDvarBool(dvar_name, index == 2)
			end,
			defaultValue = Engine.GetDvarBool(dvar_name) and 2 or 1,
			wrapAround = true
		})
	end

	button:addEventHandler("refresh_values", function()
		refresh()
	end)
	refresh()

	return button
end

local weapon_size_values = { 75, 65, 60 }

local function get_weapon_size_index()
	local value = Engine.GetDvarFloat("cg_fovViewmodel")
	local best, best_diff = 2, nil
	for i, v in ipairs(weapon_size_values) do
		local diff = math.abs(value - v)
		if not best_diff or diff < best_diff then
			best, best_diff = i, diff
		end
	end
	return best
end

local function build_weapon_size_button(controller)
	local button = MenuBuilder.BuildRegisteredType("GenericArrowButton", {
		controllerIndex = controller
	})
	button.id = "WeaponSize"
	button:SetAnchorsAndPosition(0, 0, 0, 1, 0, 0, 0, _1080p * 50, 0)
	button.buttonDescription = "Tamaño del arma en pantalla (campo de visión del arma)."
	button.Title:setText(ToUpperCase("Tamaño del arma"), 0)

	local refresh = function()
		LUI.AddUIArrowTextButtonLogic(button, controller, {
			labels = { "Pequeño", "Normal", "Grande" },
			action = function(index)
				Engine.SetDvarFloat("cg_fovViewmodel", weapon_size_values[index])
			end,
			defaultValue = get_weapon_size_index(),
			wrapAround = true
		})
	end

	button:addEventHandler("refresh_values", function()
		refresh()
	end)
	refresh()

	return button
end

local function update_fov_slider(self)
	local fraction = (self.currentValue - self.min) / (self.max - self.min)
	if self.direction == ArrowBarFillDirections.LEFT_TO_RIGHT then
		self.fillElement:SetAnchors(0, 1 - fraction, 0, 0, self.updateDuration)
	else
		self.fillElement:SetAnchors(1 - fraction, 0, 0, 0, self.updateDuration)
	end
	local real_fov = Engine.GetDvarFloat("cg_fov") * self.currentValue
	self.Text:setText(string.format("%d", math.floor(real_fov + 0.5)), self.updateDuration)
	ACTIONS.AnimateSequence(self, "ShowNumberLabel")
end

-- Mismo slider que el original (com_fovUserScale, límites de SliderBounds), con el FOV real como número
local function build_fov_slider(controller)
	raise_fov_slider_max()
	local bounds = SliderBounds.FOVUserScale

	local button = MenuBuilder.BuildRegisteredType("GenericFillBarArrowButton", {
		controllerIndex = controller
	})
	button.id = "FOV"
	button:SetAnchorsAndPosition(0, 0, 0, 1, 0, 0, 0, _1080p * 50, 0)
	button.buttonDescription = Engine.Localize("PLATFORM_UI_FOV_DESC")
	button.Title:setText(ToUpperCase(Engine.Localize("PLATFORM_UI_FOV_CAPS")), 0)

	local value = Engine.GetDvarFloat("com_fovUserScale")
	value = math.max(bounds.Min, math.min(bounds.Max, value))

	LUI.AddUIArrowFillBarButtonLogic(button, controller, {
		decimalPlacesToRound = 6,
		action = function(scale)
			Engine.SetDvarFloat("com_fovUserScale", scale)
		end,
		defaultValue = value,
		wrapAround = false,
		max = bounds.Max,
		min = bounds.Min,
		step = bounds.Step,
		fillElement = button.GenericFillBar.Fill,
		UpdateContent = update_fov_slider
	})

	-- marca del valor por defecto (escala 1), como en el original
	ACTIONS.AnimateSequence(button, "ShowTickMarker")
	local tick = (1 - bounds.Min) / (bounds.Max - bounds.Min)
	button.GenericFillBar.TickMarker:SetAnchorsAndPosition(tick, 1 - tick - 0.01, 0, 0, 0, 0, -4, 4)

	return button
end

-- Opciones de la categoría: el FOV se sustituye y los botones van justo después
local function wrap_create_options(create_options)
	return function(menu, controller)
		local options = create_options(menu, controller)
		if type(options) ~= "table" then
			return options
		end

		local index = #options + 1
		for i, option in ipairs(options) do
			if option.id == "FOV" then
				if SliderBounds and SliderBounds.FOVUserScale then
					options[i] = build_fov_slider(controller)
				end
				index = i + 1
				break
			end
		end

		table.insert(options, index, build_ads_affected_button(controller))
		table.insert(options, index + 1, build_weapon_size_button(controller))
		return options
	end
end

local function wrap_menu(builder)
	return function(menu, controller)
		raise_fov_slider_max()
		local self = builder(menu, controller)
		local process_event = self.processEvent

		self.processEvent = function(element, event)
			if event and event.name == "category_changed" and event.category == "misc_options" and event.createOptions then
				event.createOptions = wrap_create_options(event.createOptions)
			end
			return process_event(element, event)
		end

		return self
	end
end

if MenuBuilder.m_types["OptionsVideoAdvanced"] then
	MenuBuilder.m_types["OptionsVideoAdvanced"] = wrap_menu(MenuBuilder.m_types["OptionsVideoAdvanced"])
else
	local register_type = MenuBuilder.registerType
	MenuBuilder.registerType = function(name, builder, ...)
		if name == "OptionsVideoAdvanced" then
			builder = wrap_menu(builder)
		end
		return register_type(name, builder, ...)
	end
end
