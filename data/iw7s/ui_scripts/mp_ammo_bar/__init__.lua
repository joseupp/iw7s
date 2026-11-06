-- HUD MP: barra de cargador estilo campaña (SPWeaponInfoAmmoBar) bajo el contador de balas.
-- En MP la barra (WeaponInfoAmmoBar) solo se mostraba con armas de energía; aquí queda siempre visible.

local function get_value(source, controller)
	local ok, value = pcall(function()
		return source:GetValue(controller)
	end)
	return ok and value or nil
end

local function style_bar(bar)
	if not bar then
		return
	end

	local fill = SWATCHES and SWATCHES.HUD and SWATCHES.HUD.ammoFill
	if fill and bar.AmmoBarFill then
		bar.AmmoBarFill:SetRGBFromTable(fill, 0)
	end

	if bar.Background then
		bar.Background:SetAlpha(0.6, 0)
	end
end

local function patch_weapon_info(self, controller)
	local weapon = DataSources.inGame.player.currentWeapon
	local right = self.WeaponInfoAmmoBar
	local left = self.WeaponInfoAmmoBarLeft

	if not right or not left then
		return
	end

	style_bar(right)
	style_bar(left)

	-- encima de los números (como en campaña): la derecha cubre cargador + reserva
	-- armas de energía: ocupa todo el ancho del recuadro azul
	local function position_right()
		local x1, x2 = 125, 308
		if get_value(weapon.energyBullet, controller) == true then
			x1, x2 = 117, 306
		end
		right:SetAnchorsAndPosition(0, 1, 0, 1, _1080p * x1, _1080p * x2, _1080p * -8, 0)
	end
	position_right()
	right:SubscribeToModel(weapon.energyBullet:GetModel(controller), position_right)
	left:SetAnchorsAndPosition(0, 1, 0, 1, _1080p * 14.25, _1080p * 116.5, _1080p * -8, 0)

	local function right_alpha()
		local alpha = get_value(weapon.ammoInfoAlpha, controller)
		return alpha == nil and 1 or alpha
	end

	local function left_alpha()
		if get_value(weapon.isDualWielding, controller) == true then
			return right_alpha()
		end
		return 0
	end

	local function refresh()
		right:SetAlpha(right_alpha(), 0)
		left:SetAlpha(left_alpha(), 0)
	end

	-- las secuencias "Hide*EnergyMeter" ya no ocultan la barra
	self._sequences.HideRightEnergyMeter = function()
		right:SetAlpha(right_alpha(), 0)
	end
	self._sequences.HideLeftEnergyMeter = function()
		left:SetAlpha(left_alpha(), 0)
	end

	right:SubscribeToModel(weapon.ammoInfoAlpha:GetModel(controller), refresh)
	left:SubscribeToModel(weapon.isDualWielding:GetModel(controller), refresh)

	refresh()
end

local function wrap_weapon_info(builder)
	return function(menu, controller)
		local self = builder(menu, controller)
		local controller_index = controller and controller.controllerIndex
		if not controller_index and not Engine.InFrontend() then
			controller_index = self:getRootController()
		end

		local ok, err = pcall(patch_weapon_info, self, controller_index)
		if not ok then
			print("mp_ammo_bar: " .. tostring(err))
		end

		return self
	end
end

if MenuBuilder.m_types["WeaponInfo"] then
	MenuBuilder.m_types["WeaponInfo"] = wrap_weapon_info(MenuBuilder.m_types["WeaponInfo"])
else
	local register_type = MenuBuilder.registerType
	MenuBuilder.registerType = function(name, builder, ...)
		if name == "WeaponInfo" then
			builder = wrap_weapon_info(builder)
		end
		return register_type(name, builder, ...)
	end
end
