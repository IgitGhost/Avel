#pragma once

#include <QStyledItemDelegate>

namespace avel::app {

// Elides a column's text at the right: for prose in a view that elides its paths in the middle.
class RightElideDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        option->textElideMode = Qt::ElideRight;
    }
};

}  // namespace avel::app
