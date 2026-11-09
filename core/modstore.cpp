/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "modstore.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

#include "http_client.hpp"
#include "json.hpp"

namespace ps5craft {
namespace {

const char *kModrinthApi = "https://api.modrinth.com/v2";
const char *kHangarApi = "https://hangar.papermc.io/api/v1";

bool get_json(const std::string &url, Json &out, std::string &error) {
    std::string text;
    HttpLimits limits;
    limits.max_bytes = 8ull * 1024 * 1024;
    if (!http_get(url, text, error, limits)) return false;
    if (!Json::parse(text, out)) {
        error = "invalid JSON from " + url;
        return false;
    }
    return true;
}

std::string url_encode(const std::string &text) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0f];
        }
    }
    return out;
}

std::string lower(std::string value) {
    for (char &c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

int channel_rank(const std::string &channel) {
    const std::string name = lower(channel);
    if (name == "release") return 0;
    if (name == "beta" || name == "snapshot") return 1;
    if (name == "alpha") return 2;
    return 3;
}

/* Best first: stable channel, then newest. Stores already sort by date, but
   the channel order is what a player expects to install. */
void sort_versions(std::vector<ModFile> &versions) {
    std::stable_sort(versions.begin(), versions.end(),
                     [](const ModFile &a, const ModFile &b) {
                         const int ra = channel_rank(a.channel);
                         const int rb = channel_rank(b.channel);
                         if (ra != rb) return ra < rb;
                         return a.published > b.published;
                     });
}

std::vector<std::string> string_array(const Json &value) {
    std::vector<std::string> out;
    if (!value.is_array()) return out;
    for (const Json &item : value.items())
        if (item.is_string()) out.push_back(item.as_string());
    return out;
}

std::uint64_t json_size(const Json &value) {
    if (value.is_number()) return static_cast<std::uint64_t>(value.as_number());
    if (value.is_string()) return std::strtoull(value.as_string().c_str(), nullptr, 10);
    return 0;
}

std::string project_type_for(const std::string &software) {
    if (software == "fabric") return "mod";
    if (software == "paper") return "plugin";
    return "datapack";
}

/* Modrinth loader tags. A plugin jar that declares one of these loads on
   Paper. Datapacks use the datapack tag. */
std::string modrinth_loaders(const std::string &software) {
    if (software == "fabric") return "[\"fabric\"]";
    if (software == "paper") return "[\"paper\",\"bukkit\",\"spigot\",\"purpur\"]";
    return "[\"datapack\"]";
}

/* Modrinth facets: each inner array is an OR, the outer array is an AND. */
std::string modrinth_facets(const std::string &software,
                            const std::string &game_version) {
    std::string facets = "[[\"project_type:" + project_type_for(software) + "\"]";
    if (software == "fabric") facets += ",[\"categories:fabric\"]";
    if (!game_version.empty())
        facets += ",[\"versions:" + game_version + "\"]";
    facets += "]";
    return facets;
}

bool parse_modrinth_search(const Json &root, std::vector<ModProject> &out) {
    const Json &hits = root["hits"];
    if (!hits.is_array()) return false;
    for (const Json &hit : hits.items()) {
        ModProject project;
        project.source = ModSource::kModrinth;
        project.id = hit["project_id"].as_string();
        project.slug = hit["slug"].as_string(project.id);
        project.title = hit["title"].as_string(project.slug);
        project.description = hit["description"].as_string();
        project.icon_url = hit["icon_url"].as_string();
        project.project_type = hit["project_type"].as_string("mod");
        project.downloads = json_size(hit["downloads"]);
        project.loaders = string_array(hit["categories"]);
        project.game_versions = string_array(hit["versions"]);
        if (!project.id.empty()) out.push_back(std::move(project));
    }
    return true;
}

bool parse_modrinth_version(const Json &version, ModFile &out) {
    out.source = ModSource::kModrinth;
    out.project = version["project_id"].as_string();
    out.version_id = version["id"].as_string();
    out.version_name = version["version_number"].as_string(
        version["name"].as_string(out.version_id));
    out.game_versions = string_array(version["game_versions"]);
    out.loaders = string_array(version["loaders"]);
    out.channel = version["version_type"].as_string("release");
    out.published = version["date_published"].as_string();

    const Json &files = version["files"];
    if (!files.is_array() || files.items().empty()) return false;
    const Json *chosen = nullptr;
    for (const Json &file : files.items()) {
        if (file["primary"].as_bool(false)) {
            chosen = &file;
            break;
        }
    }
    if (!chosen) chosen = &files.items().front();
    out.file_name = (*chosen)["filename"].as_string();
    out.url = (*chosen)["url"].as_string();
    out.size = json_size((*chosen)["size"]);
    const Json &hashes = (*chosen)["hashes"];
    out.sha1 = hashes["sha1"].as_string();
    out.sha512 = hashes["sha512"].as_string();

    for (const Json &dep : version["dependencies"].items()) {
        ModDependency item;
        item.project_id = dep["project_id"].as_string();
        item.version_id = dep["version_id"].as_string();
        item.name = dep["file_name"].as_string(item.project_id);
        item.kind = dep["dependency_type"].as_string("required");
        out.dependencies.push_back(std::move(item));
    }
    return !out.file_name.empty() && !out.url.empty();
}

bool modrinth_search(const std::string &software, const std::string &game_version,
                     const std::string &query, int offset,
                     std::vector<ModProject> &out, std::string &error) {
    std::string url = std::string(kModrinthApi) + "/search?query=" +
                      url_encode(query) + "&limit=20&offset=" +
                      std::to_string(offset) + "&index=downloads&facets=" +
                      url_encode(modrinth_facets(software, game_version));
    Json root;
    if (!get_json(url, root, error)) return false;
    if (!parse_modrinth_search(root, out)) {
        error = "unexpected Modrinth search response";
        return false;
    }
    return true;
}

bool modrinth_versions(const std::string &project, const std::string &software,
                       const std::string &game_version,
                       std::vector<ModFile> &out, std::string &error) {
    std::string url = std::string(kModrinthApi) + "/project/" +
                      url_encode(project) + "/version?loaders=" +
                      url_encode(modrinth_loaders(software));
    if (!game_version.empty())
        url += "&game_versions=" + url_encode("[\"" + game_version + "\"]");
    Json root;
    if (!get_json(url, root, error)) return false;
    if (!root.is_array()) {
        error = "unexpected Modrinth version response";
        return false;
    }
    for (const Json &version : root.items()) {
        ModFile file;
        if (parse_modrinth_version(version, file)) out.push_back(std::move(file));
    }
    sort_versions(out);
    return true;
}

bool hangar_search(const std::string &query, int offset,
                   std::vector<ModProject> &out, std::string &error) {
    std::string url = std::string(kHangarApi) + "/projects?limit=20&offset=" +
                      std::to_string(offset) + "&platform=PAPER&sort=-downloads";
    if (!query.empty()) url += "&q=" + url_encode(query);
    Json root;
    if (!get_json(url, root, error)) return false;
    const Json &result = root["result"];
    if (!result.is_array()) {
        error = "unexpected Hangar search response";
        return false;
    }
    for (const Json &hit : result.items()) {
        ModProject project;
        project.source = ModSource::kHangar;
        project.slug = hit["namespace"]["slug"].as_string();
        project.id = project.slug;
        project.title = hit["name"].as_string(project.slug);
        project.description = hit["description"].as_string();
        project.icon_url = hit["avatarUrl"].as_string();
        project.project_type = "plugin";
        project.downloads = json_size(hit["stats"]["downloads"]);
        project.loaders.push_back("paper");
        project.game_versions = string_array(hit["supportedPlatforms"]["PAPER"]);
        if (!project.id.empty()) out.push_back(std::move(project));
    }
    return true;
}

bool parse_hangar_version(const Json &version, const std::string &project,
                          ModFile &out) {
    out.source = ModSource::kHangar;
    out.project = project;
    out.version_name = version["name"].as_string();
    out.version_id = out.version_name;
    out.game_versions = string_array(version["platformDependencies"]["PAPER"]);
    out.loaders.push_back("paper");
    out.channel = lower(version["channel"]["name"].as_string("release"));
    out.published = version["createdAt"].as_string();

    const Json &download = version["downloads"]["PAPER"];
    const Json &info = download["fileInfo"];
    out.file_name = info["name"].as_string();
    out.size = json_size(info["sizeBytes"]);
    out.sha256 = lower(info["sha256Hash"].as_string());
    out.url = download["downloadUrl"].as_string();
    /* Projects that only point at another host are resolved through that
       store's API instead; a bare external URL has no checksum here. */
    return !out.file_name.empty() && !out.url.empty();
}

bool hangar_version_list(const std::string &project, const std::string &game_version,
                         std::vector<ModFile> &out, std::string &error) {
    std::string url = std::string(kHangarApi) + "/projects/" +
                      url_encode(project) + "/versions?limit=50&offset=0&platform=PAPER";
    Json root;
    if (!get_json(url, root, error)) return false;
    const Json &result = root["result"];
    if (!result.is_array()) {
        error = "unexpected Hangar version response";
        return false;
    }
    for (const Json &version : result.items()) {
        ModFile file;
        if (!parse_hangar_version(version, project, file)) continue;
        if (!game_version.empty() && !file.game_versions.empty()) {
            bool fits = false;
            for (const std::string &supported : file.game_versions)
                if (supported == game_version) fits = true;
            if (!fits) continue;
        }
        out.push_back(std::move(file));
    }
    sort_versions(out);
    return true;
}

} // namespace

ModSource mod_source_from_string(const std::string &name) {
    if (lower(name) == "hangar") return ModSource::kHangar;
    return ModSource::kModrinth;
}

const char *mod_source_name(ModSource source) {
    return source == ModSource::kHangar ? "hangar" : "modrinth";
}

ModSource mod_default_source(const std::string &software) {
    return software == "paper" ? ModSource::kHangar : ModSource::kModrinth;
}

const char *mod_content_dir(const std::string &software) {
    if (software == "fabric") return "mods";
    if (software == "paper") return "plugins";
    if (software == "vanilla") return "world/datapacks";
    return "";
}

bool mod_search(ModSource source, const std::string &software,
                const std::string &game_version, const std::string &query,
                int offset, std::vector<ModProject> &out, std::string &error) {
    out.clear();
    if (source == ModSource::kHangar) {
        if (software != "paper") {
            error = "Hangar serves Paper plugins only";
            return false;
        }
        return hangar_search(query, offset, out, error);
    }
    return modrinth_search(software, game_version, query, offset, out, error);
}

bool mod_versions(ModSource source, const std::string &project,
                  const std::string &software, const std::string &game_version,
                  std::vector<ModFile> &out, std::string &error) {
    out.clear();
    if (source == ModSource::kHangar) {
        if (software != "paper") {
            error = "Hangar serves Paper plugins only";
            return false;
        }
        return hangar_version_list(project, game_version, out, error);
    }
    return modrinth_versions(project, software, game_version, out, error);
}

bool mod_file_fits(const ModFile &file, const std::string &software,
                   const std::string &game_version) {
    if (!game_version.empty() && !file.game_versions.empty()) {
        bool found = false;
        for (const std::string &version : file.game_versions)
            if (version == game_version) found = true;
        if (!found) return false;
    }
    if (file.loaders.empty()) return true;
    std::vector<std::string> wanted;
    if (software == "fabric") wanted = {"fabric", "quilt"};
    else if (software == "paper") wanted = {"paper", "bukkit", "spigot", "purpur"};
    else if (software == "vanilla") wanted = {"datapack"};
    if (wanted.empty()) return false;
    for (const std::string &loader : file.loaders) {
        const std::string name = lower(loader);
        for (const std::string &accept : wanted)
            if (name == accept) return true;
    }
    return false;
}

bool mod_resolve(ModSource source, const std::string &project,
                 const std::string &version_id, const std::string &software,
                 const std::string &game_version, ModFile &out,
                 std::string &error) {
    if (source == ModSource::kModrinth && !version_id.empty()) {
        Json root;
        if (!get_json(std::string(kModrinthApi) + "/version/" +
                          url_encode(version_id),
                      root, error))
            return false;
        if (!parse_modrinth_version(root, out)) {
            error = "version has no downloadable file";
            return false;
        }
        if (!mod_file_fits(out, software, game_version)) {
            error = "this version does not target " + software + " " + game_version;
            return false;
        }
        return true;
    }
    std::vector<ModFile> versions;
    if (!mod_versions(source, project, software, game_version, versions, error))
        return false;
    if (version_id.empty()) {
        if (versions.empty()) {
            error = "no version fits this server";
            return false;
        }
        out = std::move(versions.front());
        return true;
    }
    for (ModFile &file : versions) {
        if (file.version_id == version_id || file.version_name == version_id) {
            if (!mod_file_fits(file, software, game_version)) {
                error = "this version does not target " + software + " " + game_version;
                return false;
            }
            out = std::move(file);
            return true;
        }
    }
    error = "version not found: " + version_id;
    return false;
}

} // namespace ps5craft
