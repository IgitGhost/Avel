#pragma once

#include <QMainWindow>
#include <QStringList>
#include <QThreadPool>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/leftovers/scan.h"
#include "force_flow.h"
#include "hunter_target.h"
#include "leftover_model.h"

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

class ChangeStoreAccess;
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
    bool selectProgram(const std::wstring& id);
    void uninstallSelected();
    void forceSelected();
    void forceFolder();
    void startForce(ForceFlow::Request request);
    void previewLeftovers();
    void onPreviewFinished(std::shared_ptr<const leftovers::ScanResult> scan,
                           std::vector<LeftoverModel::Program> programs,
                           std::vector<leftovers::ScanTarget> targets);
    void findOrphans();
    void onOrphansFound(std::shared_ptr<const leftovers::ScanResult> scan);
    void onWindowHovered(const HunterTarget::Window& window);
    void onWindowPicked(const HunterTarget::Window& window, QPoint position);
    struct Hunt;
    void onHuntFinished(const std::shared_ptr<const Hunt>& hunt);
    void endProcess(std::uint32_t pid, const QString& name);
    void openUndoCenter();
    void onMaintenanceFinished(const QStringList& settled);
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
    ChangeStoreAccess* stores_ = nullptr;
    std::shared_ptr<const inventory::Inventory> inventory_;

    QMenu* fileMenu_ = nullptr;
    QMenu* viewMenu_ = nullptr;
    QMenu* languageMenu_ = nullptr;
    QMenu* toolsMenu_ = nullptr;
    QMenu* helpMenu_ = nullptr;
    QAction* refreshAction_ = nullptr;
    QAction* uninstallAction_ = nullptr;
    QAction* forceAction_ = nullptr;
    QAction* forceFolderAction_ = nullptr;
    QAction* openLocationAction_ = nullptr;
    QAction* previewLeftoversAction_ = nullptr;
    QAction* orphansAction_ = nullptr;
    QAction* undoCenterAction_ = nullptr;
    QAction* findAction_ = nullptr;
    QAction* exitAction_ = nullptr;
    QAction* systemLanguageAction_ = nullptr;
    QAction* aboutAction_ = nullptr;
    QAction* aboutQtAction_ = nullptr;
    QActionGroup* languageGroup_ = nullptr;

    QToolBar* toolBar_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QComboBox* categoryCombo_ = nullptr;
    HunterTarget* hunter_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QTableView* table_ = nullptr;
    DetailsView* details_ = nullptr;

    QLabel* countLabel_ = nullptr;
    QLabel* scanLabel_ = nullptr;
    QLabel* warningLabel_ = nullptr;
    QLabel* elevationLabel_ = nullptr;

    ForceFlow* forceFlow_ = nullptr;  // The one running, if any; it deletes itself
    // A leftover preview, the orphan finder or a Hunter lookup runs on the worker pool.
    bool busy_ = false;
    // Declared last so it's destroyed first: its destructor waits for a running task.
    QThreadPool workers_;
};

}  // namespace avel::app
