#pragma once

#include <QDialog>
#include <QString>

#include <memory>
#include <string>
#include <vector>

#include "core/changes/change_set.h"
#include "core/changes/store.h"

class QLabel;
class QPushButton;
class QTreeWidget;

namespace avel::app {

// Lists the change sets Avel keeps (30 days) and puts their items back: all of them, or the
// checked ones. Also deletes a set's backup on request.
class UndoCenterDialog : public QDialog {
    Q_OBJECT

public:
    // `unavailableReason` explains a null store.
    UndoCenterDialog(std::shared_ptr<changes::ChangeStore> store, const QString& unavailableReason,
                     QWidget* parent = nullptr);

    // Whether anything was restored.
    bool changedAnything() const { return changedAnything_; }

private:
    void reloadSets(const QString& select = {});
    void showSelectedSet();
    void restore(bool checkedOnly);
    void deleteSet();
    void updateButtons();
    QString selectedSetId() const;

    std::shared_ptr<changes::ChangeStore> store_;
    QString shownId_;
    changes::ChangePlan plan_;
    std::vector<changes::ItemStatus> statuses_;
    bool changedAnything_ = false;

    QLabel* setsLabel_ = nullptr;
    QTreeWidget* sets_ = nullptr;
    QLabel* itemsLabel_ = nullptr;
    QTreeWidget* items_ = nullptr;
    QPushButton* restoreAllButton_ = nullptr;
    QPushButton* restoreCheckedButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
};

}  // namespace avel::app
