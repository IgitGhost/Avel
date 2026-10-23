#include "hunter_target.h"

#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>

#include <windows.h>

#include <algorithm>
#include <iterator>
#include <string>

using namespace Qt::StringLiterals;

namespace avel::app {

namespace {

constexpr int kSize = 26;  // Logical pixels

QString processNameOf(DWORD pid) {
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return {};
    }
    wchar_t buffer[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(std::size(buffer));
    QString name;
    if (::QueryFullProcessImageNameW(process, 0, buffer, &size)) {
        name = QString::fromWCharArray(buffer, static_cast<qsizetype>(size));
        name = name.mid(name.lastIndexOf(u'\\') + 1);
    }
    ::CloseHandle(process);
    return name;
}

struct ChildSearch {
    DWORD framePid = 0;
    DWORD found = 0;
};

// A Store app's window is a frame that ApplicationFrameHost.exe owns; the app's own process owns
// a window inside it.
BOOL CALLBACK findAppWindow(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<ChildSearch*>(parameter);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    if (pid != 0 && pid != search->framePid) {
        search->found = pid;
        return FALSE;
    }
    return TRUE;
}

}  // namespace

HunterTarget::HunterTarget(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::CrossCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

QSize HunterTarget::sizeHint() const { return {kSize + 6, kSize + 2}; }

HunterTarget::Window HunterTarget::windowAt(QPoint physical) {
    HWND window = ::WindowFromPoint(POINT{physical.x(), physical.y()});
    HWND root = window != nullptr ? ::GetAncestor(window, GA_ROOT) : nullptr;
    if (root == nullptr) {
        return {};
    }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(root, &pid);
    if (pid == 0 || pid == ::GetCurrentProcessId()) {
        return {};
    }
    Window found;
    found.pid = pid;
    found.processName = processNameOf(pid);
    if (found.processName.compare(u"ApplicationFrameHost.exe"_s, Qt::CaseInsensitive) == 0) {
        ChildSearch search{pid, 0};
        ::EnumChildWindows(root, findAppWindow, reinterpret_cast<LPARAM>(&search));
        if (search.found != 0) {
            found.pid = search.found;
            found.processName = processNameOf(search.found);
        }
    }
    const int length = ::GetWindowTextLengthW(root);
    if (length > 0) {
        std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
        const int copied = ::GetWindowTextW(root, title.data(), length + 1);
        found.title = QString::fromWCharArray(title.data(), copied);
    }
    return found;
}

HunterTarget::Window HunterTarget::windowUnderCursor() {
    POINT cursor{};
    if (!::GetCursorPos(&cursor)) {
        return {};
    }
    return windowAt({cursor.x, cursor.y});
}

void HunterTarget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF area = QRectF(rect()).adjusted(3, 1, -3, -1);
    const QPointF center = area.center();
    const qreal radius = std::min(area.width(), area.height()) / 2 - 2;
    QColor color = palette().color(isEnabled() ? QPalette::WindowText : QPalette::PlaceholderText);
    if (hunting_) {
        // The crosshair is out on the screen with the pointer; its place stays empty.
        color.setAlpha(70);
    }
    QPen pen(color, 1.6);
    painter.setPen(pen);
    painter.drawEllipse(center, radius, radius);
    painter.drawEllipse(center, radius * 0.45, radius * 0.45);
    const qreal gap = radius * 0.2;
    const qreal reach = radius + 2;
    painter.drawLine(QPointF(center.x(), center.y() - reach), QPointF(center.x(), center.y() - gap));
    painter.drawLine(QPointF(center.x(), center.y() + gap), QPointF(center.x(), center.y() + reach));
    painter.drawLine(QPointF(center.x() - reach, center.y()), QPointF(center.x() - gap, center.y()));
    painter.drawLine(QPointF(center.x() + gap, center.y()), QPointF(center.x() + reach, center.y()));
    painter.setBrush(color);
    painter.drawEllipse(center, 1.2, 1.2);
}

void HunterTarget::mousePressEvent(QMouseEvent* event) {
    if (hunting_ && event->button() == Qt::RightButton) {
        stopHunting();
        Q_EMIT cancelled();
        return;
    }
    if (event->button() != Qt::LeftButton || hunting_) {
        return;
    }
    hunting_ = true;
    lastWindow_ = 0;
    grabMouse(Qt::CrossCursor);
    grabKeyboard();
    update();
}

void HunterTarget::mouseMoveEvent(QMouseEvent*) {
    if (!hunting_) {
        return;
    }
    POINT cursor{};
    ::GetCursorPos(&cursor);
    HWND window = ::WindowFromPoint(cursor);
    const auto root =
        reinterpret_cast<std::uintptr_t>(window != nullptr ? ::GetAncestor(window, GA_ROOT) : nullptr);
    if (root != lastWindow_) {
        lastWindow_ = root;
        Q_EMIT hovering(windowAt({cursor.x, cursor.y}));
    }
}

void HunterTarget::mouseReleaseEvent(QMouseEvent* event) {
    if (!hunting_ || event->button() != Qt::LeftButton) {
        return;
    }
    stopHunting();
    const Window window = windowUnderCursor();
    if (window.pid == 0) {
        Q_EMIT cancelled();
        return;
    }
    Q_EMIT picked(window, event->globalPosition().toPoint());
}

void HunterTarget::keyPressEvent(QKeyEvent* event) {
    if (hunting_ && event->key() == Qt::Key_Escape) {
        stopHunting();
        Q_EMIT cancelled();
        return;
    }
    QWidget::keyPressEvent(event);
}

void HunterTarget::stopHunting() {
    hunting_ = false;
    releaseKeyboard();
    releaseMouse();
    update();
}

}  // namespace avel::app
