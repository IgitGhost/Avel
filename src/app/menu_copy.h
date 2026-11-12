#pragma once

#include <QAction>
#include <QMenu>
#include <QObject>

namespace avel::app {

// Popup menus show a window's actions through copies. Qt's accessibility keeps one interface per
// action, made for the first menu that shows it; if that is a popup, which is gone once it closes,
// screen readers and UI Automation no longer see the action in the menu bar or the toolbar.
inline QAction* addCopy(QMenu& menu, QAction* action) {
    QAction* copy = menu.addAction(action->icon(), action->text());
    copy->setEnabled(action->isEnabled());
    copy->setShortcut(action->shortcut());
    copy->setToolTip(action->toolTip());
    copy->setObjectName(action->objectName());
    QObject::connect(copy, &QAction::triggered, action, &QAction::trigger);
    return copy;
}

}  // namespace avel::app
