#pragma once

#include <QMainWindow>

#include <memory>
#include <vector>

class QAction;
class QActionGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QSplitter;
class QTableView;
class QToolBar;

namespace avel::inventory {
struct Inventory;
struct Program;
}  // namespace avel::inventory

namespace avel::app {

class DetailsView;
class IconLoader;
class InventoryLoader;
class LanguageManager;
class ProgramFilterModel;
class ProgramTableModel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(LanguageManager& languages, QWidget* parent = nullptr);

protected:
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void createActions();
    void createMenus();
    void createToolBar();
    void createCentralArea();
    void createStatusBar();
    void retranslateUi();
    void restoreLayout();
    void saveLayout() const;

    void onScanStarted();
    void onScanFinished(std::shared_ptr<const inventory::Inventory> inventory);
    void onSelectionChanged();
    void updateActions();
    std::vector<const inventory::Program*> selectedPrograms() const;
    void uninstallSelected();
    void openInstallLocation();
    void showTableMenu(const QPoint& position);
    void ensureColumnsFit();
    void updateCounts();
    void updateDiagnostics();
    void showAbout();

    LanguageManager& languages_;
    const bool elevated_;

    ProgramTableModel* model_ = nullptr;
    ProgramFilterModel* filter_ = nullptr;
    InventoryLoader* loader_ = nullptr;
    IconLoader* iconLoader_ = nullptr;
    std::shared_ptr<const inventory::Inventory> inventory_;

    QMenu* fileMenu_ = nullptr;
    QMenu* viewMenu_ = nullptr;
    QMenu* languageMenu_ = nullptr;
    QMenu* helpMenu_ = nullptr;
    QAction* refreshAction_ = nullptr;
    QAction* uninstallAction_ = nullptr;
    QAction* openLocationAction_ = nullptr;
    QAction* findAction_ = nullptr;
    QAction* exitAction_ = nullptr;
    QAction* systemLanguageAction_ = nullptr;
    QAction* aboutAction_ = nullptr;
    QAction* aboutQtAction_ = nullptr;
    QActionGroup* languageGroup_ = nullptr;

    QToolBar* toolBar_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QComboBox* categoryCombo_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QTableView* table_ = nullptr;
    DetailsView* details_ = nullptr;

    QLabel* countLabel_ = nullptr;
    QLabel* scanLabel_ = nullptr;
    QLabel* warningLabel_ = nullptr;
    QLabel* elevationLabel_ = nullptr;
};

}  // namespace avel::app
