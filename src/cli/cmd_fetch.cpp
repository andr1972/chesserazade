#include "cli/cmd_fetch.hpp"

#include <chesserazade/net_fetcher.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace chesserazade::cli {

namespace {

// ---------------------------------------------------------------------------
// Cache directory and filename
// ---------------------------------------------------------------------------

/// Resolve the cache root. Honors `XDG_CACHE_HOME` when set,
/// otherwise falls back to `$HOME/.cache`. The final component
/// is always `chesserazade/`. Returns empty if neither env var
/// is present.
[[nodiscard]] std::filesystem::path cache_root() {
    namespace fs = std::filesystem;
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
        return fs::path(xdg) / "chesserazade";
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        return fs::path(home) / ".cache" / "chesserazade";
    }
    return {};
}

/// 64-bit FNV-1a hash of the URL. The hex string is the file
/// stem in the cache — stable across runs, safe for a filesystem,
/// no collisions to worry about at realistic scale.
[[nodiscard]] std::string url_hash_hex(std::string_view url) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : url) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 0x100000001b3ULL;
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 0; i < 16; ++i) {
        out[static_cast<std::size_t>(15 - i)] =
            hex[h & 0xfu];
        h >>= 4;
    }
    return out;
}

/// Pick a sensible filename extension based on the URL's tail.
/// PGN by default; `.json` if the URL's path ends with `.json`;
/// `.txt` otherwise. Prevents applications opening the cache
/// file guessing wrong.
[[nodiscard]] std::string_view pick_extension(std::string_view url) {
    if (url.ends_with(".pgn"))  return ".pgn";
    if (url.ends_with(".json")) return ".json";
    if (url.find("/api/puzzle/") != std::string_view::npos) return ".json";
    return ".pgn";
}

[[nodiscard]] std::filesystem::path default_cache_path(std::string_view url) {
    auto root = cache_root();
    if (root.empty()) return {};
    return root / (url_hash_hex(url) + std::string{pick_extension(url)});
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

struct FetchOpts {
    std::string url;
    std::string out_path;
    bool show_help = false;
};

struct ParseResult {
    FetchOpts opts;
    std::string error;
};

ParseResult parse_args(std::span<const std::string_view> args) {
    ParseResult r;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto a = args[i];
        if (a == "--help" || a == "-h") {
            r.opts.show_help = true;
            return r;
        }
        if (a == "--url" || a == "--out") {
            if (i + 1 >= args.size()) {
                r.error = std::string{a} + " requires a value";
                return r;
            }
            if (a == "--url")  r.opts.url      = std::string{args[i + 1]};
            else               r.opts.out_path = std::string{args[i + 1]};
            ++i;
            continue;
        }
        r.error = "unknown option '" + std::string{a} + "'";
        return r;
    }
    return r;
}

void print_help(std::ostream& out) {
    out << "Usage: chesserazade fetch [--url <URL>] [--out <path>]\n"
        << "\n"
        << "Download a PGN or puzzle JSON to the local cache.\n"
        << "\n"
        << "Options:\n"
        << "  --url <URL>   Non-interactive: fetch this URL directly.\n"
        << "  --out <path>  Save to this path instead of the default\n"
        << "                cache location.\n"
        << "  -h, --help    Show this message.\n"
        << "\n"
        << "With no flags, prompts for a URL (or preset source). All\n"
        << "network calls require explicit user confirmation — nothing\n"
        << "is fetched in the background.\n";
}

// ---------------------------------------------------------------------------
// Interactive prompting
// ---------------------------------------------------------------------------

[[nodiscard]] std::optional<std::string> interactive_pick_url() {
    std::cout << "chesserazade fetch — interactive\n"
                 "  1) enter a URL directly\n"
                 "  2) Lichess puzzle by ID\n"
                 "  q) cancel\n";
    std::cout << "choice> " << std::flush;

    std::string choice;
    if (!std::getline(std::cin, choice)) return std::nullopt;
    if (choice == "q" || choice == "Q") return std::nullopt;

    if (choice == "1") {
        std::cout << "url> " << std::flush;
        std::string url;
        if (!std::getline(std::cin, url) || url.empty()) return std::nullopt;
        return url;
    }
    if (choice == "2") {
        std::cout << "Lichess puzzle ID> " << std::flush;
        std::string id;
        if (!std::getline(std::cin, id) || id.empty()) return std::nullopt;
        // Basic sanitation: allow only alphanumerics.
        for (char c : id) {
            if (!std::isalnum(static_cast<unsigned char>(c))) {
                std::cerr << "fetch: puzzle ID must be alphanumeric\n";
                return std::nullopt;
            }
        }
        return "https://lichess.org/api/puzzle/" + id;
    }

    std::cerr << "fetch: unrecognised choice '" << choice << "'\n";
    return std::nullopt;
}

[[nodiscard]] bool confirm(std::string_view url) {
    std::cout << "about to fetch: " << url << "\n"
              << "proceed? [y/N]> " << std::flush;
    std::string ans;
    if (!std::getline(std::cin, ans)) return false;
    return ans == "y" || ans == "Y" || ans == "yes";
}

// ---------------------------------------------------------------------------
// Core fetch + save
// ---------------------------------------------------------------------------

int do_fetch(NetFetcher& fetcher, std::string_view url,
             std::string_view out_path_override,
             std::ostream& out, std::ostream& err) {
    namespace fs = std::filesystem;

    fs::path out_path;
    if (!out_path_override.empty()) {
        out_path = out_path_override;
    } else {
        out_path = default_cache_path(url);
        if (out_path.empty()) {
            err << "fetch: cannot resolve cache directory "
                   "(set HOME or XDG_CACHE_HOME)\n";
            return 1;
        }
    }

    // Cache hit: skip the network entirely. HANDOFF §8 calls this
    // out as a hard requirement — a pulled archive never gets
    // re-downloaded.
    if (fs::exists(out_path)) {
        out << "cached: " << out_path.string() << " (not re-downloaded)\n";
        return 0;
    }

    auto result = fetcher.fetch(url);
    if (!result) {
        err << "fetch: " << result.error().message << '\n';
        return 1;
    }

    std::error_code ec;
    fs::create_directories(out_path.parent_path(), ec);
    if (ec) {
        err << "fetch: could not create cache dir '"
            << out_path.parent_path().string() << "': "
            << ec.message() << '\n';
        return 1;
    }
    std::ofstream f(out_path, std::ios::binary);
    if (!f) {
        err << "fetch: could not open '" << out_path.string()
            << "' for writing\n";
        return 1;
    }
    f.write(result->data(), static_cast<std::streamsize>(result->size()));
    out << "saved:  " << out_path.string() << "  ("
        << result->size() << " bytes)\n";
    return 0;
}

} // namespace

int cmd_fetch(std::span<const std::string_view> args) {
    const auto parsed = parse_args(args);
    if (parsed.opts.show_help) {
        print_help(std::cout);
        return 0;
    }
    if (!parsed.error.empty()) {
        std::cerr << "fetch: " << parsed.error << "\n\n";
        print_help(std::cerr);
        return 1;
    }

    std::string url = parsed.opts.url;
    const bool interactive = url.empty();

    if (interactive) {
        auto picked = interactive_pick_url();
        if (!picked) {
            std::cout << "fetch: cancelled.\n";
            return 0;
        }
        url = std::move(*picked);
    }

    if (interactive && !confirm(url)) {
        std::cout << "fetch: cancelled.\n";
        return 0;
    }

    CurlFetcher fetcher;
    return do_fetch(fetcher, url, parsed.opts.out_path,
                    std::cout, std::cerr);
}

} // namespace chesserazade::cli
