// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// NetFetcher — abstract interface for pulling content over
/// HTTPS.
///
/// The core engine does not link any network library. Instead,
/// every HTTP call goes through `NetFetcher::fetch(url)`, which
/// returns the response body or a `FetchError`. The production
/// implementation (`CurlFetcher`) shells out to the system's
/// `curl` binary — a classical Unix composition that keeps
/// `chesserazade_core` free of libcurl, OpenSSL, or any other
/// third-party dependency. Tests inject a `FakeFetcher` with
/// canned responses, so the CLI's fetch flow can be exercised
/// offline.
///
/// Design constraints (from HANDOFF §8):
///   * All network calls are explicit and require user
///     confirmation — no background traffic.
///   * Responses are cached under `~/.cache/chesserazade/` so a
///     puzzle set or game archive pulled once is not
///     re-downloaded.
///   * Rate-limits and terms-of-service of the upstream sources
///     are the user's responsibility; the CLI surfaces the
///     target URL before dispatching the request.
#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace chesserazade {

struct FetchError {
    std::string message;
};

/// Fetch the body of `url` and return it as a string. HTTPS is
/// expected; the implementation is free to refuse plain HTTP.
/// Failure values carry a human-readable message suitable for
/// display in the CLI.
class NetFetcher {
public:
    virtual ~NetFetcher() = default;

    [[nodiscard]] virtual std::expected<std::string, FetchError>
    fetch(std::string_view url) = 0;

protected:
    NetFetcher() = default;
    NetFetcher(const NetFetcher&) = default;
    NetFetcher& operator=(const NetFetcher&) = default;
    NetFetcher(NetFetcher&&) = default;
    NetFetcher& operator=(NetFetcher&&) = default;
};

/// Concrete fetcher that invokes the system `curl` binary via
/// `popen`. Fails if `curl` is not on PATH. Follows redirects
/// (equivalent to `-L`) and enforces a 30-second timeout.
class CurlFetcher final : public NetFetcher {
public:
    [[nodiscard]] std::expected<std::string, FetchError>
    fetch(std::string_view url) override;
};

} // namespace chesserazade
