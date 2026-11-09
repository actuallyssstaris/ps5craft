/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Mod, plugin and datapack delivery from public mod stores.
 *
 * PS5Craft does not host content. The store lists what the publisher's API
 * lists, and every file is downloaded from the publisher's own CDN and checked
 * against the hash that API publishes. Sources:
 *
 *   Modrinth  https://api.modrinth.com/v2   mods, plugins, datapacks
 *   Hangar    https://hangar.papermc.io/api/v1   Paper plugins
 *
 * The server software decides what a store offers and where a file goes:
 *
 *   vanilla  datapacks  ->  world/datapacks
 *   fabric   mods       ->  mods
 *   paper    plugins    ->  plugins
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps5craft {

enum class ModSource : std::uint8_t {
    kModrinth = 0,
    kHangar = 1,
};

struct ModProject {
    ModSource source = ModSource::kModrinth;
    std::string id;        /* Modrinth project id, Hangar slug */
    std::string slug;
    std::string title;
    std::string description;
    std::string icon_url;
    std::string project_type; /* mod | plugin | datapack */
    std::string client_side;  /* required | optional | unsupported */
    std::string server_side;  /* required | optional | unsupported */
    std::uint64_t downloads = 0;
    std::vector<std::string> loaders;
    std::vector<std::string> game_versions;
};

struct ModDependency {
    std::string project_id;
    std::string version_id;
    std::string name;
    std::string kind; /* required | optional | incompatible | embedded */
};

struct ModFile {
    ModSource source = ModSource::kModrinth;
    std::string project;
    std::string version_id;
    std::string version_name;
    std::string file_name;
    std::string url;
    std::uint64_t size = 0;
    std::string channel;   /* release | beta | alpha */
    std::string published; /* ISO date, newest first within a channel */
    std::string sha1;
    std::string sha256;
    std::string sha512;
    std::vector<std::string> game_versions;
    std::vector<std::string> loaders;
    std::vector<ModDependency> dependencies;
};

/* Parse a source name from the API. Unknown names fall back to Modrinth. */
ModSource mod_source_from_string(const std::string &name);
const char *mod_source_name(ModSource source);

/* The store a server software uses by default, or Hangar when asked for a
   Paper plugin. */
ModSource mod_default_source(const std::string &software);

/* Directory, relative to the instance folder, that receives the file. Empty
   when the software takes no content. */
const char *mod_content_dir(const std::string &software);

/* Search one store. game_version may be empty to search every version. */
bool mod_search(ModSource source, const std::string &software,
                const std::string &game_version, const std::string &query,
                int offset, std::vector<ModProject> &out, std::string &error);

/* Versions of one project that fit the software and game version. */
bool mod_versions(ModSource source, const std::string &project,
                  const std::string &software, const std::string &game_version,
                  std::vector<ModFile> &out, std::string &error);

/* True when the file targets this server software and game version. A store
   that publishes no loader or game list is trusted, because there is nothing
   to check against. */
bool mod_file_fits(const ModFile &file, const std::string &software,
                   const std::string &game_version);

/* Resolve one version, or the newest fitting one when version_id is empty. */
bool mod_resolve(ModSource source, const std::string &project,
                 const std::string &version_id, const std::string &software,
                 const std::string &game_version, ModFile &out,
                 std::string &error);

} // namespace ps5craft
