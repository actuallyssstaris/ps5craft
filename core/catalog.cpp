/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "catalog.hpp"

#include <algorithm>
#include <cctype>

#include "http_client.hpp"
#include "json.hpp"

namespace ps5craft {
namespace {

const char *kMojangManifest =
    "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";
const char *kPaperApi = "https://fill.papermc.io/v3";
const char *kFabricMeta = "https://meta.fabricmc.net/v2";

bool get_json(const std::string &url, Json &out, std::string &error) {
    std::string text;
    HttpLimits limits;
    limits.max_bytes = 4ull * 1024 * 1024;
    if (!http_get(url, text, error, limits)) return false;
    if (!Json::parse(text, out)) {
        error = "invalid JSON from " + url;
        return false;
    }
    return true;
}

/* Mojang version JSON, keyed by version id. Resolved through the manifest. */
bool mojang_version_json(const std::string &version, Json &out,
                         std::string &error) {
    Json manifest;
    if (!get_json(kMojangManifest, manifest, error)) return false;
    std::string url;
    for (const Json &v : manifest["versions"].items()) {
        if (v["id"].as_string() == version) {
            url = v["url"].as_string();
            break;
        }
    }
    if (url.empty()) {
        error = "unknown Minecraft version: " + version;
        return false;
    }
    return get_json(url, out, error);
}

int java_major_from_mojang(const Json &version_json) {
    const Json &jv = version_json["javaVersion"];
    if (!jv.is_object()) return 0;
    return static_cast<int>(jv["majorVersion"].as_int(0));
}

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/* ---- vanilla ---- */

bool vanilla_versions(std::vector<CatalogVersion> &out, std::string &error) {
    Json manifest;
    if (!get_json(kMojangManifest, manifest, error)) return false;
    const Json &list = manifest["versions"];
    if (!list.is_array()) {
        error = "unexpected Mojang manifest";
        return false;
    }
    for (const Json &v : list.items()) {
        CatalogVersion cv;
        cv.id = v["id"].as_string();
        cv.channel = v["type"].as_string("release");
        cv.released = v["releaseTime"].as_string();
        /* Snapshots are installable, but they are not what "supported" means
           here; the dashboard marks them. */
        cv.installable = true;
        if (cv.id.empty()) continue;
        out.push_back(cv);
    }
    return true;
}

bool vanilla_resolve(const std::string &requested, SoftwareArtifact &out,
                     std::string &error) {
    Json manifest;
    if (!get_json(kMojangManifest, manifest, error)) return false;
    std::string version = requested;
    if (version.empty() || version == "latest")
        version = manifest["latest"]["release"].as_string();
    else if (version == "snapshot")
        version = manifest["latest"]["snapshot"].as_string();

    std::string url;
    for (const Json &v : manifest["versions"].items()) {
        if (v["id"].as_string() == version) {
            url = v["url"].as_string();
            break;
        }
    }
    if (url.empty()) {
        error = "unknown Minecraft version: " + version;
        return false;
    }
    Json vj;
    if (!get_json(url, vj, error)) return false;
    const Json &dl = vj["downloads"]["server"];
    if (!dl.is_object() || dl["url"].as_string().empty()) {
        error = "version " + version + " has no server download";
        return false;
    }
    out.software = "vanilla";
    out.version = version;
    out.url = dl["url"].as_string();
    out.file_name = "server.jar";
    out.sha1 = dl["sha1"].as_string();
    out.sha256 = dl["sha256"].as_string();
    out.size = dl["size"].as_int(-1);
    out.java_major = java_major_from_mojang(vj);
    out.note.clear();
    return true;
}

/* ---- paper (Fill v3; the v2 API was sunset) ---- */

bool paper_versions(std::vector<CatalogVersion> &out, std::string &error) {
    Json root;
    if (!get_json(std::string(kPaperApi) + "/projects/paper", root, error))
        return false;
    const Json &groups = root["versions"];
    if (!groups.is_object()) {
        error = "unexpected Paper API response";
        return false;
    }
    /* Versions are grouped by family, newest first; flatten in that order. */
    for (const auto &group : groups.members()) {
        for (const Json &v : group.second.items()) {
            CatalogVersion cv;
            cv.id = v.as_string();
            cv.channel = "stable";
            if (cv.id.empty()) continue;
            if (cv.id.find("-rc") != std::string::npos ||
                cv.id.find("-pre") != std::string::npos ||
                cv.id.find("-SNAPSHOT") != std::string::npos)
                continue;
            out.push_back(cv);
        }
    }
    return true;
}

bool paper_resolve(const std::string &requested, SoftwareArtifact &out,
                   std::string &error) {
    std::string version = requested;
    if (version.empty() || version == "latest") {
        std::vector<CatalogVersion> versions;
        if (!paper_versions(versions, error)) return false;
        if (versions.empty()) {
            error = "Paper API returned no versions";
            return false;
        }
        version = versions.front().id;
    }
    Json builds;
    const std::string url = std::string(kPaperApi) + "/projects/paper/versions/" +
                            version + "/builds?channel=STABLE";
    if (!get_json(url, builds, error)) return false;
    if (!builds.is_array() || builds.items().empty()) {
        error = "no stable Paper build for " + version;
        return false;
    }
    /* The API returns newest first. */
    const Json &best = builds.items().front();
    const Json &dl = best["downloads"]["server:default"];
    if (!dl.is_object() || dl["url"].as_string().empty()) {
        error = "Paper build has no server download";
        return false;
    }
    out.software = "paper";
    out.version = version;
    out.file_name = dl["name"].as_string("paper.jar");
    out.url = dl["url"].as_string();
    out.sha256 = dl["checksums"]["sha256"].as_string();
    out.size = dl["size"].as_int(-1);
    out.note = "Paper build " + std::to_string(best["id"].as_int(0)) +
               " (" + best["channel"].as_string("STABLE") + ")";

    /* The version endpoint publishes the exact minimum Java. */
    Json detail;
    std::string ignored;
    if (get_json(std::string(kPaperApi) + "/projects/paper/versions/" + version,
                 detail, ignored)) {
        out.java_major = static_cast<int>(
            detail["java"]["version"]["minimum"].as_int(0));
    }
    if (out.java_major == 0) {
        Json vj;
        if (mojang_version_json(version, vj, ignored))
            out.java_major = java_major_from_mojang(vj);
    }
    return true;
}

/* ---- fabric ---- */

bool fabric_versions(std::vector<CatalogVersion> &out, std::string &error) {
    Json root;
    if (!get_json(std::string(kFabricMeta) + "/versions/game", root, error))
        return false;
    if (!root.is_array()) {
        error = "unexpected Fabric meta response";
        return false;
    }
    for (const Json &v : root.items()) {
        if (!v["stable"].as_bool(false)) continue;
        CatalogVersion cv;
        cv.id = v["version"].as_string();
        cv.channel = "stable";
        if (!cv.id.empty()) out.push_back(cv);
    }
    return true;
}

bool fabric_resolve(const std::string &requested, SoftwareArtifact &out,
                    std::string &error) {
    std::string game = requested;
    if (game.empty() || game == "latest") {
        std::vector<CatalogVersion> versions;
        if (!fabric_versions(versions, error)) return false;
        if (versions.empty()) {
            error = "Fabric meta returned no stable game versions";
            return false;
        }
        game = versions.front().id;
    }

    Json loaders;
    if (!get_json(std::string(kFabricMeta) + "/versions/loader/" + game,
                  loaders, error))
        return false;
    if (!loaders.is_array() || loaders.items().empty()) {
        error = "no Fabric loader for Minecraft " + game;
        return false;
    }
    const std::string loader =
        loaders.items().front()["loader"]["version"].as_string();
    if (loader.empty()) {
        error = "unexpected Fabric loader response";
        return false;
    }

    Json installers;
    if (!get_json(std::string(kFabricMeta) + "/versions/installer", installers,
                  error))
        return false;
    std::string installer;
    for (const Json &i : installers.items()) {
        if (i["stable"].as_bool(false)) {
            installer = i["version"].as_string();
            break;
        }
    }
    if (installer.empty()) {
        error = "no stable Fabric installer";
        return false;
    }

    out.software = "fabric";
    out.version = game;
    out.file_name = "server.jar";
    out.url = std::string(kFabricMeta) + "/versions/loader/" + game + "/" +
              loader + "/" + installer + "/server/jar";
    out.note = "Fabric loader " + loader +
               "; the launcher downloads libraries on first start";
    Json vj;
    std::string ignored;
    if (mojang_version_json(game, vj, ignored))
        out.java_major = java_major_from_mojang(vj);
    return true;
}

} // namespace

std::vector<SoftwareInfo> catalog_software() {
    return {
        {"vanilla", "Vanilla", "Official Mojang server. Tested end to end.", true,
         ""},
        {"paper", "Paper", "High-performance fork. Tested end to end.", true, ""},
        {"fabric", "Fabric", "Mod loader. Tested end to end.", true,
         "Downloads libraries on first start, so the console needs internet "
         "then."},
        /* Untested options stay visible but disabled. */
        {"spigot", "Spigot", "Requires a build step; not supported.", false,
         "Not implemented."},
        {"purpur", "Purpur", "Paper fork; not verified on PS5.", false,
         "Not implemented."},
        {"forge", "Forge", "Installer based; not verified on PS5.", false,
         "Not implemented."},
        {"neoforge", "NeoForge", "Installer based; not verified on PS5.", false,
         "Not implemented."},
        {"quilt", "Quilt", "Loader based; not verified on PS5.", false,
         "Not implemented."},
    };
}

bool catalog_versions(const std::string &software,
                      std::vector<CatalogVersion> &out, std::string &error) {
    out.clear();
    const std::string id = lower(software);
    if (id == "vanilla") return vanilla_versions(out, error);
    if (id == "paper") return paper_versions(out, error);
    if (id == "fabric") return fabric_versions(out, error);
    error = "unsupported software: " + software;
    return false;
}

bool catalog_resolve(const std::string &software, const std::string &version,
                     SoftwareArtifact &out, std::string &error) {
    const std::string id = lower(software);
    if (id == "vanilla") return vanilla_resolve(version, out, error);
    if (id == "paper") return paper_resolve(version, out, error);
    if (id == "fabric") return fabric_resolve(version, out, error);
    error = "unsupported software: " + software;
    return false;
}

} // namespace ps5craft
