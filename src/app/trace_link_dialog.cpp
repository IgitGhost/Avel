#include "trace_link_dialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "program_text.h"

using namespace Qt::StringLiterals;

namespace avel::app {

namespace {

enum Column { NameColumn, VersionColumn, PublisherColumn };

constexpr int kProgramRole = Qt::UserRole;  // qulonglong: the index into Inventory::programs

}  // namespace

TraceLinkDialog::TraceLinkDialog(std::shared_ptr<const inventory::Inventory> inventory,
                                 const QString& traceName, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Link the trace to a program"));
    setObjectName(u"traceLinkDialog"_s);
    pool_.setMaxThreadCount(1);

    auto* intro = new QLabel(
        tr("Which installed program was installed during the trace of %1? It becomes the trace's program: "
           "removing that program uses the trace.")
            .arg(traceName),
        this);
    intro->setWordWrap(true);

    filterEdit_ = new QLineEdit(this);
    filterEdit_->setObjectName(u"programFilter"_s);
    filterEdit_->setAccessibleName(tr("Search programs"));
    filterEdit_->setPlaceholderText(tr("Search by name or publisher"));
    filterEdit_->setClearButtonEnabled(true);
    connect(filterEdit_, &QLineEdit::textChanged, this, &TraceLinkDialog::applyFilter);

    programs_ = new QTreeWidget(this);
    programs_->setObjectName(u"programList"_s);
    programs_->setAccessibleName(tr("Installed programs"));
    programs_->setColumnCount(3);
    programs_->setHeaderLabels({tr("Name"), tr("Version"), tr("Publisher")});
    programs_->setRootIsDecorated(false);
    programs_->setUniformRowHeights(true);
    programs_->setAllColumnsShowFocus(true);
    programs_->setSortingEnabled(true);
    programs_->sortByColumn(NameColumn, Qt::AscendingOrder);
    programs_->setColumnWidth(NameColumn, 360);
    programs_->setColumnWidth(VersionColumn, 120);
    connect(programs_, &QTreeWidget::currentItemChanged, this, &TraceLinkDialog::updateButtons);
    connect(programs_, &QTreeWidget::itemDoubleClicked, this, &TraceLinkDialog::accept);

    statusLabel_ = new QLabel(this);
    statusLabel_->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(this);
    linkButton_ = buttons->addButton(tr("Link"), QDialogButtonBox::AcceptRole);
    linkButton_->setObjectName(u"linkProgramButton"_s);
    linkButton_->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(filterEdit_);
    layout->addWidget(programs_, 1);
    layout->addWidget(statusLabel_);
    layout->addWidget(buttons);
    resize(760, 520);

    if (inventory) {
        setInventory(std::move(inventory));
        return;
    }
    statusLabel_->setText(tr("Looking for installed programs…"));
    updateButtons();
    pool_.start([this] {
        inventory::ScanOptions scan;
        scan.appx = false;
        auto scanned = std::make_shared<const inventory::Inventory>(inventory::scan_inventory(scan));
        QMetaObject::invokeMethod(
            this, [this, scanned = std::move(scanned)] { setInventory(scanned); }, Qt::QueuedConnection);
    });
}

TraceLinkDialog::~TraceLinkDialog() { pool_.waitForDone(); }

void TraceLinkDialog::setInventory(std::shared_ptr<const inventory::Inventory> inventory) {
    inventory_ = std::move(inventory);
    programs_->setSortingEnabled(false);
    programs_->clear();
    QList<QTreeWidgetItem*> rows;
    for (std::size_t i = 0; i < inventory_->programs.size(); ++i) {
        const auto& program = inventory_->programs[i];
        if (program.source == inventory::Source::Appx || program.display_name.empty()) {
            continue;
        }
        auto* row = new QTreeWidgetItem;
        row->setData(NameColumn, kProgramRole, static_cast<qulonglong>(i));
        row->setText(NameColumn, toQString(program.display_name));
        row->setToolTip(NameColumn, toQString(program.id));
        row->setText(VersionColumn, toQString(program.display_version));
        row->setText(PublisherColumn, toQString(program.publisher));
        rows << row;
    }
    programs_->addTopLevelItems(rows);
    programs_->setSortingEnabled(true);
    statusLabel_->setText(rows.isEmpty() ? tr("Avel found no installed programs.") : QString());
    statusLabel_->setVisible(rows.isEmpty());
    applyFilter(filterEdit_->text());
    filterEdit_->setFocus();
}

void TraceLinkDialog::applyFilter(const QString& text) {
    const QString wanted = text.trimmed();
    for (int row = 0; row < programs_->topLevelItemCount(); ++row) {
        QTreeWidgetItem* item = programs_->topLevelItem(row);
        item->setHidden(!wanted.isEmpty() && !item->text(NameColumn).contains(wanted, Qt::CaseInsensitive) &&
                        !item->text(PublisherColumn).contains(wanted, Qt::CaseInsensitive));
    }
    QTreeWidgetItem* current = programs_->currentItem();
    if (current == nullptr || current->isHidden()) {
        programs_->setCurrentItem(nullptr);
        for (int row = 0; row < programs_->topLevelItemCount(); ++row) {
            if (!programs_->topLevelItem(row)->isHidden()) {
                programs_->setCurrentItem(programs_->topLevelItem(row));
                break;
            }
        }
    }
    updateButtons();
}

void TraceLinkDialog::updateButtons() {
    const QTreeWidgetItem* current = programs_->currentItem();
    linkButton_->setEnabled(inventory_ && current != nullptr && !current->isHidden());
}

void TraceLinkDialog::accept() {
    const QTreeWidgetItem* current = programs_->currentItem();
    if (!inventory_ || current == nullptr || current->isHidden()) {
        return;
    }
    program_ =
        &inventory_
             ->programs[static_cast<std::size_t>(current->data(NameColumn, kProgramRole).toULongLong())];
    QDialog::accept();
}

}  // namespace avel::app
