#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"
#include "fov.hpp"

#include <utils/hook.hpp>

#include <algorithm>
#include <cmath>

// FOV separado para el arma (viewmodel), como en los COD nuevos. Port de boiii / s1-mod.
//
// IW7 ya tiene soporte nativo de "dual FOV" (miras de doble render): el refdef guarda dos pares de tangentes
//   refdef = cg + 0x4B38
//   cg + 0x4B48 / 0x4B4C  tanHalfFovX/Y del mundo
//   cg + 0x4B84 / 0x4B88  tanHalfFovX/Y "dual" (FOV del arma)
// y el render SIEMPRE dibuja las pasadas del arma (depth hack, depth hack sss, depth hack trans) con las
// tangentes "dual" (0x140DE6E00 construye su viewParms con viewParms + 0x160/0x164 = refdef + 0x4C/0x50).
// Sin mira dual activa, CG_CalcFov copia las del mundo en las del arma. Aquí se sustituyen por las del arma.
//
// Hilo del juego:
//   0x140888AB0  CG_CalcFov(localClientNum)
//                  fov = CG_GetViewFov(lc, 0)                     (0x14087CCB0)
//                  CG_CalcFovTangents(cg, fov, aspecto)            (0x1408B5580) -> cg + 0x4B48 / 0x4B4C
//                  si cg + 0x59A45 (mira dual activa): tangentes dual con cg_dualViewScopeFOV
//                  si no: cg + 0x4B84 = cg + 0x4B48 (8 bytes)
//   0x1408979A0  fov = (1 - zoom) * fovCadera + zoom * fovADS
//                  zoom = 0x1408F2150(cg, &arma), llamada en 0x140898AF0
//                  zoom = 0 si (cg + 0x492B8 == 0 && 0x1408A3680(lc) && !(cg + 0x59A45))
//   tanY = tan(fov / 2) * 0.75, tanX = tanY * aspecto  (0x140CE5120)
//
// Hilo de render:
//   0x140DE6E00  pasadas depth hack (0, 1, 9): ya construyen un viewParms con las tangentes dual.
//   viewParms (0x170): +0x130 / +0x134 tanHalfFovX/Y, +0x160 / +0x164 tangentes dual
//
// Reparto de superficies del arma (0x140E2D000, emisor de superficies de DObj):
//   Con depth hack y SIN el flag 0x4000 solo opaco -> lista 0 y sss -> lista 1; trans, decal, emissive...
//   iban a las listas del mapa (6, 7, 17, 18) y se dibujaban con el FOV del mundo (cristal de las miras,
//   cables). Con el flag 0x4000 (el que pone el juego con la mira dual) trans -> lista 9 y el resto -> lista 0.
//   CG_AddViewWeapon (0x1408D11F0) solo lo pone si cg + 0x59A45; se parchea en 0x1408D2809 para ponerlo
//   también cuando el arma usa su propio FOV.
//   Excepción: categoría emissive (4) forzada a la lista 0 no se dibuja (lente Troyana / réflex). En 0x140E2D374
//   se manda a la lista depth hack trans (bucket 6 -> lista 9), como el trans. Las listas emissive del mapa
//   (17 / 18) llevan también hologramas y propulsores de los personajes: no se tocan.
//
// Elementos LUI pegados al arma (lente / cristal de las miras, retículas):
//   Tipo 1 (2D anclado): 0x1404CC540 proyecta con viewIndex = (anchor + 0x108 == 2). AttachToReticleViewModel
//     pone 2 (tangentes dual) pero AttachToViewModel pone 1 y proyectaba con las del mundo. Se parchea en
//     0x1404CC60A para que 1 también use las tangentes dual.
//   Tipo 2 (en el mundo, 3D): 0x1404DE150(lc, elem, axis[3][3] float, origin[3] double) calcula su posición;
//     anchor + 0xBC: 2 = AttachToViewModel, 3 = AttachToReticleViewModel. Se dibujan con la proyección del
//     mundo, así que se mueve el elemento en espacio de cámara (escala lateral tanMundo / tanArma) para que
//     caiga donde lo pone la proyección del arma. Llamada en 0x1404DF6F7.
//   refdef: cg + 0x4B50 vieworg, cg + 0x4B5C viewaxis[3][3]
//
// Partículas pegadas al arma (casquillos...): se dibujan con la proyección del mundo; se corrige su origen al
// salir (0x140ABBB7F) y en la caché de huesos de partículas (0x140D150E1).

namespace viewmodel_fov
{
	namespace
	{
		constexpr auto deg_to_rad = 0.01745329251994329577f;

		constexpr size_t cg_world_tan_x = 0x4B48;
		constexpr size_t cg_world_tan_y = 0x4B4C;
		constexpr size_t cg_dual_tan_x = 0x4B84;
		constexpr size_t cg_dual_tan_y = 0x4B88;
		constexpr size_t cg_zoom_override = 0x492B8;
		constexpr size_t cg_dual_view_active = 0x59A45;

		game::dvar_t* cg_fov_viewmodel = nullptr;
		game::dvar_t* cg_fov_viewmodel_scale = nullptr;
		utils::hook::detour cg_calc_fov_hook;
		utils::hook::detour cg_calc_fov_tangents_hook;

		// Solo válidos mientras se ejecuta CG_CalcFov (hilo del juego)
		bool in_calc_fov = false;
		int current_local_client = 0;
		uint8_t* current_cg = nullptr;
		float captured_zoom = 0.0f;

		// Último cg y si en este frame el arma usa su propio FOV (lo leen los elementos LUI)
		uint8_t* last_cg = nullptr;
		uint8_t viewmodel_fov_applied = 0;

		template <typename T>
		T& field(void* base, const size_t offset)
		{
			return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
		}

		float get_viewmodel_fov()
		{
			return cg_fov_viewmodel ? cg_fov_viewmodel->current.value : 0.0f;
		}

		float get_viewmodel_fov_scale()
		{
			return cg_fov_viewmodel_scale ? cg_fov_viewmodel_scale->current.value : 1.0f;
		}

		// 1 / tanArma = (1 - zoom) / tanVM + zoom / tanMundo
		// Cadera: FOV propio del arma. ADS completo: FOV del mundo sin cg_fovScale (ni el factor de cg_fovAdsAffected),
		// para que el arma quede en su posición original al apuntar con cualquier escala.
		bool calc_weapon_tan(const float world_x, const float world_y, const float zoom, float& weapon_x, float& weapon_y)
		{
			const auto viewmodel_fov = get_viewmodel_fov();
			if (viewmodel_fov <= 0.0f || world_x <= 0.0f || world_y <= 0.0f)
			{
				return false;
			}

			const auto viewmodel_tan_y = std::tan(viewmodel_fov * 0.5f * deg_to_rad) * 0.75f * get_viewmodel_fov_scale();
			if (viewmodel_tan_y <= 0.0f)
			{
				return false;
			}

			// cg_fovScale multiplica el FOV en grados (0x14087DFC6)
			auto ads_tan_y = world_y;
			const auto world_scale = fov::get_world_fov_scale();
			if (world_scale > 0.0f && world_scale != 1.0f)
			{
				const auto world_fov = std::atan(world_y / 0.75f) * 2.0f;
				ads_tan_y = std::tan(world_fov / world_scale * 0.5f) * 0.75f;
			}

			const auto z = std::clamp(zoom, 0.0f, 1.0f);
			const auto inv_weapon_y = (1.0f - z) / viewmodel_tan_y + z / ads_tan_y;
			if (inv_weapon_y <= 0.0f)
			{
				return false;
			}

			weapon_y = 1.0f / inv_weapon_y;
			weapon_x = weapon_y * (world_x / world_y);
			return true;
		}

		// Llamada a 0x1408F2150 dentro de 0x1408979A0: fracción de zoom del FOV del mundo
		float get_zoom_frac_stub(void* cg, const void* weapon)
		{
			const auto zoom = utils::hook::invoke<float>(0x1408F2150, cg, weapon);

			if (in_calc_fov)
			{
				auto effective = zoom;

				// Mismo caso que el juego: con mira dual disponible pero aún no activa el mundo no hace zoom
				if (field<float>(cg, cg_zoom_override) == 0.0f &&
					utils::hook::invoke<bool>(0x1408A3680, current_local_client) &&
					!field<bool>(cg, cg_dual_view_active))
				{
					effective = 0.0f;
				}

				captured_zoom = effective;
			}

			return zoom;
		}

		// CG_CalcFovTangents(cg, fov, aspecto): se usa para obtener el puntero cg
		void cg_calc_fov_tangents_stub(void* cg, const float fov, const float aspect)
		{
			if (in_calc_fov)
			{
				current_cg = static_cast<uint8_t*>(cg);
			}

			cg_calc_fov_tangents_hook.invoke<void>(cg, fov, aspect);
		}

		void cg_calc_fov_stub(const int local_client_num)
		{
			in_calc_fov = true;
			current_local_client = local_client_num;
			current_cg = nullptr;
			captured_zoom = 0.0f;

			cg_calc_fov_hook.invoke<void>(local_client_num);

			in_calc_fov = false;
			viewmodel_fov_applied = 0;

			const auto cg = current_cg;

			if (!cg || field<bool>(cg, cg_dual_view_active))
			{
				// Mira dual activa: el juego ya ha puesto su propio FOV del arma
				return;
			}

			const auto world_x = field<float>(cg, cg_world_tan_x);
			const auto world_y = field<float>(cg, cg_world_tan_y);

			float weapon_x, weapon_y;
			if (!calc_weapon_tan(world_x, world_y, captured_zoom, weapon_x, weapon_y))
			{
				return;
			}

			field<float>(cg, cg_dual_tan_x) = weapon_x;
			field<float>(cg, cg_dual_tan_y) = weapon_y;

			last_cg = cg;
			viewmodel_fov_applied = 1;
		}
	}

	namespace
	{
		// En 0x140E2D000 (reparto con depth hack + flag 0x4000): el emissive (categoría 4) no se fuerza a la
		// lista opaca del arma (no se dibujaba: lente Troyana / réflex); va a la lista depth hack trans (9).
		void* emissive_category_stub()
		{
			return utils::hook::assemble([](utils::hook::assembler& a)
			{
				const auto to_depth_hack_opaque = a.newLabel();
				const auto to_next = a.newLabel();
				const auto emissive = a.newLabel();

				a.test(ecx, ecx);
				a.je(to_depth_hack_opaque);
				a.cmp(ecx, 4);
				a.je(emissive);
				a.mov(eax, dword_ptr(rsp, 0x48));
				a.bt(eax, 0xE);
				a.jb(to_depth_hack_opaque);

				a.bind(to_next);
				a.jmp(0x140E2D382);

				// Emissive del arma (flag 0x4000): bucket 6 (lista 9), mismo código que el trans en 0x140E2D35E
				a.bind(emissive);
				a.mov(eax, dword_ptr(rsp, 0x48));
				a.bt(eax, 0xE);
				a.jae(to_next);
				a.mov(dword_ptr(rsp, 0x70), r12d);
				a.mov(r12d, 1);
				a.mov(dword_ptr(rsp, 0x60), 6);
				a.jmp(0x140E2D416);

				a.bind(to_depth_hack_opaque);
				a.jmp(0x140E2D399);
			});
		}

		// En CG_AddViewWeapon: bts r15d, 0xE (flag 0x4000) si mira dual activa o si el arma usa su propio FOV
		void* viewmodel_dual_flag_stub()
		{
			return utils::hook::assemble([](utils::hook::assembler& a)
			{
				const auto set_flag = a.newLabel();
				const auto done = a.newLabel();

				a.cmp(byte_ptr(rbx, 0x59A45), 0);
				a.jne(set_flag);
				a.mov(rax, reinterpret_cast<size_t>(&viewmodel_fov_applied)); // rax se recarga en 0x1408D2817
				a.cmp(byte_ptr(rax), 0);
				a.je(done);

				a.bind(set_flag);
				a.bts(r15d, 0xE);

				a.bind(done);
				a.jmp(0x1408D2817);
			});
		}

		constexpr size_t cg_view_origin = 0x4B50;
		constexpr size_t cg_view_axis = 0x4B5C;

		float dot(const float* a, const float* b)
		{
			return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		}

		// Elementos LUI tipo 1: viewIndex = 1 si anchor + 0x108 == 2, o == 1 con el FOV del arma activo
		void* lui_anchor_view_index_stub()
		{
			return utils::hook::assemble([](utils::hook::assembler& a)
			{
				const auto set_dual = a.newLabel();
				const auto done = a.newLabel();

				// código original (sin el cmp / sete)
				a.mov(rdx, rax);
				a.mov(ecx, ebx);

				a.movzx(r10d, byte_ptr(rdi, 0x108));
				a.cmp(r10d, 2);
				a.je(set_dual);
				a.cmp(r10d, 1);
				a.jne(done);
				a.mov(r11, reinterpret_cast<size_t>(&viewmodel_fov_applied));
				a.cmp(byte_ptr(r11), 0);
				a.je(done);

				a.bind(set_dual);
				a.mov(r9d, 1);

				a.bind(done);
				a.jmp(0x1404CC61A);
			});
		}

		// Elementos LUI tipo 2 (en el mundo) pegados al arma
		bool lui_in_world_transform_stub(const int local_client_num, uint8_t* element, float* axis, double* origin, void* a5)
		{
			const auto result = utils::hook::invoke<bool>(0x1404DE150, local_client_num, element, axis, origin, a5);
			if (!result || !viewmodel_fov_applied || !last_cg || !element)
			{
				return result;
			}

			const auto anchor = *reinterpret_cast<uint8_t**>(element + 0x140);
			if (!anchor)
			{
				return result;
			}

			const auto attach_type = static_cast<int8_t>(anchor[0xBC]);
			if (attach_type != 2 && attach_type != 3)
			{
				return result;
			}

			const auto cg = last_cg;
			const auto world_x = field<float>(cg, cg_world_tan_x);
			const auto world_y = field<float>(cg, cg_world_tan_y);
			const auto weapon_x = field<float>(cg, cg_dual_tan_x);
			const auto weapon_y = field<float>(cg, cg_dual_tan_y);
			if (world_x <= 0.0f || world_y <= 0.0f || weapon_x <= 0.0f || weapon_y <= 0.0f)
			{
				return result;
			}

			const auto scale_left = world_x / weapon_x;
			const auto scale_up = world_y / weapon_y;

			const auto* view_origin = &field<float>(cg, cg_view_origin);
			const auto* forward = &field<float>(cg, cg_view_axis);
			const auto* left = forward + 3;
			const auto* up = forward + 6;

			// Escala lateral en espacio de cámara: proyección del mundo de p' == proyección del arma de p
			const auto transform_dir = [&](const float* v, float* out)
			{
				const auto f = dot(v, forward);
				const auto l = dot(v, left) * scale_left;
				const auto u = dot(v, up) * scale_up;
				for (auto i = 0; i < 3; ++i)
				{
					out[i] = f * forward[i] + l * left[i] + u * up[i];
				}
			};

			const float rel[3] =
			{
				static_cast<float>(origin[0] - view_origin[0]),
				static_cast<float>(origin[1] - view_origin[1]),
				static_cast<float>(origin[2] - view_origin[2]),
			};

			float moved[3];
			transform_dir(rel, moved);
			for (auto i = 0; i < 3; ++i)
			{
				origin[i] = static_cast<double>(view_origin[i]) + moved[i];
			}

			for (auto row = 0; row < 3; ++row)
			{
				float scaled[3];
				transform_dir(axis + row * 3, scaled);
				std::memcpy(axis + row * 3, scaled, sizeof(scaled));
			}

			return result;
		}

		// Casquillos y demás partículas pegadas al arma (ent >= 0x800). Se dibujan con la proyección del mundo:
		// se mueve su origen en espacio de cámara (escala lateral tanMundo / tanArma), como los elementos LUI.
		//   Salida: CG_EjectWeaponBrass (0x1408DBE40) -> 0x1407D2040 (llamada 0x1408DCFC7) -> ... ->
		//     0x140ABBB20 -> 0x140AC0370(lc, ent, bone, orient) (llamada 0x140ABBB7F). orient: origin[3] + axis[3][3]
		//   Por frame: caché de huesos de partículas (particle_bolt_caching), rellenada en 0x140D14E40 con
		//     0x140AC0780(lc, ent, count, bones, axes[count][9], origins[count][3]) (llamada 0x140D150E1).
		thread_local bool in_eject_brass = false;

		// Viewmodel: 0x800 + mano (0x14073E550 devuelve 0 / 1). 0x802.. son otras entidades del cliente
		// (hologramas, propulsores de otros jugadores...) que no se deben tocar.
		bool is_viewmodel_entity(const int ent)
		{
			return ent == 0x800 || ent == 0x801;
		}

		// p (mundo) -> punto que con la proyección del mundo cae donde la del arma pone p
		bool to_world_projection(float* point)
		{
			const auto cg = last_cg;
			if (!cg || !viewmodel_fov_applied)
			{
				return false;
			}

			const auto world_x = field<float>(cg, cg_world_tan_x);
			const auto world_y = field<float>(cg, cg_world_tan_y);
			const auto weapon_x = field<float>(cg, cg_dual_tan_x);
			const auto weapon_y = field<float>(cg, cg_dual_tan_y);
			if (world_x <= 0.0f || world_y <= 0.0f || weapon_x <= 0.0f || weapon_y <= 0.0f)
			{
				return false;
			}

			const auto* view_origin = &field<float>(cg, cg_view_origin);
			const auto* forward = &field<float>(cg, cg_view_axis);
			const auto* left = forward + 3;
			const auto* up = forward + 6;

			const float rel[3] = { point[0] - view_origin[0], point[1] - view_origin[1], point[2] - view_origin[2] };
			const auto f = dot(rel, forward);
			const auto l = dot(rel, left) * (world_x / weapon_x);
			const auto u = dot(rel, up) * (world_y / weapon_y);

			for (auto i = 0; i < 3; ++i)
			{
				point[i] = view_origin[i] + f * forward[i] + l * left[i] + u * up[i];
			}

			return true;
		}

		void particle_bone_cache_stub(const int local_client_num, const int ent, const unsigned int count, const int* bones,
		                              float* axes, float* origins)
		{
			utils::hook::invoke<void>(0x140AC0780, local_client_num, ent, count, bones, axes, origins);
			if (!is_viewmodel_entity(ent) || !origins)
			{
				return;
			}

			for (auto i = 0u; i < count; ++i)
			{
				to_world_projection(origins + i * 3);
			}
		}

		void eject_brass_play_fx_stub(const int local_client_num, void* fx, const int ent, const int bone)
		{
			in_eject_brass = true;
			utils::hook::invoke<void>(0x1407D2040, local_client_num, fx, ent, bone);
			in_eject_brass = false;
		}

		bool fx_spawn_bolt_orientation_stub(const int local_client_num, const int ent, const int bone, float* orient)
		{
			const auto result = utils::hook::invoke<bool>(0x140AC0370, local_client_num, ent, bone, orient);
			if (result && in_eject_brass && is_viewmodel_entity(ent) && orient)
			{
				to_world_projection(orient);
			}

			return result;
		}

		// Láser del arma: CG_AddViewWeapon (llamada 0x1408D2938) -> 0x1407FAF20 (no se dibuja con mira dual)
		//   -> 0x1407F9AB0 -> 0x1407EE690(lc, ?, arma, dobj, ?, orient) (llamada 0x1407F9B0C) saca tag_laser
		//   (o tag_flash) del arma en coordenadas de mundo. orient: origin[3] + axis[3][3]. El rayo se dibuja con
		//   la proyección del mundo: se lleva el origen (y la dirección) a donde lo pone la proyección del arma.
		bool viewmodel_laser_orientation_stub(const int local_client_num, void* a2, void* a3, void* a4, void* a5,
		                                      float* orient)
		{
			const auto result = utils::hook::invoke<bool>(0x1407EE690, local_client_num, a2, a3, a4, a5, orient);
			if (!result || !orient)
			{
				return result;
			}

			const auto cg = last_cg;
			if (!cg || !viewmodel_fov_applied)
			{
				return result;
			}

			// Punto final del cañón (origen + forward) antes de mover nada, para sacar la dirección en pantalla
			float tip[3] =
			{
				orient[0] + orient[3],
				orient[1] + orient[4],
				orient[2] + orient[5],
			};

			if (!to_world_projection(orient))
			{
				return result;
			}

			to_world_projection(tip);

			float dir[3] = { tip[0] - orient[0], tip[1] - orient[1], tip[2] - orient[2] };
			const auto length = std::sqrt(dot(dir, dir));
			if (length > 0.0001f)
			{
				for (auto i = 0; i < 3; ++i)
				{
					orient[3 + i] = dir[i] / length;
				}
			}

			return result;
		}
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (game::environment::is_dedi())
			{
				return;
			}

			cg_fov_viewmodel = game::Dvar_RegisterFloat("cg_fovViewmodel", 65.0f, 0.0f, 160.0f, game::DVAR_FLAG_SAVED,
				"FOV of the weapon viewmodel (0 = use world FOV)");
			cg_fov_viewmodel_scale = game::Dvar_RegisterFloat("cg_fovViewmodelScale", 1.0f, 0.1f, 3.0f, game::DVAR_FLAG_SAVED,
				"Scale applied to the weapon viewmodel FOV");

			cg_calc_fov_hook.create(0x140888AB0, cg_calc_fov_stub);
			cg_calc_fov_tangents_hook.create(0x1408B5580, cg_calc_fov_tangents_stub);
			utils::hook::call(0x140898AF0, get_zoom_frac_stub);

			// Superficies del arma a las listas depth hack (también decal / trans / emissive)
			utils::hook::jump(0x1408D2809, viewmodel_dual_flag_stub()); // salto de 5 bytes
			utils::hook::jump(0x140E2D374, emissive_category_stub()); // salto de 5 bytes

			// Elementos LUI pegados al arma
			utils::hook::jump(0x1404CC60A, lui_anchor_view_index_stub()); // salto de 5 bytes: no toca rax
			utils::hook::call(0x1404DF6F7, lui_in_world_transform_stub);

			// Casquillos y partículas pegadas al arma
			utils::hook::call(0x1408DCFC7, eject_brass_play_fx_stub);
			utils::hook::call(0x140ABBB7F, fx_spawn_bolt_orientation_stub);
			utils::hook::call(0x140D150E1, particle_bone_cache_stub);

			// Láser del arma (tag_laser)
			utils::hook::call(0x1407F9B0C, viewmodel_laser_orientation_stub);
		}
	};
}

REGISTER_COMPONENT(viewmodel_fov::component)
