/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Server software catalog: resolves a Minecraft version to a downloadable
   server artifact. Mirrors the Pelican egg install path (version manifest ->
   version JSON -> downloads.server.url) and adds hash verification. */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps5craft {

struct CatalogVersion {
    std::string id;
    std::string channel;   /* release | snapshot | stable */
    std::string released;  /* ISO-8601 when known */
    bool installable = true;
    std::string note;
};

struct SoftwareArtifact {
    std::string software;
    std::string version;
    std::string url;
    std::string file_name;    /* suggested name inside the instance */
    std::string sha1;         /* hex, empty when the source publishes none */
    std::string sha256;
    std::int64_t size = -1;
    int java_major = 0;       /* 0 = unknown */
    std::string note;         /* honest caveat shown in the dashboard */
};

/* Software ids the backend accepts, in display order. */
struct SoftwareInfo {
    std::string id;
    std::string name;
    std::string description;
    bool supported;           /* verified end to end on this build */
    std::string note;
};
std::vector<SoftwareInfo> catalog_software();

/* Version list for one software. */
bool catalog_versions(const std::string &software,
                      std::vector<CatalogVersion> &out, std::string &error);

/* Resolve a version (or "latest") to a concrete artifact + hashes. */
bool catalog_resolve(const std::string &software, const std::string &version,
                     SoftwareArtifact &out, std::string &error);

} // namespace ps5craft
