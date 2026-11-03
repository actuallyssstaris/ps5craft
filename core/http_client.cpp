/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "http_client.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace ps5craft {
namespace {

struct Buffer {
    std::string data;
    std::uint64_t max = 0;
    bool overflow = false;
};

std::size_t write_buffer(char *ptr, std::size_t size, std::size_t nmemb,
                         void *userdata) {
    Buffer *b = static_cast<Buffer *>(userdata);
    const std::size_t n = size * nmemb;
    if (b->max && b->data.size() + n > b->max) {
        b->overflow = true;
        return 0; /* abort the transfer */
    }
    b->data.append(ptr, n);
    return n;
}

struct FileSink {
    std::FILE *f = nullptr;
    HttpProgress progress;
    bool aborted = false;
};

std::size_t write_file(char *ptr, std::size_t size, std::size_t nmemb,
                       void *userdata) {
    FileSink *s = static_cast<FileSink *>(userdata);
    const std::size_t n = size * nmemb;
    if (std::fwrite(ptr, 1, n, s->f) != n) return 0;
    return n;
}

int transfer_progress(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                      curl_off_t, curl_off_t) {
    FileSink *s = static_cast<FileSink *>(userdata);
    if (!s->progress) return 0;
    if (!s->progress(static_cast<std::uint64_t>(dlnow < 0 ? 0 : dlnow),
                     static_cast<std::uint64_t>(dltotal < 0 ? 0 : dltotal))) {
        s->aborted = true;
        return 1; /* abort */
    }
    return 0;
}

std::string user_agent() {
    return "PS5Craft/0.1 (+https://github.com/actuallyssstaris/ps5craft)";
}

/* libcurl is global-stateful; init once. */
bool curl_ready() {
    static const bool ok = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    return ok;
}

void apply_common(CURL *h, const std::string &url) {
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, user_agent().c_str());
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
    const std::string ca = http_ca_bundle();
    if (!ca.empty()) curl_easy_setopt(h, CURLOPT_CAINFO, ca.c_str());
}

std::string curl_error(CURLcode code, const char *buffer) {
    if (buffer && *buffer) return buffer;
    return curl_easy_strerror(code);
}

} // namespace

namespace {
std::string g_ca_bundle;
}

void http_set_ca_bundle(std::string path) { g_ca_bundle = std::move(path); }

std::string http_ca_bundle() {
    if (const char *e = std::getenv("PS5CRAFT_CA_BUNDLE")) {
        if (*e) return e;
    }
    if (!g_ca_bundle.empty()) return g_ca_bundle;
    if (const char *root = std::getenv("PS5CRAFT_DATA_ROOT")) {
        std::string p = std::string(root) + "/ca-bundle.crt";
        if (access(p.c_str(), R_OK) == 0) return p;
    }
    return std::string();
}

bool http_get(const std::string &url, std::string &out, std::string &error,
              const HttpLimits &limits) {
    out.clear();
    if (!curl_ready()) {
        error = "libcurl init failed";
        return false;
    }
    CURL *h = curl_easy_init();
    if (!h) {
        error = "libcurl handle failed";
        return false;
    }
    Buffer buf;
    buf.max = limits.max_bytes;
    char errbuf[CURL_ERROR_SIZE] = {0};
    apply_common(h, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_buffer);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, static_cast<long>(limits.timeout_s));
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT,
                     static_cast<long>(limits.connect_timeout_s));

    const CURLcode rc = curl_easy_perform(h);
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(h);

    if (rc != CURLE_OK) {
        error = buf.overflow ? "response larger than the allowed limit"
                             : curl_error(rc, errbuf);
        return false;
    }
    if (status < 200 || status >= 300) {
        error = "HTTP " + std::to_string(status);
        return false;
    }
    out = std::move(buf.data);
    return true;
}

bool http_download(const std::string &url, const std::string &path,
                   const HttpProgress &progress, std::string &error,
                   int timeout_s) {
    if (!curl_ready()) {
        error = "libcurl init failed";
        return false;
    }
    CURL *h = curl_easy_init();
    if (!h) {
        error = "libcurl handle failed";
        return false;
    }
    FileSink sink;
    sink.progress = progress;
    sink.f = std::fopen(path.c_str(), "wb");
    if (!sink.f) {
        curl_easy_cleanup(h);
        error = "cannot open " + path;
        return false;
    }
    char errbuf[CURL_ERROR_SIZE] = {0};
    apply_common(h, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, transfer_progress);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &sink);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, static_cast<long>(timeout_s));
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
    /* A stalled transfer should fail instead of hanging forever. */
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);

    const CURLcode rc = curl_easy_perform(h);
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(h);
    const bool flush_ok = std::fflush(sink.f) == 0;
    std::fclose(sink.f);

    if (sink.aborted) {
        error = "download cancelled";
        return false;
    }
    if (rc != CURLE_OK) {
        error = curl_error(rc, errbuf);
        return false;
    }
    if (status < 200 || status >= 300) {
        error = "HTTP " + std::to_string(status);
        return false;
    }
    if (!flush_ok) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

} // namespace ps5craft
