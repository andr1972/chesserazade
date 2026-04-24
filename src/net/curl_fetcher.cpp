// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// `curl`-based implementation of `NetFetcher`.
///
/// Why shell out instead of linking libcurl: keeping the core
/// library dependency-free is a project invariant. The CLI is
/// allowed to call out to external tools — `curl` is almost
/// always present on the developer's machine, and shelling out
/// keeps HTTPS, redirects, and TLS certificate validation
/// entirely in `curl`'s lane.
///
/// We shell out with `popen` and consume stdout. URL validation
/// is minimal: we reject anything that contains whitespace, a
/// shell metacharacter, or does not begin with `https://` /
/// `http://`. A malformed URL would be a user error caught at
/// the CLI parse stage; this check is a belt-and-braces second
/// line.

#include <chesserazade/net_fetcher.hpp>

// popen/pclose require POSIX, not ISO C++. cstdio alone does not
// expose them under -std=c++23 strict mode; the feature-test
// macro opens the door.
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace chesserazade {

namespace {

[[nodiscard]] bool looks_like_url(std::string_view u) noexcept {
    if (u.starts_with("https://") || u.starts_with("http://")) {
        // Reject whitespace and shell metacharacters. This is a
        // backup — the real defense is that the URL is passed
        // as a single, quoted argv element to popen via the
        // command string below.
        for (char c : u) {
            if (c <= ' ' || c == '"' || c == '\'' || c == '`'
                || c == '\\' || c == '$' || c == ';' || c == '|'
                || c == '&' || c == '<' || c == '>' || c == '*'
                || c == '?' || c == '(' || c == ')') {
                return false;
            }
        }
        return true;
    }
    return false;
}

} // namespace

std::expected<std::string, FetchError>
CurlFetcher::fetch(std::string_view url) {
    if (!looks_like_url(url)) {
        return std::unexpected(FetchError{
            "url must begin with http:// or https:// and contain "
            "no whitespace or shell metacharacters"});
    }

    // -s silent, -L follow redirects, --max-time 30, -f fail on
    // HTTP ≥ 400. The URL is safe to interpolate because we
    // validated its character set above.
    std::string cmd = "curl -sfL --max-time 30 ";
    cmd.push_back('"');
    cmd.append(url);
    cmd.push_back('"');

    // popen/pclose are POSIX, so they live in the global
    // namespace rather than std::.
    std::FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe == nullptr) {
        return std::unexpected(FetchError{"failed to spawn curl (popen)"});
    }

    std::string body;
    std::array<char, 4096> buf{};
    while (true) {
        const std::size_t n = std::fread(buf.data(), 1, buf.size(), pipe);
        if (n == 0) break;
        body.append(buf.data(), n);
    }

    const int rc = ::pclose(pipe);
    if (rc != 0) {
        return std::unexpected(FetchError{
            "curl exited with status " + std::to_string(rc)
            + " (check URL, network, TLS)"});
    }
    return body;
}

} // namespace chesserazade
