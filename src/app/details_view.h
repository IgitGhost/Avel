#pragma once

#include <QTextBrowser>

namespace avel::inventory {
struct Program;
}

namespace avel::app {

class ProgramTraces;

// Everything known about the selected program, as selectable text with clickable links.
class DetailsView : public QTextBrowser {
    Q_OBJECT

public:
    explicit DetailsView(QWidget* parent = nullptr);

    // nullptr shows a hint instead. The program must outlive its display.
    void setProgram(const inventory::Program* program);
    // Shows the program's install trace, if it has one; `traces` must outlive the view.
    void setTraces(const ProgramTraces* traces);

protected:
    void changeEvent(QEvent* event) override;

private:
    void render();
    void openLink(const QUrl& url);

    const inventory::Program* program_ = nullptr;
    const ProgramTraces* traces_ = nullptr;
};

}  // namespace avel::app
