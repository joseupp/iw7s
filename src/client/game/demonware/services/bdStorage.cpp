#include <std_include.hpp>
#include "../dw_include.hpp"

#include "component/console/console.hpp"

#include <utils/nt.hpp>
#include <utils/io.hpp>
#include <utils/cryptography.hpp>
#include <utils/compression.hpp>

#include "game/game.hpp"

namespace demonware
{
	namespace
	{
		// Playlist sin comprimir en iw7s/dw/data, con los nombres de tu extractor: la lista en un .pdo y las recetas
		// de los modos ("recipe N" en la lista) en .gam (texto) o .dat (binarias). El nombre es la posición en el
		// .aggr en hexadecimal: las recetas van por orden de nombre (la primera es recipe 0). Se comprime al pedirla
		// el juego.
		// Formato .aggr: u16 1, u16 posición y u16 tamaño de la lista, u16 número de recetas, u16 posición y u16
		// tamaño de cada receta; detrás, cada parte comprimida con zlib.
		bool build_playlists_aggr(std::string& buffer)
		{
			const std::filesystem::path dir = "iw7s/dw/data";
			std::error_code ec;
			if (!std::filesystem::is_directory(dir, ec))
			{
				return false;
			}

			std::vector<std::filesystem::path> lists, recipe_files;
			for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
			{
				if (!entry.is_regular_file(ec))
				{
					continue;
				}

				auto ext = entry.path().extension().string();
				std::transform(ext.begin(), ext.end(), ext.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (ext == ".pdo")
				{
					lists.push_back(entry.path());
				}
				else if (ext == ".gam" || ext == ".dat")
				{
					recipe_files.push_back(entry.path());
				}
			}

			if (lists.empty())
			{
				return false;
			}

			std::sort(lists.begin(), lists.end());
			std::sort(recipe_files.begin(), recipe_files.end(), [](const auto& a, const auto& b)
			{
				return a.filename().string() < b.filename().string();
			});

			std::vector<std::string> parts(1);
			if (!utils::io::read_file(lists[0].string(), &parts[0]))
			{
				return false;
			}

			for (const auto& file : recipe_files)
			{
				std::string recipe;
				if (!utils::io::read_file(file.string(), &recipe))
				{
					return false;
				}

				parts.emplace_back(std::move(recipe));
			}

			std::string data;
			std::vector<std::pair<std::size_t, std::size_t>> entries;
			auto offset = 2u + 4u + 2u + 4u * (parts.size() - 1);
			for (const auto& part : parts)
			{
				const auto compressed = utils::compression::zlib::compress(part);
				if (compressed.empty())
				{
					return false;
				}

				entries.emplace_back(offset, compressed.size());
				offset += compressed.size();
				data.append(compressed);
			}

			if (offset > 0xFFFF)
			{
				console::error("iw7s/dw/data: la playlist comprimida ocupa %zu bytes (máximo 65535)\n", offset);
				return false;
			}

			std::string header;
			const auto write_u16 = [&header](const std::size_t value)
			{
				header.push_back(static_cast<char>(value & 0xFF));
				header.push_back(static_cast<char>((value >> 8) & 0xFF));
			};

			write_u16(1);
			write_u16(entries[0].first);
			write_u16(entries[0].second);
			write_u16(entries.size() - 1);
			for (auto i = 1u; i < entries.size(); ++i)
			{
				write_u16(entries[i].first);
				write_u16(entries[i].second);
			}

			buffer = header + data;
			return true;
		}

		// Orden: sin comprimir de iw7s/dw/data, iw7s/dw/playlists_tu23.aggr y la del ejecutable
		std::string load_playlists()
		{
			std::string buffer;
			if (build_playlists_aggr(buffer) || utils::io::read_file("iw7s/dw/playlists_tu23.aggr", &buffer))
			{
				return buffer;
			}

			return utils::nt::load_resource(DW_PLAYLISTS);
		}
	}

	bdStorage::bdStorage() : service(10, "bdStorage")
	{
		this->register_task(20, &bdStorage::listAllPublisherFiles);
		this->register_task(21, &bdStorage::getPublisherFile);
		this->register_task(24, &bdStorage::uploadAndValidateFiles);
		this->register_task(18, &bdStorage::uploadFiles);
		this->register_task(16, &bdStorage::getFiles);
		this->register_task(12, &bdStorage::getFile);

		this->map_publisher_resource("motd-.*\\.txt", DW_MOTD);
		// this->map_publisher_resource("ffotd-.*\\.ff", DW_FASTFILE);
		this->map_publisher_resource_variant("playlists(_.+)?\\.aggr", load_playlists);
	}

	void bdStorage::map_publisher_resource(const std::string& expression, const INT id)
	{
		auto data = utils::nt::load_resource(id);
		this->map_publisher_resource_variant(expression, std::move(data));
	}

	void bdStorage::map_publisher_resource_variant(const std::string& expression, resource_variant resource)
	{
		if (resource.valueless_by_exception())
		{
			throw std::runtime_error("Publisher resource variant is empty!");
		}

		this->publisher_resources_.emplace_back(std::regex{expression}, std::move(resource));
	}

	bool bdStorage::load_publisher_resource(const std::string& name, std::string& buffer)
	{
		for (const auto& resource : this->publisher_resources_)
		{
			if (std::regex_match(name, resource.first))
			{
				if (std::holds_alternative<std::string>(resource.second))
				{
					buffer = std::get<std::string>(resource.second);
				}
				else
				{
					buffer = std::get<callback>(resource.second)();
				}

				return true;
			}
		}

		console::demonware("[DW]: [bdStorage]: missing publisher file: %s\n", name.data());

		return false;
	}

	void bdStorage::listAllPublisherFiles(service_server* server, byte_buffer* buffer)
	{
		uint32_t date;
		uint16_t num_results, offset;
		std::string unk, filename, data;

		buffer->read_string(&unk);
		buffer->read_uint32(&date);
		buffer->read_uint16(&num_results);
		buffer->read_uint16(&offset);
		buffer->read_string(&filename);

		console::demonware("[DW]: [bdStorage]: list publisher files: %s\n", filename.data());

		auto reply = server->create_reply(this->task_id());

		if (this->load_publisher_resource(filename, data))
		{
			auto info = std::make_unique<bdFileInfo>();

			info->file_id = *reinterpret_cast<const uint64_t*>(utils::cryptography::sha1::compute(filename).data());
			info->filename = filename;
			info->create_time = 0;
			info->modified_time = info->create_time;
			info->file_size = uint32_t(data.size());
			info->owner_id = 0;
			info->visibility = false;
			info->checksum = "f5f1fb4ddd2d85e2ed9a28b3204125ec";

			reply.add(info);
		}

		reply.send();
	}

	void bdStorage::getPublisherFile(service_server* server, byte_buffer* buffer)
	{
		std::string unk, filename;
		buffer->read_string(&unk);
		buffer->read_string(&filename);

		console::demonware("[DW]: [bdStorage]: loading publisher file: %s\n", filename.data());

		std::string data;

		if (this->load_publisher_resource(filename, data))
		{
			console::demonware("[DW]: [bdStorage]: sending publisher file: %s, size: %lld\n", filename.data(), data.size());

			auto reply = server->create_reply(this->task_id());
			auto result = std::make_unique<bdFileData>(data);
			reply.add(result);
			reply.send();
		}
		else
		{
			server->create_reply(this->task_id(), BD_NO_FILE).send();
		}
	}

	std::string bdStorage::get_user_file_path(const std::string& name)
	{
		const auto regular_path = "iw7s/players2/user/"s;
		
		// disable this for now
		/*static const auto fs_game = game::Dvar_FindVar("fs_game");
		if (fs_game && fs_game->current.string && *fs_game->current.string)
		{
			return regular_path + fs_game->current.string + "/" + name;
		}*/

		return regular_path + name;
	}

	void bdStorage::uploadAndValidateFiles(service_server* server, byte_buffer* buffer) const
	{
		uint64_t owner;
		uint32_t numfiles;
		std::string game, platform;

		buffer->read_string(&game);
		buffer->read_uint64(&owner);
		buffer->read_string(&platform);
		buffer->read_uint32(&numfiles);

		auto reply = server->create_reply(this->task_id());

		for (uint32_t i = 0; i < numfiles; i++)
		{
			std::string filename, data;
			uint32_t unk;
			bool priv;

			buffer->read_string(&filename);
			buffer->read_blob(&data);
			buffer->read_uint32(&unk);
			buffer->read_bool(&priv);

			const auto path = get_user_file_path(filename);
			utils::io::write_file(path, data);

			auto info = std::make_unique<bdFile2>();

			info->unk1 = 0;
			info->unk2 = 0;
			info->unk3 = 0;
			info->priv = false;
			info->owner_id = owner;
			info->platform = platform;
			info->filename = filename;
			info->data = data;

			console::demonware("[DW]: [bdStorage]: set user file: %s\n", filename.data());

			reply.add(info);
		}

		reply.send();
	}

	void bdStorage::uploadFiles(service_server* server, byte_buffer* buffer) const
	{
		std::string game, platform;
		std::uint64_t owner;
		std::uint32_t numfiles;

		buffer->read_string(&game);
		buffer->read_uint64(&owner);
		buffer->read_string(&platform);
		buffer->read_uint32(&numfiles);

		auto reply = server->create_reply(this->task_id());

		for (uint32_t i = 0; i < numfiles; i++)
		{
			std::string filename, data;
			std::uint32_t version;
			bool priv;

			buffer->read_string(&filename);
			buffer->read_blob(&data);
			buffer->read_uint32(&version);
			buffer->read_bool(&priv);

			const auto path = get_user_file_path(filename);
			utils::io::write_file(path, data);

			auto info = std::make_unique<bdContextUserStorageFileInfo>();
			info->modifed_time = static_cast<uint32_t>(time(nullptr));
			info->create_time = info->modifed_time;
			info->priv = priv;
			info->owner_id = owner;
			info->account_type = platform;
			info->filename = filename;

			console::demonware("[DW]: [bdStorage]: set user file: %s\n", filename.data());

			reply.add(info);
		}

		reply.send();
	}

	void bdStorage::getFiles(service_server* server, byte_buffer* buffer) const
	{
		std::string context;
		buffer->read_string(&context);

		uint32_t count;
		buffer->read_uint32(&count);

		std::vector<std::pair<uint64_t, std::string>> user_ctxs;

		for (auto i = 0u; i < count; i++)
		{
			uint64_t user_id;
			std::string acc_type;
			buffer->read_uint64(&user_id);
			buffer->read_string(&acc_type);

			user_ctxs.emplace_back(user_id, acc_type);
		}

		buffer->read_uint32(&count);

		std::vector<std::string> filenames;

		for (auto i = 0u; i < count; i++)
		{
			std::string filename;
			buffer->read_string(&filename);
			filenames.push_back(std::move(filename));
		}

		auto reply = server->create_reply(this->task_id());
		for (size_t i = 0u; i < filenames.size(); i++)
		{
			auto entry = std::make_unique<bdFileQueryResult>();
			entry->user_id = user_ctxs.at(i).first;
			entry->platform = user_ctxs.at(i).second;
			entry->filename = filenames.at(i);
			entry->errorcode = 0;

			auto& name = filenames.at(i);
			std::string filedata;
			if (utils::io::read_file(get_user_file_path(name), &filedata))
			{
				entry->filedata = filedata;
				console::demonware("[DW]: [bdStorage]: get user file: %s\n", name.data());
			}
			else
			{
				entry->errorcode = game::BD_NO_FILE;
				console::demonware("[DW]: [bdStorage]: missing user file: %s\n", name.data());
			}

			reply.add(entry);
		}

		reply.send();
	}

	void bdStorage::getFile(service_server* server, byte_buffer* buffer) const
	{
		// TODO:
		auto reply = server->create_reply(this->task_id());
		reply.send();
	}
}
