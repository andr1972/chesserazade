// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
/// Tests for the `NetFetcher` abstraction.
///
/// The production fetcher shells out to `curl`, so it cannot be
/// exercised offline. These tests cover the interface contract
/// via a `FakeFetcher` that hands back canned responses.
#include <chesserazade/net_fetcher.hpp>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <string_view>

using namespace chesserazade;

namespace {

/// Minimal stub fetcher for tests. URLs map to canned responses
/// or to pre-arranged errors.
class FakeFetcher final : public NetFetcher {
public:
    std::map<std::string, std::string> ok_responses;
    std::map<std::string, std::string> errors;

    std::expected<std::string, FetchError>
    fetch(std::string_view url) override {
        const std::string key{url};
        if (auto it = errors.find(key); it != errors.end()) {
            return std::unexpected(FetchError{it->second});
        }
        if (auto it = ok_responses.find(key); it != ok_responses.end()) {
            return it->second;
        }
        return std::unexpected(FetchError{"FakeFetcher: no canned response"});
    }
};

} // namespace

TEST_CASE("NetFetcher: canned success returns the body", "[fetch]") {
    FakeFetcher f;
    f.ok_responses["https://example.test/game.pgn"] =
        "[Event \"t\"]\n\n1. e4 *\n";

    auto r = f.fetch("https://example.test/game.pgn");
    REQUIRE(r.has_value());
    REQUIRE(r->find("[Event") == 0);
}

TEST_CASE("NetFetcher: canned error is propagated", "[fetch]") {
    FakeFetcher f;
    f.errors["https://example.test/404"] = "HTTP 404";
    auto r = f.fetch("https://example.test/404");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message == "HTTP 404");
}

TEST_CASE("NetFetcher: unknown URL produces an error by default",
          "[fetch]") {
    FakeFetcher f;
    auto r = f.fetch("https://example.test/unknown");
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("CurlFetcher: rejects non-http URLs", "[fetch][curl]") {
    CurlFetcher f;
    // file:// and ssh:// must be rejected without ever touching
    // the network — the abstraction is HTTPS-only by design.
    auto r1 = f.fetch("file:///etc/passwd");
    REQUIRE_FALSE(r1.has_value());
    auto r2 = f.fetch("ssh://user@host/path");
    REQUIRE_FALSE(r2.has_value());
}

TEST_CASE("CurlFetcher: rejects URLs with shell metacharacters",
          "[fetch][curl]") {
    CurlFetcher f;
    auto r = f.fetch("https://example.test/;rm -rf /");
    REQUIRE_FALSE(r.has_value());
}
