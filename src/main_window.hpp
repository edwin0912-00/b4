// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "display_transform.hpp"
#include "editor.hpp"
#include "graph.hpp"
#include "render_worker.hpp"
#include "timeline.hpp"
#include <QElapsedTimer>
#include <QMainWindow>
#include <QTimer>
class QApplication;
class QTreeWidget;
class QLineEdit;
class QTextEdit;
class QDoubleSpinBox;
class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QTabWidget;
class QTabBar;
class QStackedWidget;
class QTreeWidgetItem;
class QProgressBar;
class QSlider;
class QToolButton;
namespace motion {
void applyApplicationTheme(QApplication &);
class Viewer;
class PropertyPanel;
class AudioMix;
class AudioPlayback;
class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    Editor &editor() { return editor_; }
    Timeline *timeline() { return timeline_; }
    bool openProject(const QString &path);
    bool saveAs(const QString &path);
    void setTime(Time);
    Time currentTime() const { return now_; }
    QImage displayedImage() const { return displayedImage_; }
    QString lastError() const { return lastError_; }
    void authorProof(const QString &directory);
    void exportTo(const QString &directory, bool still = false, bool movie = false);
    void importMediaFile(const QString &, bool addLayer = false, Time at = {});
    void createCompositionFromAsset(Id);
    void addAssetLayer(Id, Time at, Id targetComp = 0);
    bool isPlaying() const { return playing_; }
    Id currentComposition() const { return comp_; }
  signals:
    void frameDisplayed();
    void mediaImported(motion::Id);
    void outputFinished(motion::ExportResult);

  protected:
    bool eventFilter(QObject *, QEvent *) override;
    void closeEvent(QCloseEvent *) override;

  private:
    void buildUi();
    void refresh();
    void refreshProperties();
    void requestFrame();
    void report(const QString &);
    void guarded(std::function<void()>);
    bool confirmDiscard();
    void addLayer(LayerKind);
    void importPng(const QString &);
    void deleteLayers();
    void duplicate();
    void precomposeSelection();
    void parentSelection();
    void addKey(PropertyRef);
    void selectGraphProperty(PropertyRef);
    void saveRecoveryNow();
    void setFilter(QString);
    bool prepareKeyAction();
    void togglePlayback();
    void beginPlayback();
    void advancePlayback();
    void stopPlayback();
    void updateAudio();
    Project footageProject() const;
    Project playbackProject() const;
    void chooseComposition(Id);
    void appendLog(QString);
    void configureComposition(bool create);
    void syncProjectSelection();
    void resetWorkspace();
    void filterProject();
    void setViewerMode(int);
    void alignSelection(int axis, int edge);
    Editor editor_;
    RenderWorker worker_;
    Id comp_ = 1;
    Time now_, footageTime_;
    PropertyRef graphProperty_ = Property::PositionX;
    QString path_, baseDirectory_, identity_, lastError_, actionLog_;
    bool refreshing_ = false, exporting_ = false, playing_ = false, inFlight_ = false;
    std::uint64_t requestId_ = 0;
    PreviewRequest active_;
    QImage displayedImage_;
    QElapsedTimer playbackClock_;
    Time playbackStart_;
    WorkArea playbackRange_;
    Time playbackFps_, playbackDuration_;
    bool previewWorkArea_ = false;
    QTimer playbackTimer_, recoveryTimer_, uTimer_;
    bool uPending_ = false;
    bool preparingPlayback_ = false, importing_ = false, advancingPlayback_ = false;
    std::uint64_t projectEpoch_ = 0, importTicket_ = 0, playbackTicket_ = 0, audioRevision_ = 0;
    QString dragToken_;
    QSlider *sourceSlider_ = nullptr;
    std::shared_ptr<AudioMix> audioMix_;
    std::unique_ptr<AudioPlayback> audioPlayback_;
    Viewer *viewer_ = nullptr;
    Timeline *timeline_ = nullptr;
    Graph *graph_ = nullptr;
    QTreeWidget *projectTree_ = nullptr;
    QLineEdit *name_ = nullptr;
    PropertyPanel *propertyPanel_ = nullptr, *effectPanel_ = nullptr;
    QLineEdit *projectSearch_ = nullptr, *timelineSearch_ = nullptr;
    QTabBar *viewerTabs_ = nullptr;
    QStackedWidget *graphStack_ = nullptr;
    QTreeWidget *queueTree_ = nullptr;
    QTreeWidgetItem *queueItem_ = nullptr;
    QProgressBar *queueProgress_ = nullptr;
    int viewerMode_ = 1;
    Id footage_ = 0;
    std::vector<PropertyRef> graphProperties_;
    QComboBox *parent_ = nullptr, *previewScale_ = nullptr, *graphPropertyBox_ = nullptr;
    QComboBox *viewerChannel_ = nullptr, *renderMotionBlurOverride_ = nullptr;
    QComboBox *previewRange_ = nullptr, *renderTimeSpan_ = nullptr;
    QCheckBox *visible_ = nullptr, *locked_ = nullptr, *viewerGrayscale_ = nullptr;
    QDoubleSpinBox *in_ = nullptr, *out_ = nullptr, *start_ = nullptr, *frameInput_ = nullptr;
    QDoubleSpinBox *viewerExposure_ = nullptr;
    DisplayOptions displayOptions_;
    QToolButton *motionBlurToggle_ = nullptr;
    QLabel *selectionLabel_ = nullptr, *frameLabel_ = nullptr, *notice_ = nullptr, *renderLabel_ = nullptr;
    QPushButton *play_ = nullptr, *cancelExport_ = nullptr;
    QTabWidget *timelineTabs_ = nullptr;
    std::jthread importThread_, playbackThread_;
};
} // namespace motion
