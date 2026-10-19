#pragma once

#include <QImage>
#include <QObject>
#include <QStringList>
#include <QThreadPool>

#include <atomic>
#include <memory>

namespace avel::app {

// Extracts program icons on background threads. A source is a DisplayIcon value
// ("C:\App\app.exe,0", "C:\App\app.ico", "...\icon.dll,-101") or a packaged app's logo image.
class IconLoader : public QObject {
    Q_OBJECT

public:
    explicit IconLoader(int iconSize, QObject* parent = nullptr);
    ~IconLoader() override;

    // Loads every source in the background, replacing any unfinished earlier request.
    void load(const QStringList& sources);

    // Synchronous extraction; safe on any thread. Returns a null image on failure.
    static QImage extract(const QString& source, int size);

Q_SIGNALS:
    void iconLoaded(const QString& source, const QImage& image);

private:
    int iconSize_;
    // Bumped by every load() so results of an abandoned request are dropped.
    std::shared_ptr<std::atomic<int>> generation_ = std::make_shared<std::atomic<int>>(0);
    QThreadPool pool_;  // Declared last: destroyed (and waited for) first
};

}  // namespace avel::app
