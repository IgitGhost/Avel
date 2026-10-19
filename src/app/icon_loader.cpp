#include "icon_loader.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <windows.h>
#include <shlobj.h>

#include <algorithm>

#include "core/platform/user_profiles.h"

using namespace Qt::StringLiterals;

namespace avel::app {

namespace {

constexpr int kBatchSize = 16;

// Packaged apps reference "Assets\StoreLogo.png" but ship scale- or size-qualified variants such
// as "StoreLogo.scale-200.png" or "StoreLogo.targetsize-48.png"; pick the best one.
QString resolve_qualified_image(const QString& path, int size) {
    const QFileInfo info(path);
    if (info.exists()) {
        return path;
    }
    const QDir directory = info.dir();
    const QStringList candidates =
        directory.entryList({info.completeBaseName() + u".*."_s + info.suffix()}, QDir::Files);
    if (candidates.isEmpty()) {
        return {};
    }
    static const QRegularExpression qualifier(u"(scale|targetsize)-(\\d+)"_s);
    QString best;
    int best_score = -1;
    for (const auto& candidate : candidates) {
        const auto match = qualifier.match(candidate);
        if (!match.hasMatch() || candidate.contains(u"contrast"_s)) {
            continue;
        }
        // Prefer the smallest image that is still at least `size` pixels; scale-N means N% of the
        // nominal 50 px store logo.
        const int value = match.captured(2).toInt();
        const int pixels = match.captured(1) == u"scale"_s ? 50 * value / 100 : value;
        const int score = pixels >= size ? 10000 - pixels : pixels;
        if (score > best_score) {
            best_score = score;
            best = candidate;
        }
    }
    return directory.filePath(best.isEmpty() ? candidates.front() : best);
}

QImage load_image_file(const QString& path, int size) {
    const QString resolved = resolve_qualified_image(path, size);
    if (resolved.isEmpty()) {
        return {};
    }
    QImage image(resolved);
    if (image.isNull()) {
        return {};
    }
    return image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QImage load_icon_resource(const QString& path, int index, int size) {
    HICON icon = nullptr;
    const std::wstring file = path.toStdWString();
    if (::SHDefExtractIconW(file.c_str(), index, 0, &icon, nullptr, static_cast<UINT>(size)) != S_OK ||
        !icon) {
        return {};
    }
    QImage image = QImage::fromHICON(icon);
    ::DestroyIcon(icon);
    return image;
}

}  // namespace

IconLoader::IconLoader(int iconSize, QObject* parent) : QObject(parent), iconSize_(iconSize) {
    pool_.setMaxThreadCount(std::clamp(QThread::idealThreadCount(), 1, 4));
}

IconLoader::~IconLoader() {
    ++*generation_;
    pool_.clear();
    pool_.waitForDone();
}

void IconLoader::load(const QStringList& sources) {
    const int generation = ++*generation_;
    pool_.clear();
    for (qsizetype start = 0; start < sources.size(); start += kBatchSize) {
        const QStringList batch = sources.mid(start, kBatchSize);
        pool_.start([this, batch, generation, counter = generation_, size = iconSize_] {
            for (const auto& source : batch) {
                if (counter->load() != generation) {
                    return;  // Superseded by a newer request
                }
                QImage image = extract(source, size);
                if (image.isNull()) {
                    continue;
                }
                QMetaObject::invokeMethod(
                    this,
                    [this, source, image = std::move(image), generation, counter] {
                        if (counter->load() == generation) {
                            Q_EMIT iconLoaded(source, image);
                        }
                    },
                    Qt::QueuedConnection);
            }
        });
    }
}

QImage IconLoader::extract(const QString& source, int size) {
    QString path = source.trimmed();
    int index = 0;
    // "path,index"; a negative index is a resource ID.
    if (const auto comma = path.lastIndexOf(u','); comma > 0) {
        bool is_number = false;
        const int value = path.mid(comma + 1).trimmed().toInt(&is_number);
        if (is_number) {
            index = value;
            path = path.left(comma).trimmed();
        }
    }
    path.remove(u'"');
    path = QString::fromStdWString(platform::expand_environment(path.toStdWString()));
    if (path.isEmpty()) {
        return {};
    }
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == u"png"_s || suffix == u"jpg"_s || suffix == u"jpeg"_s || suffix == u"bmp"_s ||
        suffix == u"gif"_s) {
        return load_image_file(path, size);
    }
    return load_icon_resource(path, index, size);
}

}  // namespace avel::app
