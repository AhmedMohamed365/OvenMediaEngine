//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2018 AirenSoft. All rights reserved.
//
//==============================================================================
#include "config_manager.h"

#include <base/info/ome_version.h>
#include <monitoring/monitoring.h>
#include <sys/utsname.h>

#include <iostream>

#include "config_converter.h"
#include "config_logger_loader.h"
#include "config_private.h"
#include "items/items.h"

namespace cfg
{
	struct XmlWriter : pugi::xml_writer
	{
		ov::String result;

		void write(const void *data, size_t size) override
		{
			result.Append(static_cast<const char *>(data), size);
		}
	};

	ConfigManager::ConfigManager()
	{
		// Modify if supported xml version is added or changed

		// Current OME compatible with v8 & v9 & v10
		_supported_versions_map["Server"] = {8, 9, 10};
		_supported_versions_map["Logger"] = {2};
	}

	ConfigManager::~ConfigManager()
	{
	}

	void ConfigManager::CheckLegacyConfigs(ov::String config_path)
	{
		// LastConfig was used <= 0.12.10, but later version changed to <API><Storage>.
		// Inform the user that LastConfig is no longer used and throws an exception so that OME can be terminated.
		bool is_last_config_found		 = ov::PathManager::IsFile(ov::PathManager::Combine(config_path, CFG_LAST_CONFIG_FILE_NAME));
		bool is_legacy_last_config_found = ov::PathManager::IsFile(ov::PathManager::Combine(config_path, CFG_LAST_CONFIG_FILE_NAME_LEGACY));

		if (is_last_config_found || is_legacy_last_config_found)
		{
			throw CreateConfigError("Legacy config file found. Please migrate '%s' manually or delete it and run OME again.", is_last_config_found ? CFG_LAST_CONFIG_FILE_NAME : CFG_LAST_CONFIG_FILE_NAME_LEGACY);
		}
	}

	void ConfigManager::LoadConfigs(ov::String config_path)
	{
		if (config_path.IsEmpty())
		{
			// Default: <OME_HOME>/conf
			config_path = ov::PathManager::GetAppPath("conf");
		}

		CheckLegacyConfigs(config_path);

		LoadLoggerConfig(config_path);
		LoadServerConfig(config_path);

		_config_path = config_path;
	}

	void ConfigManager::ReloadConfigs()
	{
		LoadConfigs(_config_path);
	}

	void ConfigManager::LoadLicenseKey(const ov::String &config_path)
	{
		// Load from environment variable first
		auto license_key_env = std::getenv("OME_LICENSE_KEY");
		if (license_key_env != nullptr)
		{
			_license_key = license_key_env;
		}
		else
		{
			// Load from file
			auto file_path = ov::PathManager::Combine(config_path, LICENSE_STORAGE_FILE);

			std::ifstream fs(file_path);
			if (!fs.is_open())
			{
				return;
			}

			std::string line;
			std::getline(fs, line);
			fs.close();

			line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());

			_license_key = line.c_str();
		}

		logti("License key loaded: %s", _license_key.CStr());
	}

	void ConfigManager::LoadServerID(const ov::String &config_path)
	{
		{
			auto [result, server_id] = LoadServerIDFromStorage(config_path);
			if (result == true)
			{
				_server_id = server_id;
				return;
			}
		}

		{
			auto [result, server_id] = GenerateServerID();
			if (result == true)
			{
				_server_id = server_id;
				StoreServerID(config_path, server_id);
				return;
			}
		}
	}

	std::tuple<bool, ov::String> ConfigManager::LoadServerIDFromStorage(const ov::String &config_path) const
	{
		// If node id is empty, try to load ID from file
		auto node_id_storage = ov::PathManager::Combine(config_path, SERVER_ID_STORAGE_FILE);

		std::ifstream fs(node_id_storage);
		if (!fs.is_open())
		{
			return {false, ""};
		}

		std::string line;
		std::getline(fs, line);
		fs.close();

		line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());

		if (line.empty())
		{
			return {false, ""};
		}

		return {true, line.c_str()};
	}

	bool ConfigManager::StoreServerID(const ov::String &config_path, ov::String server_id)
	{
		// Store server_id to storage
		auto node_id_storage = ov::PathManager::Combine(config_path, SERVER_ID_STORAGE_FILE);

		std::ofstream fs(node_id_storage);
		if (!fs.is_open())
		{
			return false;
		}

		fs.write(server_id.CStr(), server_id.GetLength());
		fs.close();
		return true;
	}

	std::tuple<bool, ov::String> ConfigManager::GenerateServerID() const
	{
		return {true, ov::UUID().ToString()};
	}

	void ConfigManager::LoadLoggerConfig(const ov::String &config_path)
	{
		struct stat value			  = {0};

		ov::String logger_config_path = ov::PathManager::Combine(config_path, CFG_LOG_FILE_NAME);

		::memset(&_last_modified, 0, sizeof(_last_modified));
		if (::stat(logger_config_path, &value) == -1)
		{
			// There is no file or to open file error
			// OME will work with the default settings.
			logtw("There is no configuration file for logs : %s. OME will run with the default settings.", logger_config_path.CStr());
			return;
		}

		if (
#if defined(__APPLE__)
			(_last_modified.tv_sec == value.st_mtimespec.tv_sec) &&
			(_last_modified.tv_nsec == value.st_mtimespec.tv_nsec)
#else
			(_last_modified.tv_sec == value.st_mtim.tv_sec) &&
			(_last_modified.tv_nsec == value.st_mtim.tv_nsec)
#endif
		)
		{
			// File is not changed
			return;
		}

		::ov_log_reset_enable();

#if defined(__APPLE__)
		_last_modified = value.st_mtimespec;
#else
		_last_modified = value.st_mtim;
#endif

		auto logger_loader = std::make_shared<ConfigLoggerLoader>(logger_config_path);

		logger_loader->Parse();

		CheckValidVersion("Logger", ov::Converter::ToInt32(logger_loader->GetVersion()));

		auto log_path = logger_loader->GetLogPath();
		::ov_log_set_path(log_path.CStr());

		// Init stat log
		//TODO(Getroot): This is temporary code for testing. This will change to more elegant code in the future.
		::ov_stat_log_set_path(STAT_LOG_WEBRTC_EDGE_SESSION, log_path.CStr());
		::ov_stat_log_set_path(STAT_LOG_WEBRTC_EDGE_REQUEST, log_path.CStr());
		::ov_stat_log_set_path(STAT_LOG_WEBRTC_EDGE_VIEWERS, log_path.CStr());
		::ov_stat_log_set_path(STAT_LOG_HLS_EDGE_SESSION, log_path.CStr());
		::ov_stat_log_set_path(STAT_LOG_HLS_EDGE_REQUEST, log_path.CStr());
		::ov_stat_log_set_path(STAT_LOG_HLS_EDGE_VIEWERS, log_path.CStr());

		logti("Trying to set logfile in directory... (%s)", log_path.CStr());

		std::vector<std::shared_ptr<LoggerTagInfo>> tags = logger_loader->GetTags();

		for (auto iterator = tags.begin(); iterator != tags.end(); ++iterator)
		{
			auto name = (*iterator)->GetName();

			if (::ov_log_set_enable(name.CStr(), (*iterator)->GetLevel(), true) == false)
			{
				throw CreateConfigError("Could not set log level for tag: %s", name.CStr());
			}
		}

		logger_loader->Reset();
	}

	void ConfigManager::LoadServerConfig(const ov::String &config_path)
	{
		const char *XML_ROOT_NAME	  = "Server";
		ov::String server_config_path = ov::PathManager::Combine(config_path, CFG_MAIN_FILE_NAME);

		logti("Trying to load configurations... (%s)", server_config_path.CStr());
		DataSource data_source(DataType::Xml, config_path, CFG_MAIN_FILE_NAME, XML_ROOT_NAME);

		std::unique_lock lock(_server_mutex);

		_server = std::make_shared<Server>();
		_server->SetItemName(XML_ROOT_NAME);
		_server->FromDataSource(data_source);

		CheckValidVersion(XML_ROOT_NAME, ov::Converter::ToInt32(_server->GetVersion()));

		logtt("Validating omit rules...");
		_server->ValidateOmitJsonNameRules();

		LoadServerID(config_path);
		_server->SetID(_server_id);

		LoadLicenseKey(config_path);
		_server->SetLicenseKey(_license_key);
	}

	void ConfigManager::CheckValidVersion(const ov::String &name, int version)
	{
		auto versions_iterator = _supported_versions_map.find(name);

		if (versions_iterator == _supported_versions_map.end())
		{
			throw CreateConfigError("Cannot find conf XML (%s.xml)", name.CStr());
		}

		auto supported_versions = versions_iterator->second;

		if (version == 0)
		{
			throw CreateConfigError(
				"Could not obtain version in your XML. If you have upgraded OME, see misc/conf_examples/%s.xml",
				name.CStr());
		}

		auto version_iterator = std::find(supported_versions.begin(), supported_versions.end(), version);

		if (version_iterator == supported_versions.end())
		{
			ov::String description;

			description.Format(
				"The version of %s.xml is outdated (Your XML version: %d).\n",
				name.CStr(), version);

			description.AppendFormat(
				"If you have upgraded OME, see misc/conf_examples/%s.xml\n",
				name.CStr());

			if (version <= 7)
			{
				description.AppendFormat("Major Changes (v7 -> v8):\n");
				description.AppendFormat(" - Added <Server>.<Bind>.<Managers>.<API> for setting API binding port\n");
				description.AppendFormat(" - Added <Server>.<API> for setting API server\n");
				description.AppendFormat(" - Added <Server>.<VirtualHosts>.<VirtualHost>.<Applications>.<Application>.<OutputProfiles>\n");
				description.AppendFormat(" - Changed <Server>.<VirtualHosts>.<VirtualHost>.<Domain> to <Host>\n");
				description.AppendFormat(" - Changed <CrossDomain> to <CrossDomains>\n");
				description.AppendFormat(" - Deleted <Server>.<VirtualHosts>.<VirtualHost>.<Applications>.<Application>.<Streams>\n");
				description.AppendFormat(" - Deleted <Server>.<VirtualHosts>.<VirtualHost>.<Applications>.<Application>.<Encodes>\n");
			}

			if (version <= 8)
			{
				description.AppendFormat("Major Changes (v8 -> v9):\n");
				description.AppendFormat(" - Added <Server>.<Bind>.<Managers>.<API>.<Storage> to store configs created using API\n");
			}

			if (version <= 9)
			{
				description.AppendFormat("Major Changes (v9 -> v10):\n");
				description.AppendFormat(" - Added <Server>.<Bind>.<Managers>.<API>.<CrossDomains>\n");
			}

			throw CreateConfigError("%s", description.CStr());
		}
	}

	void ConfigManager::UpdateRestPullInXml(const ov::String& vhost_name, const ov::String& app_name, const ov::String& stream_name, const std::vector<ov::String>& url_list, bool is_add)
	{
		std::unique_lock lock(_config_mutex);
		ov::String server_config_path = ov::PathManager::Combine(_config_path, CFG_MAIN_FILE_NAME);

		pugi::xml_document doc;
		pugi::xml_parse_result result = doc.load_file(server_config_path.CStr(), pugi::parse_default | pugi::parse_comments | pugi::parse_declaration);
		if (!result)
		{
			logte("Failed to load Server.xml for updating REST pull: %s", result.description());
			return;
		}

		pugi::xml_node server_node = doc.child("Server");
		if (!server_node) return;

		pugi::xml_node virtual_hosts = server_node.child("VirtualHosts");
		if (!virtual_hosts) return;

		for (pugi::xml_node vhost = virtual_hosts.child("VirtualHost"); vhost; vhost = vhost.next_sibling("VirtualHost"))
		{
			if (ov::String(vhost.child("Name").child_value()) == vhost_name)
			{
				pugi::xml_node origins = vhost.child("Origins");
				if (!origins)
				{
					if (!is_add) break; // Nothing to remove
					origins = vhost.append_child("Origins");
				}

				ov::String target_location = ov::String::FormatString("/%s/%s", app_name.CStr(), stream_name.CStr());

				// Remove existing origin with this location
				for (pugi::xml_node origin = origins.child("Origin"); origin; )
				{
					pugi::xml_node next = origin.next_sibling("Origin");
					if (ov::String(origin.child("Location").child_value()) == target_location)
					{
						origins.remove_child(origin);
					}
					origin = next;
				}

				if (is_add)
				{
					// Add new origin
					pugi::xml_node new_origin = origins.append_child("Origin");
					new_origin.append_child("Location").text().set(target_location.CStr());
					pugi::xml_node pass = new_origin.append_child("Pass");

					ov::String scheme;
					if (!url_list.empty())
					{
						// URL format is likely <scheme>://...
						std::string first_url = url_list[0].CStr();
						size_t scheme_end = first_url.find("://");
						if (scheme_end != std::string::npos)
						{
							scheme = ov::String(first_url.substr(0, scheme_end).c_str()).LowerCaseString();
						}
					}

					if (!scheme.IsEmpty())
					{
						pass.append_child("Scheme").text().set(scheme.CStr());
					}

					pugi::xml_node urls = pass.append_child("Urls");
					for (const auto& url : url_list)
					{
						std::string url_str = url.CStr();
						size_t scheme_end = url_str.find("://");
						if (scheme_end != std::string::npos)
						{
							url_str = url_str.substr(scheme_end + 3);
						}
						urls.append_child("Url").text().set(url_str.c_str());
					}
				}

				if (!doc.save_file(server_config_path.CStr(), "\t", pugi::format_default | pugi::format_save_file_text))
				{
					logte("Failed to save updated Server.xml to %s", server_config_path.CStr());
				}
				break;
			}
		}
	}
}  // namespace cfg