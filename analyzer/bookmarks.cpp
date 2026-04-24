#include "bookmarks.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

namespace chesserazade::analyzer {

// ---------------------------------------------------------------------------
// Ply ↔ notation
// ---------------------------------------------------------------------------

QString ply_to_notation(int ply) {
    if (ply <= 0) return QStringLiteral("0");
    // ply 2N-1 → white has just moved → "N w"
    // ply 2N   → black has just moved → "N b"
    const bool white_moved = (ply % 2 == 1);
    const int move_number = white_moved ? (ply + 1) / 2 : ply / 2;
    return QStringLiteral("%1 %2").arg(move_number)
                                  .arg(white_moved ? QLatin1Char('w')
                                                   : QLatin1Char('b'));
}

std::optional<int> notation_to_ply(const QString& s) {
    const QString t = s.trimmed();
    if (t.isEmpty()) return std::nullopt;
    if (t == QLatin1String("0")) return 0;

    static const QRegularExpression re(
        QStringLiteral("^\\s*(\\d+)\\s*([wbWB])\\s*$"));
    const auto m = re.match(t);
    if (!m.hasMatch()) return std::nullopt;

    bool ok = false;
    const int n = m.captured(1).toInt(&ok);
    if (!ok || n <= 0) return std::nullopt;
    const QChar side = m.captured(2).at(0).toLower();
    return (side == QLatin1Char('w')) ? (2 * n - 1) : (2 * n);
}

// ---------------------------------------------------------------------------
// Storage path + (de)serialisation
// ---------------------------------------------------------------------------

namespace {

constexpr int SCHEMA_VERSION = 1;

[[nodiscard]] QString app_data_dir() {
    // AppDataLocation already appends `<organization>/<app>` to
    // the base path (→ `.../chesserazade/chesserazade-analyzer`)
    // on Linux, which nests bookmarks one level deeper than we
    // want. Use GenericDataLocation (`~/.local/share`) and tack
    // on a single "chesserazade" segment so bookmarks sit at
    // `~/.local/share/chesserazade/bookmarks.json` — the same
    // directory any other chesserazade binary (CLI etc.) could
    // read from without caring which analyzer built this build.
    const QString base = QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation);
    return base + QStringLiteral("/chesserazade");
}

[[nodiscard]] QJsonObject bookmark_to_json(const Bookmark& b) {
    QJsonObject o;
    o.insert(QStringLiteral("zip"),     b.zip);
    o.insert(QStringLiteral("white"),   b.white);
    o.insert(QStringLiteral("black"),   b.black);
    o.insert(QStringLiteral("date"),    b.date);
    o.insert(QStringLiteral("event"),   b.event);
    o.insert(QStringLiteral("round"),   b.round);
    o.insert(QStringLiteral("eco"),     b.eco);
    o.insert(QStringLiteral("ply"),     b.ply);
    o.insert(QStringLiteral("label"),   b.label);
    o.insert(QStringLiteral("comment"), b.comment);
    o.insert(QStringLiteral("folder"),  b.folder);
    o.insert(QStringLiteral("created_ms"),
             static_cast<qint64>(b.created_ms));
    return o;
}

[[nodiscard]] Bookmark bookmark_from_json(const QJsonObject& o) {
    Bookmark b;
    b.zip        = o.value(QStringLiteral("zip"     )).toString();
    b.white      = o.value(QStringLiteral("white"   )).toString();
    b.black      = o.value(QStringLiteral("black"   )).toString();
    b.date       = o.value(QStringLiteral("date"    )).toString();
    b.event      = o.value(QStringLiteral("event"   )).toString();
    b.round      = o.value(QStringLiteral("round"   )).toString();
    b.eco        = o.value(QStringLiteral("eco"     )).toString();
    b.ply        = o.value(QStringLiteral("ply"     )).toInt(0);
    b.label      = o.value(QStringLiteral("label"   )).toString();
    b.comment    = o.value(QStringLiteral("comment" )).toString();
    b.folder     = o.value(QStringLiteral("folder"  )).toString();
    b.created_ms = static_cast<std::int64_t>(
        o.value(QStringLiteral("created_ms")).toVariant().toLongLong());
    return b;
}

} // namespace

QString bookmarks_file_path() {
    return app_data_dir() + QStringLiteral("/bookmarks.json");
}

std::optional<std::vector<Bookmark>> load_bookmarks() {
    const QString path = bookmarks_file_path();
    if (!QFileInfo::exists(path)) return std::vector<Bookmark>{};

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (doc.isNull() || !doc.isObject()) return std::nullopt;
    const QJsonArray arr =
        doc.object().value(QStringLiteral("bookmarks")).toArray();

    std::vector<Bookmark> out;
    out.reserve(static_cast<std::size_t>(arr.size()));
    for (const QJsonValue& v : arr) {
        if (!v.isObject()) continue;
        out.push_back(bookmark_from_json(v.toObject()));
    }
    return out;
}

bool save_bookmarks(const std::vector<Bookmark>& all) {
    const QString path = bookmarks_file_path();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonArray arr;
    for (const Bookmark& b : all) arr.append(bookmark_to_json(b));

    QJsonObject top;
    top.insert(QStringLiteral("schema_version"), SCHEMA_VERSION);
    top.insert(QStringLiteral("bookmarks"), arr);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QByteArray bytes =
        QJsonDocument(top).toJson(QJsonDocument::Indented);
    const qint64 written = f.write(bytes);
    return written == bytes.size();
}

// ---------------------------------------------------------------------------
// Fuzzy resolver
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] bool name_matches(const QString& bm_name,
                                const std::string& hdr_name) {
    if (bm_name.isEmpty() || hdr_name.empty()) return true;
    const QString h = QString::fromStdString(hdr_name);
    // Substring either way — typical snag: bookmark stored
    // "Fischer, Robert J." against a header saying just "Fischer".
    return h.contains(bm_name, Qt::CaseInsensitive)
        || bm_name.contains(h,  Qt::CaseInsensitive);
}

/// Treat '?' (and stretches of it) as wildcards. A chunk on one
/// side matches any chunk of the same length on the other.
///
/// "1972" in the bookmark vs "1972.07.11" in the header → match
///   (bookmark's year-only is a prefix of the header's full date).
/// "1972.??.??" vs "1972.07.11" → match
///   (unknowns wildcard to the header's real month/day).
/// "1972" vs "1973.01.01" → miss (years differ).
[[nodiscard]] bool date_matches(const QString& bm_date,
                                const std::string& hdr_date) {
    if (bm_date.isEmpty() || hdr_date.empty()) return true;
    const QString h = QString::fromStdString(hdr_date);
    const QString& a = bm_date;
    const QString& b = h;
    const qsizetype n = std::min(a.size(), b.size());
    for (qsizetype i = 0; i < n; ++i) {
        const QChar ca = a[i];
        const QChar cb = b[i];
        if (ca == QLatin1Char('?') || cb == QLatin1Char('?')) continue;
        if (ca != cb) return false;
    }
    return true;
}

[[nodiscard]] bool substr_match(const QString& bm, const std::string& hdr) {
    if (bm.isEmpty() || hdr.empty()) return true;
    return QString::fromStdString(hdr)
        .contains(bm, Qt::CaseInsensitive);
}

[[nodiscard]] bool exact_match(const QString& bm, const std::string& hdr) {
    if (bm.isEmpty() || hdr.empty()) return true;
    return QString::fromStdString(hdr)
        .compare(bm, Qt::CaseInsensitive) == 0;
}

} // namespace

std::optional<std::size_t>
resolve_game(const Bookmark& bm,
             const std::vector<PgnGameHeader>& headers) {
    // Pass 1: white + black.
    std::vector<std::size_t> pool;
    pool.reserve(headers.size());
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (name_matches(bm.white, headers[i].white)
            && name_matches(bm.black, headers[i].black)) {
            pool.push_back(i);
        }
    }
    if (pool.empty()) return std::nullopt;
    if (pool.size() == 1) return pool.front();

    // Pass 2: date.
    std::vector<std::size_t> by_date;
    by_date.reserve(pool.size());
    for (std::size_t i : pool) {
        if (date_matches(bm.date, headers[i].date)) by_date.push_back(i);
    }
    if (by_date.size() == 1) return by_date.front();
    if (!by_date.empty()) pool = std::move(by_date);

    // Pass 3: event (substring), round (exact), eco (exact).
    // Each tie-breaker is only applied if the bookmark has
    // the field; and we only narrow further as long as the
    // narrower set is non-empty.
    auto narrow = [&](auto pred) {
        std::vector<std::size_t> next;
        next.reserve(pool.size());
        for (std::size_t i : pool) if (pred(headers[i])) next.push_back(i);
        if (!next.empty()) pool = std::move(next);
    };
    if (!bm.event.isEmpty()) {
        narrow([&](const PgnGameHeader& h) {
            return substr_match(bm.event, h.event);
        });
    }
    if (pool.size() > 1 && !bm.round.isEmpty()) {
        narrow([&](const PgnGameHeader& h) {
            return exact_match(bm.round, h.round);
        });
    }
    if (pool.size() > 1 && !bm.eco.isEmpty()) {
        // PgnGameHeader has no `eco` yet; we pretend every
        // header's ECO is empty, which means this pass never
        // eliminates anyone. Left in so that extending the
        // indexer later automatically starts using ECO.
    }

    if (pool.size() == 1) return pool.front();
    return std::nullopt;
}

QStringList folders_in_use(const std::vector<Bookmark>& all) {
    QSet<QString> seen;
    for (const Bookmark& b : all) {
        if (!b.folder.isEmpty()) seen.insert(b.folder);
    }
    QStringList out(seen.begin(), seen.end());
    std::sort(out.begin(), out.end(),
              [](const QString& a, const QString& b) {
                  return a.compare(b, Qt::CaseInsensitive) < 0;
              });
    return out;
}

} // namespace chesserazade::analyzer
