#include "pgn_cache.hpp"

#include "unzip.hpp"

#include <chesserazade/net_fetcher.hpp>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>

namespace chesserazade::analyzer {

QString cache_dir() {
    const QByteArray xdg = qgetenv("XDG_CACHE_HOME");
    if (!xdg.isEmpty()) {
        return QString::fromUtf8(xdg) + QStringLiteral("/chesserazade");
    }
    const QByteArray home = qgetenv("HOME");
    if (!home.isEmpty()) {
        return QString::fromUtf8(home)
             + QStringLiteral("/.cache/chesserazade");
    }
    return QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation);
}

QString zip_path_for_zip(const QString& zip_file) {
    return cache_dir() + QLatin1Char('/') + zip_file;
}

QString pgn_path_for_zip(const QString& zip_file) {
    const QString stem = QFileInfo(zip_file).completeBaseName();
    return cache_dir() + QLatin1Char('/') + stem
         + QStringLiteral(".pgn");
}

std::optional<QString>
ensure_pgn(const QString& zip_file, QWidget* parent) {
    const QString pgn_path = pgn_path_for_zip(zip_file);
    if (QFileInfo::exists(pgn_path)) return pgn_path;

    const QString url = QStringLiteral(
        "https://www.pgnmentor.com/players/") + zip_file;

    const auto btn = QMessageBox::question(
        parent,
        QObject::tr("Download?"),
        QObject::tr("About to download:\n\n%1\n\nSize varies "
                    "(tens of KB to a few MB). Proceed?").arg(url),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);
    if (btn != QMessageBox::Yes) return std::nullopt;

    const QString cache = cache_dir();
    QDir().mkpath(cache);

    CurlFetcher fetcher;
    const auto r = fetcher.fetch(url.toStdString());
    if (!r) {
        QMessageBox::warning(parent,
            QObject::tr("Download failed"),
            QString::fromStdString(r.error().message));
        return std::nullopt;
    }

    const QString zip_path = zip_path_for_zip(zip_file);
    QFile zf(zip_path);
    if (!zf.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(parent,
            QObject::tr("Save failed"),
            QObject::tr("Could not open %1 for writing.")
                .arg(zip_path));
        return std::nullopt;
    }
    zf.write(r->data(), static_cast<qint64>(r->size()));
    zf.close();

    const auto uz = unzip(zip_path, cache);
    if (!uz) {
        QMessageBox::warning(parent,
            QObject::tr("Unzip failed"), uz.error().message);
        return std::nullopt;
    }

    if (!QFileInfo::exists(pgn_path)) {
        QMessageBox::warning(parent,
            QObject::tr("PGN missing"),
            QObject::tr("Download succeeded but the expected file\n"
                        "%1\nwas not produced by unzip. The "
                        "archive may use a non-standard layout.")
                .arg(pgn_path));
        return std::nullopt;
    }
    return pgn_path;
}

} // namespace chesserazade::analyzer
