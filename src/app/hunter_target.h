#pragma once

#include <QPoint>
#include <QString>
#include <QWidget>

#include <cstdint>

namespace avel::app {

// The Hunter: a crosshair on the toolbar. Press it, drag the pointer onto any program's window and
// let go to find the program it belongs to. An elevated window can't take drag-and-drop from
// Explorer, so this is how a running program is picked. Avel's own windows are ignored, and Esc
// or the right button cancels.
class HunterTarget : public QWidget {
    Q_OBJECT

public:
    explicit HunterTarget(QWidget* parent = nullptr);

    QSize sizeHint() const override;

    // A top-level window and the process it belongs to; pid 0 when there is none, or it's Avel's.
    struct Window {
        std::uint32_t pid = 0;
        QString title;
        QString processName;  // The executable's file name, when Windows tells
    };

    // The top-level window under a point on the screen, in physical pixels.
    static Window windowAt(QPoint physical);

Q_SIGNALS:
    // While dragging: the window under the pointer changed.
    void hovering(const Window& window);
    // Released over `window`; `position` is where, in Qt's (logical) screen coordinates.
    void picked(const Window& window, QPoint position);
    void cancelled();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void stopHunting();
    static Window windowUnderCursor();

    bool hunting_ = false;
    std::uintptr_t lastWindow_ = 0;  // The HWND last reported by hovering()
};

}  // namespace avel::app
