#pragma once

#include <QTextBrowser>

namespace avel::inventory {
struct Program;
}

namespace avel::app {

// Everything known about the selected program, as selectable text with clickable links.
class DetailsView : public QTextBrowser {
    Q_OBJECT

public:
    explicit DetailsView(QWidget* parent = nullptr);

    // nullptr shows a hint instead. The program must outlive its display.
    void setProgram(const inventory::Program* program);

protected:
    void changeEvent(QEvent* event) override;

private:
    void render();
    void openLink(const QUrl& url);

    const inventory::Program* program_ = nullptr;
};

}  // namespace avel::app
