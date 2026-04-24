// Copyright (c) 2024-2026 Andrzej Borucki
// SPDX-License-Identifier: Apache-2.0
//
#include "pgnmentor_index.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace chesserazade::analyzer {

namespace {

/// Candidate locations for the shipped index. The first file
/// that exists wins. Tried in priority order:
///   1. `CHESSERAZADE_SOURCE_DIR/data/pgnmentor_index.json`
///      (compile-time baked — developer build from any cwd).
///   2. `./data/pgnmentor_index.json` relative to the cwd.
[[nodiscard]] QStringList candidate_paths() {
    QStringList paths;
#ifdef CHESSERAZADE_SOURCE_DIR
    paths << QString::fromUtf8(CHESSERAZADE_SOURCE_DIR)
             + QStringLiteral("/data/pgnmentor_index.json");
#endif
    paths << QDir::current().filePath(
        QStringLiteral("data/pgnmentor_index.json"));
    return paths;
}

} // namespace

std::optional<std::vector<PgnMentorEntry>> load_pgnmentor_index() {
    QString path;
    for (const QString& p : candidate_paths()) {
        if (QFileInfo::exists(p)) { path = p; break; }
    }
    if (path.isEmpty()) return std::nullopt;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;

    const QByteArray bytes = f.readAll();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (doc.isNull() || !doc.isObject()) return std::nullopt;

    const QJsonArray players = doc.object().value("players").toArray();
    if (players.isEmpty()) return std::nullopt;

    std::vector<PgnMentorEntry> out;
    out.reserve(static_cast<std::size_t>(players.size()));
    for (const QJsonValue& v : players) {
        const QJsonObject obj = v.toObject();
        const QString name = obj.value("name").toString();
        const QString file = obj.value("file").toString();
        if (name.isEmpty() || file.isEmpty()) continue;
        out.push_back({name, file});
    }
    if (out.empty()) return std::nullopt;
    return out;
}

} // namespace chesserazade::analyzer
