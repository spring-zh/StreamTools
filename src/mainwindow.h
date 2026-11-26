#pragma once

#include "bitstreamtypes.h"

#include <QFutureWatcher>
#include <QMainWindow>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QSortFilterProxyModel;
class QStackedWidget;
class QTableView;
class QTabWidget;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;

namespace bitscope {

class FrameTableModel;
class Av1BlockProvider;
class HevcCuProvider;
class MacroblockView;
class PreviewProvider;
class TimelineWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    void openFile(const QString &path);
    void requestFileView();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void buildMenus();
    void applyTheme();
    void chooseFile();
    void parseFinished();
    void setViewMode(int index);
    void selectionChanged(const QModelIndex &current);
    void selectFrame(int frameIndex);
    void showFrame(int frameIndex);
    void showNal(int nalIndex);
    void updateVisiblePreviews();
    void updateMetrics();
    void updateFrameStatistics(int frameIndex);
    void updateCuTree(int frameIndex);
    void populateCuTreeChildren(QTreeWidgetItem *root);
    void populateFileView();
    void showFileBox(QTreeWidgetItem *item);
    void updateFilters();
    void clearDetails(const QString &message);

    StreamPtr m_stream;
    FrameTableModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    PreviewProvider *m_previews = nullptr;
    HevcCuProvider *m_hevcCuProvider = nullptr;
    Av1BlockProvider *m_av1BlockProvider = nullptr;
    QFutureWatcher<StreamPtr> m_watcher;

    QLabel *m_fileLabel = nullptr;
    QLabel *m_codecMetric = nullptr;
    QLabel *m_resolutionMetric = nullptr;
    QLabel *m_frameMetric = nullptr;
    QLabel *m_rateMetric = nullptr;
    QComboBox *m_mode = nullptr;
    QComboBox *m_typeFilter = nullptr;
    QLineEdit *m_search = nullptr;
    QTableView *m_table = nullptr;
    QStackedWidget *m_leftStack = nullptr;
    QTreeWidget *m_fileTree = nullptr;
    MacroblockView *m_macroView = nullptr;
    QLabel *m_unitInfo = nullptr;
    QLabel *m_frameStats = nullptr;
    QTreeWidget *m_slices = nullptr;
    QTreeWidget *m_cuTree = nullptr;
    QTreeWidget *m_syntax = nullptr;
    QPlainTextEdit *m_hex = nullptr;
    QTextBrowser *m_references = nullptr;
    QTextBrowser *m_summary = nullptr;
    QTabWidget *m_tabs = nullptr;
    TimelineWidget *m_timeline = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
    QString m_lastPath;
    int m_currentFrame = -1;
    bool m_preferFileView = false;
};

} // namespace bitscope
