// SPDX-License-Identifier: MPL-2.0
#include "main_window.hpp"
#include "media.hpp"
#include "media_tree.hpp"
#include "numeric_control.hpp"
#include "property_panel.hpp"
#include <QAbstractSpinBox>
#include <QActionGroup>
#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorSpace>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <utility>
namespace motion {
namespace {
QPixmap motionBlurGlyphPixmap(bool enabled) {
    QPixmap pixmap(20, 18);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    const QColor color(enabled ? "#66b7f2" : "#707070");
    std::array<QRectF, 3> rings;
    for (int i = 0; i < int(rings.size()); ++i)
        rings[i] = {qreal(1 + i * 5), 4.5, 9, 9};
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(enabled ? QBrush(QColor("#234b67")) : QBrush(Qt::NoBrush));
    for (const auto &ring : rings)
        p.drawEllipse(ring);
    p.setPen(QPen(color, 1.25));
    p.setBrush(QBrush(Qt::NoBrush));
    for (const auto &ring : rings)
        p.drawEllipse(ring);
    return pixmap;
}
} // namespace
void applyApplicationTheme(QApplication &app) {
    app.setStyle("Fusion");
    QFont font = app.font();
    font.setFamily("Arial");
    font.setPixelSize(12);
    app.setFont(font);
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#1d1d1d"));
    palette.setColor(QPalette::WindowText, QColor("#c9c9c9"));
    palette.setColor(QPalette::Base, QColor("#1d1d1d"));
    palette.setColor(QPalette::AlternateBase, QColor("#272727"));
    palette.setColor(QPalette::Text, QColor("#c9c9c9"));
    palette.setColor(QPalette::Button, QColor("#303030"));
    palette.setColor(QPalette::ButtonText, QColor("#c9c9c9"));
    palette.setColor(QPalette::Highlight, QColor("#3c5065"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#777777"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#777777"));
    app.setPalette(palette);
    app.setStyleSheet(
        "QMainWindow::separator{width:4px;height:4px;background:#111111;} QDockWidget::title{padding:4px "
        "7px;background:#292929;border-bottom:1px solid #151515;}"
        "QToolBar{spacing:2px;padding:2px;border-bottom:1px solid #111111;} QToolButton{padding:2px "
        "6px;border:0;} QToolButton:hover{background:#414141;}"
        "QPushButton{padding:2px 5px;min-height:16px;border:1px solid "
        "#474747;border-radius:2px;background:#303030;} QPushButton:hover{background:#404040;}"
        "QLineEdit,QDoubleSpinBox,QComboBox{padding:1px 3px;min-height:18px;border:1px solid "
        "#383838;background:#1d1d1d;selection-background-color:#426584;}"
        "QLineEdit:focus,QDoubleSpinBox:focus,QTextEdit:focus{border:1px solid #709fd0;} "
        "QHeaderView::section{padding:2px 4px;background:#292929;border:0;border-right:1px solid #171717;}"
        "QTabBar::tab{padding:4px 9px;background:#1d1d1d;border:0;color:#a7a7a7;} "
        "QTabBar::tab:selected{color:#eeeeee;border-bottom:1px solid #b1b1b1;}"
        "QTabWidget::pane{border:0;} QTreeView{outline:0;} QTreeView::item{height:17px;} "
        "QScrollBar:vertical{width:10px;background:#1d1d1d;} "
        "QScrollBar:horizontal{height:10px;background:#1d1d1d;}"
        "QScrollBar::handle{background:#484848;min-height:22px;min-width:22px;} "
        "QScrollBar::add-line,QScrollBar::sub-line{width:0;height:0;} "
        "QStatusBar{padding:0px;font-size:11px;}");
}

class Viewer final : public QWidget {
  public:
    QImage image;
    QString message = "Create a composition and add a layer";
    QSize compSize{1920, 1080};
    std::vector<QPolygonF> outlines;
    std::function<void(QPointF, bool)> pressed;
    std::function<void(QPointF)> moved;
    std::function<void(bool)> released;
    QPointF origin;
    bool dragging = false, hand = false, panning = false, interactive = true, checker = false;
    double zoom = 0;
    QPointF offset, panStart, panBefore;
    explicit Viewer(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(360, 230);
        setFocusPolicy(Qt::StrongFocus);
    }
    QRectF canvas() const {
        QSizeF size = compSize;
        if (zoom > 0)
            size *= zoom / devicePixelRatioF();
        else
            size.scale(QSizeF(std::max(1, width() - 24), std::max(1, height() - 24)), Qt::KeepAspectRatio);
        return {(width() - size.width()) / 2 + offset.x(), (height() - size.height()) / 2 + offset.y(),
                size.width(), size.height()};
    }
    QPointF scene(QPointF point) const {
        auto r = canvas();
        return {(point.x() - r.left()) / r.width() * compSize.width(),
                (point.y() - r.top()) / r.height() * compSize.height()};
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#1d1d1d"));
        auto r = canvas();
        p.save();
        p.setClipRect(r);
        p.fillRect(r, Qt::black);
        if (checker)
            for (int y = int(r.top()); y < r.bottom(); y += 16)
                for (int x = int(r.left()); x < r.right(); x += 16)
                    p.fillRect(x, y, 16, 16,
                               QColor(((x - int(r.left())) / 16 + (y - int(r.top())) / 16) % 2 ? "#4c4c4c"
                                                                                               : "#333333"));
        if (!image.isNull()) {
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(r, image);
        }
        p.restore();
        p.setPen(QColor("#343434"));
        p.drawRect(r);
        if (image.isNull()) {
            p.setPen(QColor("#bdbdbd"));
            p.drawText(r.adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap, message);
        }
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor("#f1bd77"), 1));
        for (auto polygon : outlines) {
            for (auto &point : polygon)
                point = {r.left() + point.x() / compSize.width() * r.width(),
                         r.top() + point.y() / compSize.height() * r.height()};
            p.drawPolygon(polygon);
        }
        if (hasFocus()) {
            p.setPen(QColor("#278ddd"));
            p.drawRect(rect().adjusted(1, 1, -2, -2));
        }
    }
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton || !canvas().contains(e->position()))
            return;
        setFocus();
        if (hand) {
            panning = true;
            panStart = e->position();
            panBefore = offset;
            return;
        }
        if (!interactive)
            return;
        origin = scene(e->position());
        if (pressed)
            pressed(origin, e->modifiers() & Qt::ShiftModifier);
        dragging = true;
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (panning) {
            offset = panBefore + e->position() - panStart;
            update();
            return;
        }
        if (dragging && moved)
            moved(scene(e->position()) - origin);
    }
    void mouseReleaseEvent(QMouseEvent *) override {
        if (panning) {
            panning = false;
            return;
        }
        if (dragging && released)
            released(false);
        dragging = false;
    }
    void keyPressEvent(QKeyEvent *e) override {
        if (e->key() == Qt::Key_Escape && panning) {
            offset = panBefore;
            panning = false;
            update();
            return;
        }
        if (e->key() == Qt::Key_Escape && dragging) {
            if (released)
                released(true);
            dragging = false;
        } else
            QWidget::keyPressEvent(e);
    }
};
namespace {
QDoubleSpinBox *number(QWidget *parent, double min, double max, int decimals = 3) {
    auto *box = new QDoubleSpinBox(parent);
    box->setRange(min, max);
    box->setDecimals(decimals);
    box->setKeyboardTracking(false);
    box->setMinimumWidth(90);
    return box;
}
QIcon toolIcon(const QString &name) {
    QPixmap pix(20, 20);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor("#bcbcbc"), 1.4));
    if (name == "Selection") {
        p.setBrush(QColor("#c6c6c6"));
        p.drawPolygon(QPolygonF{{4, 2}, {4, 16}, {8, 12}, {11, 18}, {14, 16}, {10, 10}, {16, 10}});
    } else if (name == "Hand") {
        QPainterPath path;
        path.moveTo(6, 17);
        path.lineTo(3, 10);
        path.quadTo(3, 8, 5, 10);
        path.lineTo(7, 12);
        path.lineTo(6, 4);
        path.quadTo(7, 1, 8, 4);
        path.lineTo(9, 9);
        path.lineTo(9, 3);
        path.quadTo(11, 1, 11, 4);
        path.lineTo(12, 9);
        path.lineTo(12, 4);
        path.quadTo(14, 3, 14, 5);
        path.lineTo(14, 10);
        path.lineTo(16, 6);
        path.quadTo(18, 6, 17, 9);
        path.lineTo(15, 17);
        path.closeSubpath();
        p.drawPath(path);
    } else if (name == "T") {
        QFont f("Arial");
        f.setPixelSize(17);
        p.setFont(f);
        p.drawText(QRect(0, 0, 20, 20), Qt::AlignCenter, "T");
    } else if (name == "Solid")
        p.drawRect(QRectF(3, 4, 14, 12));
    else if (name == "Null") {
        p.drawRect(QRectF(5, 5, 10, 10));
        p.drawLine(10, 1, 10, 19);
        p.drawLine(1, 10, 19, 10);
    } else if (name == "Graph") {
        p.drawLine(3, 17, 18, 17);
        p.drawLine(3, 17, 3, 2);
        QPainterPath path;
        path.moveTo(3, 15);
        path.cubicTo(8, 15, 9, 4, 17, 4);
        p.drawPath(path);
    } else {
        QPainterPath path;
        path.moveTo(2, 5);
        path.lineTo(8, 5);
        path.lineTo(10, 7);
        path.lineTo(18, 7);
        path.lineTo(16, 16);
        path.lineTo(2, 16);
        path.closeSubpath();
        p.drawPath(path);
    }
    return QIcon(pix);
}
QString idString(Id id) { return QString::number(id); }
} // namespace
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), editor_({}, this), worker_(this) {
    identity_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    dragToken_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    baseDirectory_ = QDir::currentPath();
    buildUi();
    auto trackNumericEdit = [this](QDoubleSpinBox *box) {
        connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, box] {
            if (!refreshing_)
                box->setProperty("userEdited", true);
        });
    };
    for (auto *box : {in_, out_, start_})
        trackNumericEdit(box);
    qApp->installEventFilter(this);
    connect(&editor_, &Editor::changed, this, &MainWindow::refresh);
    connect(&editor_, &Editor::documentReplaced, this, [this] {
        ++projectEpoch_;
        ++importTicket_;
        importThread_.request_stop();
        importing_ = false;
        stopPlayback();
        viewerMode_ = 1;
        footage_ = 0;
        footageTime_ = time(0);
        sourceSlider_->hide();
        {
            QSignalBlocker block(viewerTabs_);
            viewerTabs_->setCurrentIndex(1);
            viewerTabs_->setTabText(0, "Layer (none)");
            viewerTabs_->setTabText(2, "Footage (none)");
        }
        viewer_->interactive = true;
        dragToken_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        projectTree_->setProperty("dragToken", dragToken_);
    });
    connect(&editor_, &Editor::selectionChanged, this, [this] {
        syncProjectSelection();
        refreshProperties();
        if (viewerMode_ == 0)
            requestFrame();
        viewer_->update();
    });
    connect(&worker_, &RenderWorker::previewReady, this, [this](PreviewResult result) {
        if (!matches(result, active_))
            return;
        inFlight_ = false;
        if (!result.error.isEmpty()) {
            displayedImage_ = {};
            viewer_->image = {};
            viewer_->message = result.error;
            if (playing_ || preparingPlayback_)
                stopPlayback();
            report(result.error);
        } else {
            displayedImage_ = result.image;
            viewer_->image = result.image;
            viewer_->message.clear();
            const auto fps = composition(active_.project, active_.comp).fps;
            const auto frame = std::llround(seconds(result.at) * seconds(fps));
            const auto nominal = std::max<long long>(1, std::llround(seconds(fps)));
            frameLabel_->setText(frameText(frame, nominal));
            frameLabel_->setToolTip(QString("Displayed frame %1 · non-drop · exact time %2/%3 s")
                                        .arg(frame)
                                        .arg(result.at.numerator)
                                        .arg(result.at.denominator));
            renderLabel_->setText("CPU · " + QString::number(result.milliseconds, 'f', 1) + " ms");
            emit frameDisplayed();
        }
        viewer_->update();
        const Time viewTime = viewerMode_ == 2
                                  ? (footage_ && asset(editor_.project(), footage_).kind != AssetKind::Image
                                         ? footageTime_
                                         : time(0))
                                  : now_;
        if (result.error.isEmpty() && viewTime != result.at)
            requestFrame();
    });
    connect(&worker_, &RenderWorker::exportProgress, this, [this](qint64 done, qint64 total) {
        notice_->setText(QString("Exporting  %1 / %2 frames").arg(done).arg(total));
        queueProgress_->setValue(total ? int(done * 100 / total) : 0);
        if (queueItem_)
            queueItem_->setText(2, QString::number(done) + " / " + QString::number(total));
    });
    connect(&worker_, &RenderWorker::exportFinished, this, [this](ExportResult result) {
        exporting_ = false;
        findChild<QPushButton *>("queueCancel")->setEnabled(false);
        findChild<QPushButton *>("queueRender")->setEnabled(true);
        if (queueItem_)
            queueItem_->setText(1, result.state);
        menuBar()->setEnabled(true);
        for (auto *toolbar : findChildren<QToolBar *>())
            toolbar->setEnabled(true);
        projectTree_->setEnabled(true);
        timelineTabs_->setEnabled(true);
        cancelExport_->hide();
        viewerTabs_->setEnabled(true);
        sourceSlider_->setEnabled(true);
        refreshProperties();
        if (result.state == "complete") {
            report(QString("Export complete: %1 frames · %2").arg(result.completed).arg(result.directory));
        } else
            report(QString("Export %1 after %2 frames. %3")
                       .arg(result.state)
                       .arg(result.completed)
                       .arg(result.error));
        emit outputFinished(result);
        requestFrame();
    });
    connect(&playbackTimer_, &QTimer::timeout, this, &MainWindow::advancePlayback);
    playbackTimer_.setInterval(16);
    recoveryTimer_.setInterval(60000);
    connect(&recoveryTimer_, &QTimer::timeout, this, &MainWindow::saveRecoveryNow);
    recoveryTimer_.start();
    uTimer_.setSingleShot(true);
    connect(&uTimer_, &QTimer::timeout, this, [this] {
        uPending_ = false;
        setFilter("U");
    });
    QSettings settings;
    resize(1506, 989); // 1506 x 1020 including the native title bar in the captured workspace.
    restoreGeometry(settings.value("windowGeometry-ae-reference-v1").toByteArray());
    resetWorkspace();
    if (!restoreState(settings.value("workspace-ae-reference-v1").toByteArray()))
        QTimer::singleShot(0, this, [this] { resetWorkspace(); });
    refresh();
}
void MainWindow::guarded(std::function<void()> action) {
    try {
        action();
    } catch (const std::exception &e) {
        report(QString::fromUtf8(e.what()));
    }
}
void MainWindow::report(const QString &message) {
    lastError_ = message;
    notice_->setText(message);
    notice_->setToolTip(message);
}
void MainWindow::appendLog(QString text) {
    actionLog_ += QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) + " " + text + "\n";
}
void MainWindow::buildUi() {
    setWindowTitle("Before Effects — native compositor");
    setDockNestingEnabled(true);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
    setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
    auto *central = new QWidget;
    auto *centerLayout = new QVBoxLayout(central);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    viewerTabs_ = new QTabBar;
    viewerTabs_->setObjectName("sourceViewTabs");
    viewerTabs_->setExpanding(false);
    viewerTabs_->setDrawBase(false);
    viewerTabs_->addTab("Layer (none)");
    viewerTabs_->addTab("Composition");
    viewerTabs_->addTab("Footage (none)");
    viewerTabs_->setCurrentIndex(1);
    centerLayout->addWidget(viewerTabs_);
    auto *breadcrumb = new QLabel("Composition");
    breadcrumb->setObjectName("compositionBreadcrumb");
    breadcrumb->setContentsMargins(8, 3, 0, 3);
    centerLayout->addWidget(breadcrumb);
    viewer_ = new Viewer;
    viewer_->setObjectName("compositionViewer");
    centerLayout->addWidget(viewer_, 1);
    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(5, 0, 5, 0);
    footer->setSpacing(5);
    auto *zoom = new QComboBox;
    zoom->setObjectName("viewerZoom");
    zoom->addItems({"Fit", "25%", "50%", "100%", "200%"});
    zoom->setMaximumWidth(80);
    footer->addWidget(zoom);
    connect(zoom, qOverload<int>(&QComboBox::activated), this, [this](int index) {
        static const double scales[] = {0, .25, .5, 1, 2};
        viewer_->zoom = scales[index];
        viewer_->offset = {};
        viewer_->update();
    });
    previewScale_ = new QComboBox;
    previewScale_->setObjectName("viewerPreviewScale");
    previewScale_->addItems({"Half", "Full"});
    previewScale_->setMaximumWidth(85);
    footer->addWidget(previewScale_);
    connect(previewScale_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { requestFrame(); });
    viewerChannel_ = new QComboBox;
    viewerChannel_->setObjectName("viewerChannel");
    for (const auto &[label, channel] :
         std::array<std::pair<const char *, DisplayChannel>, 6>{{
             {"RGB", DisplayChannel::RGB}, {"Red", DisplayChannel::Red}, {"Green", DisplayChannel::Green},
             {"Blue", DisplayChannel::Blue}, {"Alpha", DisplayChannel::Alpha},
             {"Luminance", DisplayChannel::Luminance}}})
        viewerChannel_->addItem(label, int(channel));
    viewerChannel_->setToolTip("View-only channel selection");
    viewerChannel_->setMaximumWidth(90);
    footer->addWidget(viewerChannel_);
    viewerGrayscale_ = new QCheckBox("Gray");
    viewerGrayscale_->setObjectName("viewerGrayscale");
    viewerGrayscale_->setChecked(displayOptions_.grayscale);
    viewerGrayscale_->setEnabled(false);
    viewerGrayscale_->setToolTip("Show a selected color channel as grayscale instead of channel color");
    footer->addWidget(viewerGrayscale_);
    viewerExposure_ = new QDoubleSpinBox;
    viewerExposure_->setObjectName("viewerExposure");
    viewerExposure_->setRange(-20, 20);
    viewerExposure_->setDecimals(2);
    viewerExposure_->setSingleStep(.25);
    viewerExposure_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    viewerExposure_->setSuffix(" st");
    viewerExposure_->setMaximumWidth(82);
    viewerExposure_->setToolTip("View-only exposure in stops; disabled for Alpha");
    footer->addWidget(viewerExposure_);
    auto updateDisplay = [this] {
        displayOptions_.channel = static_cast<DisplayChannel>(viewerChannel_->currentData().toInt());
        displayOptions_.grayscale = viewerGrayscale_->isChecked();
        displayOptions_.exposureStops = viewerExposure_->value();
        const bool audioWaveform = viewerMode_ == 2 && footage_ &&
                                   asset(editor_.project(), footage_).kind == AssetKind::Audio;
        const bool colorChannel = displayOptions_.channel == DisplayChannel::Red ||
                                  displayOptions_.channel == DisplayChannel::Green ||
                                  displayOptions_.channel == DisplayChannel::Blue;
        viewerChannel_->setEnabled(!audioWaveform);
        viewerExposure_->setEnabled(!audioWaveform && displayOptions_.channel != DisplayChannel::Alpha);
        viewerGrayscale_->setEnabled(!audioWaveform && colorChannel);
        requestFrame();
    };
    connect(viewerChannel_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [updateDisplay](int) { updateDisplay(); });
    connect(viewerGrayscale_, &QCheckBox::toggled, this, [updateDisplay](bool) { updateDisplay(); });
    connect(viewerExposure_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [updateDisplay](double) { updateDisplay(); });
    auto *grid = new QToolButton;
    grid->setText("▦");
    grid->setToolTip("Toggle transparency grid");
    grid->setAccessibleName("Transparency grid");
    grid->setCheckable(true);
    footer->addWidget(grid);
    connect(grid, &QToolButton::toggled, this, [this](bool on) {
        viewer_->checker = on;
        viewer_->update();
    });
    frameLabel_ = new QLabel("0:00:00:00");
    footer->addWidget(frameLabel_);
    sourceSlider_ = new QSlider(Qt::Horizontal);
    sourceSlider_->setObjectName("sourceTimeSlider");
    sourceSlider_->setMaximumWidth(260);
    sourceSlider_->hide();
    footer->addWidget(sourceSlider_);
    connect(sourceSlider_, &QSlider::sliderMoved, this, [this](int frame) {
        if (viewerMode_ != 2 || !footage_)
            return;
        stopPlayback();
        const auto p = footageProject();
        footageTime_ = frameTime(frame, p.compositions.front().fps);
        requestFrame();
    });
    footer->addStretch();
    centerLayout->addLayout(footer);
    setCentralWidget(central);
    connect(viewerTabs_, &QTabBar::currentChanged, this, &MainWindow::setViewerMode);
    auto dock = [&](QString title, QString id, QWidget *widget, Qt::DockWidgetArea area) {
        auto *d = new QDockWidget(title, this);
        d->setObjectName(id);
        d->setWidget(widget);
        addDockWidget(area, d);
        return d;
    };
    auto *mediaTree = new MediaTree;
    projectTree_ = mediaTree;
    mediaTree->importFile = [this](QString path) { importMediaFile(path); };
    projectTree_->setProperty("dragToken", dragToken_);
    projectTree_->setObjectName("projectTree");
    projectTree_->setHeaderLabels({"Name", "Type", "Size", "Rate"});
    projectTree_->setIndentation(12);
    projectTree_->setFrameShape(QFrame::NoFrame);
    projectTree_->setColumnWidth(0, 150);
    projectTree_->setColumnWidth(1, 80);
    projectTree_->setColumnWidth(2, 65);
    projectTree_->setColumnWidth(3, 45);
    projectTree_->setMinimumWidth(170);
    auto *projectPanel = new QWidget;
    auto *projectLayout = new QVBoxLayout(projectPanel);
    projectLayout->setContentsMargins(5, 2, 5, 2);
    projectLayout->setSpacing(3);
    auto *projectInfo = new QLabel("Project");
    projectInfo->setObjectName("projectInfo");
    projectInfo->setMinimumHeight(105);
    projectInfo->setAlignment(Qt::AlignTop);
    projectLayout->addWidget(projectInfo);
    projectSearch_ = new QLineEdit;
    projectSearch_->setObjectName("projectSearch");
    projectSearch_->setClearButtonEnabled(true);
    projectSearch_->setPlaceholderText("Search");
    projectLayout->addWidget(projectSearch_);
    projectLayout->addWidget(projectTree_, 1);
    auto *projectDock = dock("Project", "projectDock", projectPanel, Qt::LeftDockWidgetArea);
    effectPanel_ = new PropertyPanel(&editor_, nullptr, PropertyPanel::Section::Effects);
    auto *effectDock = dock("Effect Controls", "effectControlsDock", effectPanel_, Qt::LeftDockWidgetArea);
    tabifyDockWidget(effectDock, projectDock);
    projectDock->raise();
    connect(effectPanel_, &PropertyPanel::error, this, &MainWindow::report);
    connect(effectPanel_, &PropertyPanel::propertySelected, this, &MainWindow::selectGraphProperty);
    connect(effectPanel_, &PropertyPanel::timeChanged, this, &MainWindow::setTime);
    connect(projectSearch_, &QLineEdit::textChanged, this, &MainWindow::filterProject);
    projectTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(projectTree_, &QWidget::customContextMenuRequested, this, [this](QPoint point) {
        auto *item = projectTree_->itemAt(point);
        if (!item || item->data(0, Qt::UserRole + 1) != "asset")
            return;
        Id id = item->data(0, Qt::UserRole).toULongLong();
        QMenu menu;
        auto *create = menu.addAction("New composition from footage");
        auto *add = menu.addAction("Add to current composition");
        auto *picked = menu.exec(projectTree_->viewport()->mapToGlobal(point));
        if (picked == create)
            createCompositionFromAsset(id);
        else if (picked == add)
            addAssetLayer(id, now_);
    });
    connect(projectTree_, &QTreeWidget::currentItemChanged, this, [this, projectInfo](QTreeWidgetItem *item) {
        if (!item || refreshing_)
            return;
        projectInfo->setText(item->text(0) + "\n" + item->text(1) + "  " + item->text(2) + "  " +
                             item->text(3));
    });
    connect(projectTree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        if (!item || exporting_)
            return;
        guarded([&] {
            auto kind = item->data(0, Qt::UserRole + 1).toString();
            Id id = item->data(0, Qt::UserRole).toULongLong();
            if (kind == "comp") {
                chooseComposition(id);
                setViewerMode(1);
            } else if (kind == "asset") {
                if (footage_ != id)
                    footageTime_ = time(0);
                footage_ = id;
                setViewerMode(2);
            } else if (kind == "solid") {
                chooseComposition(item->data(0, Qt::UserRole + 2).toULongLong());
                editor_.select({id});
                setViewerMode(0);
            }
        });
    });
    auto *props = new QWidget;
    auto *form = new QFormLayout(props);
    form->setContentsMargins(5, 3, 5, 3);
    form->setVerticalSpacing(2);
    selectionLabel_ = new QLabel("No layer selected");
    form->addRow(selectionLabel_);
    name_ = new QLineEdit;
    name_->setObjectName("layerName");
    form->addRow("Name", name_);
    connect(name_, &QLineEdit::editingFinished, this, [this] {
        if (!refreshing_ && !editor_.selection().empty() && !exporting_)
            guarded([&] {
                Id id = editor_.selection().front();
                editor_.apply("Rename layer", [&](Project &p) {
                    if (layer(p, id).locked)
                        throw std::runtime_error("Unlock layer first");
                    layer(p, id).name = name_->text();
                });
            });
    });
    visible_ = new QCheckBox("Visible");
    locked_ = new QCheckBox("Locked");
    auto *flags = new QHBoxLayout;
    flags->addWidget(visible_);
    flags->addWidget(locked_);
    form->addRow(flags);
    connect(visible_, &QCheckBox::toggled, this, [this](bool on) {
        if (!refreshing_ && !exporting_)
            guarded([&] {
                editor_.apply("Visibility", [&](Project &p) {
                    for (Id id : editor_.selection()) {
                        if (layer(p, id).locked)
                            throw std::runtime_error("Unlock layer first");
                        layer(p, id).visible = on;
                    }
                });
            });
    });
    connect(locked_, &QCheckBox::toggled, this, [this](bool on) {
        if (!refreshing_ && !exporting_)
            guarded([&] {
                editor_.apply("Layer lock", [&](Project &p) {
                    for (Id id : editor_.selection())
                        layer(p, id).locked = on;
                });
            });
    });
    propertyPanel_ = new PropertyPanel(&editor_);
    form->addRow(propertyPanel_);
    connect(propertyPanel_, &PropertyPanel::error, this, &MainWindow::report);
    connect(propertyPanel_, &PropertyPanel::propertySelected, this, &MainWindow::selectGraphProperty);
    connect(propertyPanel_, &PropertyPanel::timeChanged, this, &MainWindow::setTime);
    in_ = number(props, -86400, 86400);
    out_ = number(props, -86400, 86400);
    start_ = number(props, -86400, 86400);
    in_->setObjectName("layerInTime");
    out_->setObjectName("layerOutTime");
    start_->setObjectName("layerStartTime");
    form->addRow("In (seconds)", in_);
    form->addRow("Out (seconds)", out_);
    form->addRow("Source start", start_);
    for (auto *box : {in_, out_, start_})
        connect(box, &QDoubleSpinBox::editingFinished, this, [this, box] {
            if (!refreshing_ && !exporting_ && box->property("userEdited").toBool()) {
                box->setProperty("userEdited", false);
                guarded([&] {
                    editor_.apply("Layer timing", [&](Project &p) {
                        for (Id id : editor_.selection()) {
                            auto &l = layer(p, id);
                            if (l.locked)
                                throw std::runtime_error("Unlock layer first");
                            if (box == in_)
                                l.in = fromSeconds(box->value());
                            else if (box == out_)
                                l.out = fromSeconds(box->value());
                            else
                                l.start = fromSeconds(box->value());
                        }
                    });
                });
            }
        });
    parent_ = new QComboBox;
    form->addRow("Parent", parent_);
    auto *setParent = new QPushButton("Assign parent");
    form->addRow(setParent);
    connect(setParent, &QPushButton::clicked, this, &MainWindow::parentSelection);
    for (auto *combo : props->findChildren<QComboBox *>()) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(12);
    }
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(props);
    scroll->setMinimumWidth(285);
    auto *propertiesDock = dock("Properties", "propertiesDock", scroll, Qt::RightDockWidgetArea);
    auto *effectsBrowser = new QWidget;
    auto *effectsLayout = new QVBoxLayout(effectsBrowser);
    effectsLayout->setContentsMargins(5, 2, 5, 2);
    effectsLayout->setSpacing(3);
    auto *effectSearch = new QLineEdit;
    effectSearch->setObjectName("effectsSearch");
    effectSearch->setPlaceholderText("Search");
    effectSearch->setClearButtonEnabled(true);
    effectsLayout->addWidget(effectSearch);
    auto *effectsList = new QTreeWidget;
    effectsList->setObjectName("effectsBrowser");
    effectsList->setHeaderHidden(true);
    effectsList->setIndentation(10);
    effectsList->setFrameShape(QFrame::NoFrame);
    auto *presets = new QTreeWidgetItem(effectsList, {"Presets"});
    auto *bright = new QTreeWidgetItem(presets, {"Brighten"});
    auto *dim = new QTreeWidgetItem(presets, {"Dim"});
    bright->setData(0, Qt::UserRole + 1, "brighten");
    dim->setData(0, Qt::UserRole + 1, "dim");
    std::map<QString, QTreeWidgetItem *> effectGroups;
    for (const auto &spec : effectSpecs()) {
        auto &group = effectGroups[spec.category];
        if (!group) {
            group = new QTreeWidgetItem(effectsList, {spec.category});
            group->setExpanded(true);
        }
        auto *item = new QTreeWidgetItem(group, {spec.name});
        item->setData(0, Qt::UserRole, spec.type);
    }
    presets->setExpanded(true);
    effectsLayout->addWidget(effectsList, 1);
    connect(effectSearch, &QLineEdit::textChanged, this, [effectsList](const QString &query) {
        for (int i = 0; i < effectsList->topLevelItemCount(); ++i) {
            auto *group = effectsList->topLevelItem(i);
            bool any = false;
            for (int j = 0; j < group->childCount(); ++j) {
                auto *item = group->child(j);
                bool show = query.isEmpty() || item->text(0).contains(query, Qt::CaseInsensitive) ||
                            group->text(0).contains(query, Qt::CaseInsensitive);
                item->setHidden(!show);
                any |= show;
            }
            group->setHidden(!any);
        }
    });
    connect(effectsList, &QTreeWidget::itemActivated, this, [=, this](QTreeWidgetItem *item) {
        if (exporting_ || editor_.selection().empty())
            return;
        const auto preset = item->data(0, Qt::UserRole + 1).toString();
        if (preset == "brighten" || preset == "dim")
            guarded([&] {
                editor_.apply("Apply color preset", [&](Project &p) {
                    for (Id id : editor_.selection()) {
                        Id effect = addEffect(p, id);
                        layer(p, id).channel({"color.gain", 0, effect}).base = preset == "brighten" ? 1.2 : .8;
                    }
                });
            });
        else {
            const auto type = item->data(0, Qt::UserRole).toString();
            if (type.isEmpty())
                return;
            effectPanel_->addEffect(type);
        }
        findChild<QDockWidget *>("effectControlsDock")->raise();
    });
    auto *effectsDock =
        dock("Effects & Presets", "effectsBrowserDock", effectsBrowser, Qt::RightDockWidgetArea);
    auto *auxTabs = new QTabWidget;
    auxTabs->setObjectName("auxiliaryTabs");
    auto *libraries = new QWidget;
    auto *libraryLayout = new QVBoxLayout(libraries);
    auto *libraryNote = new QLabel("Cloud libraries are not available in this build.");
    libraryNote->setWordWrap(true);
    libraryNote->setStyleSheet("color:#969696;");
    libraryLayout->addWidget(libraryNote);
    libraryLayout->addStretch();
    auxTabs->addTab(libraries, "Libraries");
    auto *align = new QWidget;
    auto *alignLayout = new QVBoxLayout(align);
    alignLayout->setContentsMargins(5, 5, 5, 5);
    alignLayout->addWidget(new QLabel("Align layer bounds to composition"));
    for (int axis = 0; axis < 2; ++axis) {
        auto *row = new QHBoxLayout;
        for (int edge = 0; edge < 3; ++edge) {
            QString name = axis == 0 ? QStringList{"Left", "Center", "Right"}[edge]
                                     : QStringList{"Top", "Middle", "Bottom"}[edge];
            auto *button = new QPushButton(name);
            button->setObjectName("align." + QString::number(axis) + "." + QString::number(edge));
            connect(button, &QPushButton::clicked, this, [this, axis, edge] { alignSelection(axis, edge); });
            row->addWidget(button);
        }
        alignLayout->addLayout(row);
    }
    alignLayout->addStretch();
    auxTabs->addTab(align, "Align");
    auto *aux = dock("Panels", "auxiliaryDock", auxTabs, Qt::RightDockWidgetArea);
    aux->setTitleBarWidget(new QWidget(aux));
    splitDockWidget(effectsDock, aux, Qt::Vertical);
    propertiesDock->hide();

    auto *lower = new QWidget;
    auto *lowerLayout = new QVBoxLayout(lower);
    lowerLayout->setContentsMargins(0, 0, 0, 0);
    auto *transport = new QHBoxLayout;
    play_ = new QPushButton("Play");
    connect(play_, &QPushButton::clicked, this, &MainWindow::togglePlayback);
    frameInput_ = new FrameNumber(lower);
    frameInput_->setFixedWidth(116);
    frameInput_->setStyleSheet(
        "QDoubleSpinBox{color:#43a5f5;font-size:14px;font-weight:bold;border:0;min-height:24px;}");
    frameInput_->setToolTip("Current time · non-drop. Enter h:mm:ss:ff or a frame number.");
    transport->setContentsMargins(5, 0, 5, 0);
    transport->setSpacing(6);
    frameInput_->setObjectName("currentFrame");
    transport->addWidget(frameInput_);
    connect(frameInput_, &QDoubleSpinBox::editingFinished, this, [this] {
        if (!refreshing_)
            guarded([&] {
                setTime(
                    frameTime(std::llround(frameInput_->value()), composition(editor_.project(), comp_).fps));
            });
    });
    timelineSearch_ = new QLineEdit;
    timelineSearch_->setObjectName("timelineSearch");
    timelineSearch_->setPlaceholderText("Search layers and properties");
    timelineSearch_->setClearButtonEnabled(true);
    timelineSearch_->setFixedWidth(270);
    transport->addWidget(timelineSearch_);
    transport->addWidget(play_);
    previewRange_ = new QComboBox;
    previewRange_->setObjectName("previewTimeSpan");
    previewRange_->setAccessibleName("Preview range");
    previewRange_->setToolTip("Composition preview range; footage uses its full source duration");
    previewRange_->addItem("Full composition", false);
    previewRange_->addItem("Work Area", true);
    transport->addWidget(previewRange_);
    connect(previewRange_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        const bool restart = playing_ || preparingPlayback_;
        stopPlayback();
        previewWorkArea_ = previewRange_->currentData().toBool();
        if (restart)
            beginPlayback();
    });
    motionBlurToggle_ = new QToolButton;
    motionBlurToggle_->setObjectName("compositionMotionBlurToggle");
    QIcon motionBlurIcon;
    motionBlurIcon.addPixmap(motionBlurGlyphPixmap(false), QIcon::Normal, QIcon::Off);
    motionBlurIcon.addPixmap(motionBlurGlyphPixmap(true), QIcon::Normal, QIcon::On);
    motionBlurToggle_->setIcon(motionBlurIcon);
    motionBlurToggle_->setIconSize(QSize(20, 18));
    motionBlurToggle_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    motionBlurToggle_->setCheckable(true);
    motionBlurToggle_->setStyleSheet(
        "QToolButton:checked{background:#334e62;border:1px solid #66b7f2;border-radius:2px;}");
    motionBlurToggle_->setToolTip(
        "Composition motion blur · transform-only; the root composition owns the shutter when nested");
    motionBlurToggle_->setAccessibleName("Composition motion blur");
    transport->addWidget(motionBlurToggle_);
    connect(motionBlurToggle_, &QToolButton::toggled, this, [this](bool enabled) {
        if (refreshing_ || exporting_)
            return;
        guarded([&] {
            editor_.apply("Composition motion blur",
                          [&](Project &p) { composition(p, comp_).motionBlurEnabled = enabled; });
        });
    });
    transport->addStretch();

    graphPropertyBox_ = new QComboBox;
    graphPropertyBox_->setObjectName("graphProperty");
    graphPropertyBox_->setMaximumWidth(220);

    auto *graphType = new QComboBox;
    graphType->setObjectName("graphType");
    graphType->addItems({"Value Graph", "Speed Graph"});
    graphType->setMaximumWidth(140);
    graphType->hide();
    transport->addWidget(graphType);
    auto *velocityButton = new QPushButton("Velocity…");
    velocityButton->setObjectName("keyframeVelocityButton");
    velocityButton->hide();
    transport->addWidget(velocityButton);
    transport->addWidget(graphPropertyBox_);
    auto *addKeyButton = new QPushButton("Add key");
    transport->addWidget(addKeyButton);
    connect(addKeyButton, &QPushButton::clicked, this, [this] { addKey(graphProperty_); });
    auto *timelinePage = new QWidget;
    auto *timelinePageLayout = new QVBoxLayout(timelinePage);
    timelinePageLayout->setContentsMargins(0, 0, 0, 0);
    timelinePageLayout->setSpacing(0);
    timelinePageLayout->addLayout(transport);
    timelineTabs_ = new QTabWidget;
    timelineTabs_->setObjectName("timelineTabs");
    timeline_ = new Timeline(&editor_);
    connect(timelineSearch_, &QLineEdit::textChanged, timeline_, &Timeline::setSearch);
    connect(timeline_, &Timeline::layerOpened, this, [this](Id) { setViewerMode(0); });
    connect(timeline_, &Timeline::fileDropped, this,
            [this](QString path, Time at) { importMediaFile(path, true, at); });
    connect(timeline_, &Timeline::itemDropped, this, [this](Id id, bool comp, QString token, Time at) {
        if (token != dragToken_) {
            report("Use File Import for media from another project window");
            return;
        }
        if (!comp) {
            addAssetLayer(id, at);
            return;
        }
        guarded([&] {
            Id layerId = 0;
            editor_.apply("Add precomposition", [&](Project &p) {
                const auto &source = composition(p, id);
                auto &target = composition(p, comp_);
                Layer l;
                l.id = p.nextId++;
                layerId = l.id;
                l.kind = LayerKind::Precomp;
                l.source = id;
                l.name = source.name;
                l.width = source.width;
                l.height = source.height;
                l.start = l.in = at;
                l.out = std::min(target.duration, at + source.duration);
                target.layers.insert(target.layers.begin(), l);
            });
            editor_.select({layerId});
        });
    });
    auto *timelineScroll = new QScrollArea;
    timelineScroll->setWidgetResizable(true);
    timelineScroll->setWidget(timeline_);
    graph_ = new Graph(&editor_);
    graph_->setObjectName("graphEditor");
    connect(graphType, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int i) { graph_->setSpeedGraph(i == 1); });
    connect(velocityButton, &QPushButton::clicked, graph_, &Graph::editVelocity);
    connect(graph_, &Graph::keySelected, timeline_, &Timeline::selectKey);
    connect(timeline_, &Timeline::keySelected, this, [this](KeyRef key) {
        selectGraphProperty(key.property);
        graph_->selectKey(key);
    });
    graphStack_ = new QStackedWidget;
    graphStack_->addWidget(timelineScroll);
    graphStack_->addWidget(graph_);
    timelinePageLayout->addWidget(graphStack_, 1);
    timelineTabs_->addTab(timelinePage, "Composition");
    auto *graphToggle = new QToolButton;
    graphToggle->setText("Graph");
    graphToggle->setObjectName("graphToggle");
    graphToggle->setToolTip("Graph Editor · Shift+F3");
    graphToggle->setCheckable(true);
    transport->insertWidget(3, graphToggle);
    graphPropertyBox_->hide();
    addKeyButton->hide();
    connect(graphToggle, &QToolButton::toggled, this, [=, this](bool on) {
        // Cocoa can focus an inspector control while the previous page is hidden.
        const auto property = graphProperty_;
        const auto selectedKey = graph_->keySelection();
        graphStack_->setCurrentIndex(on ? 1 : 0);
        graphPropertyBox_->setVisible(on);
        addKeyButton->setVisible(on);
        graphType->setVisible(on);
        velocityButton->setVisible(on);
        if (on)
            graph_->setFocus();
        else
            timeline_->setFocus();
        selectGraphProperty(property);
        if (selectedKey)
            graph_->selectKey(*selectedKey);
    });
    lowerLayout->addWidget(timelineTabs_);
    lower->setMinimumHeight(280);
    auto *timelineDock = dock("Timeline", "timelineDock", lower, Qt::BottomDockWidgetArea);
    timelineDock->setTitleBarWidget(new QWidget(timelineDock));
    auto *queue = new QWidget;
    auto *queueLayout = new QVBoxLayout(queue);
    queueLayout->setContentsMargins(6, 3, 6, 3);
    queueLayout->setSpacing(4);
    auto *queueBar = new QHBoxLayout;
    queueBar->addWidget(new QLabel("Current Render"));
    queueProgress_ = new QProgressBar;
    queueProgress_->setObjectName("renderQueueProgress");
    queueProgress_->setRange(0, 100);
    queueProgress_->setValue(0);
    queueBar->addWidget(queueProgress_, 1);
    auto *format = new QComboBox;
    format->setObjectName("renderFormat");
    format->addItems({"PNG · RGBA · no audio", "MP4 · H.264 + AAC · opaque"});
    queueBar->addWidget(format);
    renderTimeSpan_ = new QComboBox;
    renderTimeSpan_->setObjectName("renderTimeSpan");
    renderTimeSpan_->setAccessibleName("Render time span");
    renderTimeSpan_->setToolTip("Time span captured when a render is submitted; stills use the current frame");
    renderTimeSpan_->addItem("Full composition", false);
    renderTimeSpan_->addItem("Work Area", true);
    queueBar->addWidget(renderTimeSpan_);
    renderMotionBlurOverride_ = new QComboBox;
    renderMotionBlurOverride_->setObjectName("renderMotionBlurOverride");
    renderMotionBlurOverride_->addItem("Current settings", int(MotionBlurOverride::CurrentSettings));
    renderMotionBlurOverride_->addItem("Off", int(MotionBlurOverride::Off));
    renderMotionBlurOverride_->addItem("On checked layers", int(MotionBlurOverride::OnForCheckedLayers));
    renderMotionBlurOverride_->setToolTip("Motion-blur render override; this setting is captured at render start");
    queueBar->addWidget(renderMotionBlurOverride_);
    auto *render = new QPushButton("Render composition");
    render->setObjectName("queueRender");
    queueBar->addWidget(render);
    auto *cancel = new QPushButton("Stop");
    cancel->setObjectName("queueCancel");
    cancel->setEnabled(false);
    queueBar->addWidget(cancel);
    queueLayout->addLayout(queueBar);
    queueTree_ = new QTreeWidget;
    queueTree_->setObjectName("renderQueue");
    queueTree_->setHeaderLabels({"Composition", "Status", "Frames", "Output module", "Output to"});
    queueTree_->setRootIsDecorated(false);
    queueTree_->setColumnWidth(0, 220);
    queueTree_->setColumnWidth(1, 110);
    queueTree_->setColumnWidth(2, 100);
    queueTree_->setColumnWidth(3, 160);
    queueLayout->addWidget(queueTree_, 1);
    timelineTabs_->insertTab(0, queue, "Render Queue");
    timelineTabs_->setCurrentIndex(1);
    connect(render, &QPushButton::clicked, this, [this, format] {
        auto directory = QFileDialog::getExistingDirectory(this, "Choose output parent folder");
        if (!directory.isEmpty())
            exportTo(QDir(directory).filePath((format->currentIndex() == 1 ? "movie-" : "frames-") +
                                              QDateTime::currentDateTime().toString("yyyyMMdd-HHmmsszzz")),
                     false, format->currentIndex() == 1);
    });
    connect(cancel, &QPushButton::clicked, &worker_, &RenderWorker::cancel);

    connect(graphPropertyBox_, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        if (i >= 0 && i < int(graphProperties_.size()))
            selectGraphProperty(graphProperties_[i]);
    });
    connect(timeline_, &Timeline::propertySelected, this, [this](PropertyRef p) { selectGraphProperty(p); });
    connect(timeline_, &Timeline::navigationStarted, this, &MainWindow::stopPlayback);
    connect(timeline_, &Timeline::timeChanged, this, &MainWindow::setTime);
    connect(graph_, &Graph::timeChanged, this, &MainWindow::setTime);
    connect(timeline_, &Timeline::error, this, &MainWindow::report);
    connect(timeline_, &Timeline::status, this, &MainWindow::report);
    connect(graph_, &Graph::error, this, &MainWindow::report);
    auto *file = menuBar()->addMenu("File");
    auto *layers = menuBar()->addMenu("Layer");
    auto *animation = menuBar()->addMenu("Animation");
    auto *view = menuBar()->addMenu("View");
    auto action = [&](QMenu *menu, QString title, std::function<void()> f, QKeySequence key = {}) {
        auto *a = menu->addAction(title);
        if (!key.isEmpty())
            a->setShortcut(key);
        connect(a, &QAction::triggered, this, [this, f] { guarded(f); });
        return a;
    };
    action(
        file, "New project",
        [this] {
            if (confirmDiscard()) {
                comp_ = 1;
                path_.clear();
                identity_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
                editor_.replace({});
                timeline_->setComposition(comp_);
                graph_->setComposition(comp_);
                setTime(time(0));
            }
        },
        QKeySequence::New);
    action(
        file, "Open project…",
        [this] {
            auto path =
                QFileDialog::getOpenFileName(this, "Open native project", {}, "Motion project (*.json)");
            if (!path.isEmpty() && confirmDiscard())
                openProject(path);
        },
        QKeySequence::Open);
    action(
        file, "Save",
        [this] {
            if (path_.isEmpty()) {
                auto path =
                    QFileDialog::getSaveFileName(this, "Save native project", {}, "Motion project (*.json)");
                if (!path.isEmpty())
                    saveAs(path);
            } else
                saveAs(path_);
        },
        QKeySequence::Save);
    action(
        file, "Save As…",
        [this] {
            auto path =
                QFileDialog::getSaveFileName(this, "Save native project copy", {}, "Motion project (*.json)");
            if (!path.isEmpty())
                saveAs(path);
        },
        QKeySequence::SaveAs);
    action(
        file, "Import media…",
        [this] {
            auto path = QFileDialog::getOpenFileName(
                this, "Import media", {},
                "Media (*.png *.mov *.mp4 *.m4v *.m4a *.wav *.aif *.aiff *.mp3 *.caf)");
            if (!path.isEmpty())
                importMediaFile(path);
        },
        QKeySequence("Ctrl+I"));
    action(file, "New composition…", [this] { configureComposition(true); });
    action(file, "Composition settings…", [this] { configureComposition(false); });
    action(file, "Export H.264 / AAC movie…", [this] {
        auto parent = QFileDialog::getExistingDirectory(this, "Choose movie output parent folder");
        if (!parent.isEmpty())
            exportTo(
                QDir(parent).filePath("movie-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmsszzz")),
                false, true);
    });
    action(file, "Export PNG sequence…", [this] {
        auto parent = QFileDialog::getExistingDirectory(this, "Choose parent output folder");
        if (!parent.isEmpty())
            exportTo(
                QDir(parent).filePath("frames-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")));
    });
    action(file, "Export current PNG…", [this] {
        auto parent = QFileDialog::getExistingDirectory(this, "Choose parent output folder");
        if (!parent.isEmpty())
            exportTo(
                QDir(parent).filePath("still-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")),
                true);
    });
    action(file, "Recover snapshot…", [this] {
        auto path = QFileDialog::getOpenFileName(
            this, "Restore recovery as a new document",
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/recovery",
            "JSON (*.json)");
        if (path.isEmpty() || !confirmDiscard())
            return;
        auto recovered = loadRecovery(path);
        comp_ = recovered.compositions.front().id;
        path_.clear();
        editor_.replace(recovered);
        timeline_->setComposition(comp_);
        graph_->setComposition(comp_);
        report("Recovery restored in memory. Save As to a new project file.");
    });
    for (auto type : {LayerKind::Solid, LayerKind::Text, LayerKind::Null})
        action(layers, "Add " + layerKindName(type), [this, type] { addLayer(type); });
    action(layers, "Duplicate", [this] { duplicate(); }, QKeySequence("Ctrl+D"));
    action(layers, "Delete selected layers", [this] { deleteLayers(); });
    action(layers, "Precompose selection", [this] { precomposeSelection(); });
    auto *edit = menuBar()->addMenu("Edit");
    auto *undo = editor_.undoStack().createUndoAction(this, "Undo");
    undo->setShortcut(QKeySequence::Undo);
    edit->addAction(undo);
    auto *redo = editor_.undoStack().createRedoAction(this, "Redo");
    redo->setShortcut(QKeySequence::Redo);
    edit->addAction(redo);
    action(animation, "Cut selected keys", [this] {
        if (prepareKeyAction())
            timeline_->cutKeys();
    });
    action(animation, "Copy selected keys", [this] {
        if (prepareKeyAction())
            timeline_->copyKeys();
    });
    action(animation, "Paste keys at current time", [this] { timeline_->pasteKeys(); });
    action(animation, "Delete selected keys", [this] {
        if (prepareKeyAction())
            timeline_->deleteKeys();
    });
    action(animation, "Ease selected keys · F9", [this] {
        if (graph_->isVisible())
            graph_->easeSelected();
        else
            timeline_->easeKeys();
    });
    action(animation, "Ease In", [this] {
        if (graph_->isVisible())
            graph_->easeSelected(true, false);
        else
            timeline_->easeKeys(true, false);
    });
    action(animation, "Ease Out", [this] {
        if (graph_->isVisible())
            graph_->easeSelected(false, true);
        else
            timeline_->easeKeys(false, true);
    });
    action(animation, "Keyframe Velocity…", [this] {
        if (graph_->isVisible())
            graph_->editVelocity();
        else
            timeline_->editVelocity();
    });
    for (QString shortcut : {"P", "S", "R", "T", "L", "U", "UU"})
        action(animation, "Show properties · " + shortcut, [this, shortcut] { setFilter(shortcut); });
    action(view, "All properties", [this] { setFilter({}); });
    action(view, "Timeline / graph", [graphToggle] { graphToggle->click(); }, QKeySequence("Shift+F3"));
    action(view, "Preview cache budget…", [this] {
        bool ok;
        int mb = QInputDialog::getInt(this, "Preview cache", "MiB", 256, 16, 1024, 16, &ok);
        if (ok) {
            worker_.setCacheMiB(mb);
            QSettings().setValue("cacheMiB", mb);
        }
    });
    action(view, "Reset workspace", [this] { resetWorkspace(); });
    auto *toolbar = addToolBar("Create");
    toolbar->setObjectName("createToolbar");
    toolbar->setMovable(false);
    toolbar->setMinimumHeight(36);
    auto *tools = new QActionGroup(this);
    auto *select = toolbar->addAction("↖");
    select->setToolTip("Selection tool");
    select->setCheckable(true);
    select->setChecked(true);
    tools->addAction(select);
    auto *hand = toolbar->addAction("Hand");
    hand->setToolTip("Pan the view without moving layers");
    hand->setCheckable(true);
    tools->addAction(hand);
    connect(tools, &QActionGroup::triggered, this, [=, this](QAction *tool) {
        viewer_->hand = tool == hand;
        viewer_->setCursor(viewer_->hand ? Qt::OpenHandCursor : Qt::ArrowCursor);
    });
    toolbar->addSeparator();
    toolbar->addAction("T", this, [this] { addLayer(LayerKind::Text); });
    toolbar->addAction("Solid", this, [this] { addLayer(LayerKind::Solid); });
    toolbar->addAction("Null", this, [this] { addLayer(LayerKind::Null); });
    toolbar->addSeparator();
    toolbar->addAction("Import media", this, [this] {
        auto path = QFileDialog::getOpenFileName(
            this, "Import media", {}, "Media (*.png *.mov *.mp4 *.m4v *.m4a *.wav *.aif *.aiff *.mp3 *.caf)");
        if (!path.isEmpty())
            importMediaFile(path);
    });
    select->setText("Selection");
    select->setShortcut(QKeySequence("V"));
    hand->setShortcut(QKeySequence("H"));
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setIconSize(QSize(20, 20));
    for (auto *tool : toolbar->actions())
        if (!tool->isSeparator()) {
            tool->setIcon(toolIcon(tool->text()));
            if (tool->toolTip().isEmpty())
                tool->setToolTip(tool->text());
        }
    graphToggle->setIcon(toolIcon("Graph"));
    graphToggle->setText({});
    graphToggle->setIconSize(QSize(18, 18));
    auto *window = menuBar()->addMenu("Window");
    for (auto *d : findChildren<QDockWidget *>())
        window->addAction(d->toggleViewAction());
    notice_ = new QLabel("Ready");
    notice_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    notice_->setMaximumWidth(1100);
    renderLabel_ = new QLabel;
    renderLabel_->setToolTip("CPU compositor · preview preparation and cache lookup time");
    cancelExport_ = new QPushButton("Cancel export");
    cancelExport_->hide();
    connect(cancelExport_, &QPushButton::clicked, this, [this] {
        if (importing_) {
            ++importTicket_;
            importThread_.request_stop();
            importing_ = false;
            cancelExport_->hide();
            report("Import cancelled");
        } else if (preparingPlayback_)
            stopPlayback();
        else
            worker_.cancel();
    });
    statusBar()->addWidget(notice_, 1);
    statusBar()->addPermanentWidget(renderLabel_);
    statusBar()->addPermanentWidget(cancelExport_);
    worker_.setCacheMiB(QSettings().value("cacheMiB", 256).toInt());
    viewer_->pressed = [this](QPointF pos, bool extend) {
        guarded([&] {
            if (exporting_)
                return;
            Id hit = 0;
            const auto &c = composition(editor_.project(), comp_);
            for (const auto &l : c.layers) {
                if (!motion::isVisible(l, now_) || l.kind == LayerKind::Null)
                    continue;
                try {
                    auto q = mapPoint(inverse(worldTransform(editor_.project(), comp_, l.id, now_)),
                                      {pos.x(), pos.y()});
                    if (q.x >= 0 && q.y >= 0 && q.x < l.width && q.y < l.height) {
                        hit = l.id;
                        break;
                    }
                } catch (const std::exception &) {
                }
            }
            if (hit) {
                auto ids = extend ? editor_.selection() : std::vector<Id>{};
                if (std::find(ids.begin(), ids.end(), hit) == ids.end())
                    ids.push_back(hit);
                editor_.select(ids);
                editor_.beginGesture();
            } else
                editor_.select({});
        });
    };
    viewer_->moved = [this](QPointF delta) {
        guarded([&] {
            if (exporting_ || editor_.selection().empty())
                return;
            editor_.previewGesture([&](Project &p) {
                for (Id id : editor_.selection()) {
                    auto &l = layer(p, id);
                    auto ancestor = l.parent;
                    bool parentSelected = false;
                    while (ancestor) {
                        if (std::find(editor_.selection().begin(), editor_.selection().end(), *ancestor) !=
                            editor_.selection().end()) {
                            parentSelected = true;
                            break;
                        }
                        ancestor = layer(p, *ancestor).parent;
                    }
                    if (parentSelected)
                        continue;
                    Vec2 d{delta.x(), delta.y()};
                    if (l.parent) {
                        auto inv = inverse(worldTransform(p, comp_, *l.parent, now_));
                        auto a = mapPoint(inv, d), b = mapPoint(inv, {0, 0});
                        d = {a.x - b.x, a.y - b.y};
                    }
                    auto t = sourceTime(l, now_);
                    double x = valueAt(l.channel(Property(2)), t), y = valueAt(l.channel(Property(3)), t);
                    motion::setProperty(p, id, Property::PositionX, t, x + d.x);
                    motion::setProperty(p, id, Property::PositionY, t, y + d.y);
                }
            });
        });
    };
    viewer_->released = [this](bool cancel) {
        guarded([&] {
            if (cancel)
                editor_.cancelGesture();
            else
                editor_.commitGesture("Move layer in viewer");
        });
    };
}
void MainWindow::refresh() {
    QScopedValueRollback guard(refreshing_, true);
    QSignalBlocker block(projectTree_);
    projectTree_->clear();
    for (const auto &c : editor_.project().compositions) {
        auto *item =
            new QTreeWidgetItem(projectTree_, {c.name, "Composition",
                                               QString::number(c.width) + " × " + QString::number(c.height),
                                               QString::number(seconds(c.fps), 'g', 6)});
        item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(c.id));
        item->setData(0, Qt::UserRole + 1, "comp");
    }
    for (const auto &a : editor_.project().assets) {
        auto *item = new QTreeWidgetItem(
            projectTree_,
            {QFileInfo(a.path).fileName(),
             a.kind == AssetKind::Image   ? "PNG file"
             : a.kind == AssetKind::Video ? "Video"
                                          : "Audio",
             a.kind == AssetKind::Audio ? QString::number(a.audioChannels) + " ch"
                                        : QString::number(a.width) + " × " + QString::number(a.height),
             a.kind == AssetKind::Image ? "Still"
             : a.kind == AssetKind::Audio
                 ? QString::number(a.audioRate) + " Hz"
                 : (a.variableRate ? QString("VFR") : QString::number(seconds(a.fps), 'g', 6))});
        item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(a.id));
        item->setData(0, Qt::UserRole + 1, "asset");
        item->setData(0, Qt::UserRole + 3, QDir(baseDirectory_).absoluteFilePath(a.path));
        item->setToolTip(0, a.path + (a.kind == AssetKind::Image ? QString()
                                                                 : QString("\nDuration %1 s%2")
                                                                       .arg(seconds(a.duration), 0, 'f', 3)
                                                                       .arg(a.hasAudio ? " · audio" : "")));
    }
    QTreeWidgetItem *solids = nullptr;
    for (const auto &c : editor_.project().compositions)
        for (const auto &l : c.layers)
            if (l.kind == LayerKind::Solid) {
                if (!solids)
                    solids = new QTreeWidgetItem(projectTree_, {"Solids", "Folder"});
                auto *item = new QTreeWidgetItem(
                    solids,
                    {l.name, "Solid", QString::number(l.width) + " × " + QString::number(l.height), ""});
                item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(l.id));
                item->setData(0, Qt::UserRole + 1, "solid");
                item->setData(0, Qt::UserRole + 2, QVariant::fromValue<qulonglong>(c.id));
                item->setToolTip(0, "Layer-owned solid source");
            }
    if (std::none_of(editor_.project().compositions.begin(), editor_.project().compositions.end(),
                     [&](const Composition &c) { return c.id == comp_; }))
        comp_ = editor_.project().compositions.front().id;
    const auto &c = composition(editor_.project(), comp_);
    static_cast<FrameNumber *>(frameInput_)->setRate(seconds(c.fps));
    if (viewerMode_ == 2 && std::none_of(editor_.project().assets.begin(), editor_.project().assets.end(),
                                         [this](const Asset &a) { return a.id == footage_; }))
        footage_ = 0;
    if (timelineTabs_->count() > 1)
        timelineTabs_->setTabText(1, c.name);
    if (viewerMode_ == 1)
        viewer_->compSize = {c.width, c.height};
    viewerTabs_->setTabText(1, "Composition  " + c.name);
    findChild<QLabel *>("compositionBreadcrumb")->setText(c.name);
    filterProject();
    setWindowTitle((editor_.dirty() ? "● " : "") + c.name + " — Before Effects");
    syncProjectSelection();
    refreshProperties();
    updateAudio();
    requestFrame();
}
void MainWindow::syncProjectSelection() {
    QSignalBlocker block(projectTree_);
    for (QTreeWidgetItemIterator it(projectTree_); *it; ++it) {
        auto *item = *it;
        const auto kind = item->data(0, Qt::UserRole + 1).toString();
        const Id id = item->data(0, Qt::UserRole).toULongLong();
        const bool selected = (viewerMode_ == 2 && kind == "asset" && id == footage_) ||
                              (viewerMode_ != 2 && kind == "comp" && id == comp_);
        item->setSelected(selected);
        if (selected) {
            projectTree_->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
            findChild<QLabel *>("projectInfo")
                ->setText(item->text(0) + "\n" + item->text(1) + "\n" + item->text(2) + "  " + item->text(3));
        }
    }
}
void MainWindow::filterProject() {
    const auto query = projectSearch_->text();
    for (int i = 0; i < projectTree_->topLevelItemCount(); ++i) {
        auto *item = projectTree_->topLevelItem(i);
        bool show = query.isEmpty() || item->text(0).contains(query, Qt::CaseInsensitive);
        for (int j = 0; j < item->childCount(); ++j) {
            auto *child = item->child(j);
            bool match = query.isEmpty() || child->text(0).contains(query, Qt::CaseInsensitive);
            child->setHidden(!match);
            show |= match;
        }
        item->setHidden(!show);
        if (!query.isEmpty() && item->childCount())
            item->setExpanded(true);
    }
}
void MainWindow::resetWorkspace() {
    auto *project = findChild<QDockWidget *>("projectDock");
    auto *effects = findChild<QDockWidget *>("effectControlsDock");
    auto *browser = findChild<QDockWidget *>("effectsBrowserDock");
    auto *aux = findChild<QDockWidget *>("auxiliaryDock");
    auto *timeline = findChild<QDockWidget *>("timelineDock");
    auto *properties = findChild<QDockWidget *>("propertiesDock");
    for (auto *d : findChildren<QDockWidget *>()) {
        d->setFloating(false);
        d->show();
    }
    addDockWidget(Qt::LeftDockWidgetArea, effects);
    tabifyDockWidget(effects, project);
    project->raise();
    addDockWidget(Qt::RightDockWidgetArea, browser);
    splitDockWidget(browser, aux, Qt::Vertical);
    properties->hide();
    addDockWidget(Qt::BottomDockWidgetArea, timeline);
    for (auto *d : {project, effects}) {
        if (!d->titleBarWidget()) {
            auto *bar = new QWidget(d);
            bar->setFixedHeight(0);
            d->setTitleBarWidget(bar);
        }
    }
    for (auto *tabs : findChildren<QTabBar *>())
        tabs->setDrawBase(false);
    const int upper = int(height() * .65);
    resizeDocks({project, browser}, {int(width() * .2145), int(width() * .1494)}, Qt::Horizontal);
    resizeDocks({browser, aux}, {int(upper * .686), int(upper * .314)}, Qt::Vertical);
    int top = menuBar()->isNativeMenuBar() ? 0 : menuBar()->height();
    for (auto *bar : findChildren<QToolBar *>())
        if (bar->isVisible())
            top += bar->height();
    resizeDocks({timeline}, {std::max(240, int((height() - top) * .347) - statusBar()->height())},
                Qt::Vertical);
}
void MainWindow::setViewerMode(int mode) {
    if (exporting_)
        return;
    stopPlayback();
    sourceSlider_->setVisible(mode == 2 && footage_ &&
                              asset(editor_.project(), footage_).kind != AssetKind::Image);
    if (mode == 2 && footage_) {
        const auto p = footageProject();
        const auto &c = p.compositions.front();
        QSignalBlocker block(sourceSlider_);
        sourceSlider_->setRange(0, int(std::max<std::int64_t>(0, frameCount(c.duration, c.fps) - 1)));
        sourceSlider_->setValue(int(std::llround(seconds(footageTime_) * seconds(c.fps))));
    }
    viewerMode_ = mode;
    {
        QSignalBlocker block(viewerTabs_);
        viewerTabs_->setCurrentIndex(mode);
    }
    viewer_->interactive = mode == 1;
    viewer_->offset = {};
    viewer_->outlines.clear();
    refreshProperties();
    syncProjectSelection();
    requestFrame();
}
void MainWindow::alignSelection(int axis, int edge) {
    if (exporting_ || editor_.selection().empty())
        return;
    guarded([&] {
        editor_.apply("Align layer bounds", [&](Project &p) {
            const auto &c = composition(p, comp_);
            const double target = (axis ? c.height : c.width) * edge / 2.;
            for (Id id : editor_.selection()) {
                auto &l = layer(p, id);
                if (l.locked)
                    throw std::runtime_error("Unlock layer first");
                auto transform = worldTransform(p, comp_, id, now_);
                double low = 1e100, high = -1e100;
                for (Vec2 corner : std::array<Vec2, 4>{{{0, 0},
                                                        {double(l.width), 0},
                                                        {0, double(l.height)},
                                                        {double(l.width), double(l.height)}}}) {
                    auto v = mapPoint(transform, corner);
                    double coordinate = axis ? v.y : v.x;
                    low = std::min(low, coordinate);
                    high = std::max(high, coordinate);
                }
                double delta = target - (edge == 0 ? low : edge == 1 ? (low + high) / 2 : high);
                Vec2 change = axis ? Vec2{0, delta} : Vec2{delta, 0};
                if (l.parent) {
                    auto inv = inverse(worldTransform(p, comp_, *l.parent, now_));
                    auto a = mapPoint(inv, {0, 0}), b = mapPoint(inv, change);
                    change = {b.x - a.x, b.y - a.y};
                }
                auto t = sourceTime(l, now_);
                motion::setProperty(p, id, Property::PositionX, t,
                                    valueAt(l.channel(Property::PositionX), t) + change.x);
                motion::setProperty(p, id, Property::PositionY, t,
                                    valueAt(l.channel(Property::PositionY), t) + change.y);
            }
        });
    });
}
void MainWindow::refreshProperties() {
    QScopedValueRollback guard(refreshing_, true);
    if (previewRange_)
        previewRange_->setEnabled(viewerMode_ != 2);
    if (motionBlurToggle_) {
        const QSignalBlocker blocker(motionBlurToggle_);
        motionBlurToggle_->setChecked(composition(editor_.project(), comp_).motionBlurEnabled);
        motionBlurToggle_->setEnabled(!exporting_);
    }
    const auto &selection = editor_.selection();
    const Layer *l = selection.empty() ? nullptr : &layer(editor_.project(), selection.front());
    findChild<QDockWidget *>("propertiesDock")->setEnabled(l && !exporting_);
    viewer_->outlines.clear();
    for (Id id : selection) {
        try {
            const auto &item = layer(editor_.project(), id);
            if (item.kind == LayerKind::Audio)
                continue;
            auto m = worldTransform(editor_.project(), comp_, id, now_);
            QPolygonF poly;
            for (Vec2 pt : std::array<Vec2, 4>{{{0, 0},
                                                {double(item.width), 0},
                                                {double(item.width), double(item.height)},
                                                {0, double(item.height)}}}) {
                auto q = mapPoint(m, pt);
                poly << QPointF(q.x, q.y);
            }
            viewer_->outlines.push_back(poly);
        } catch (const std::exception &) {
        }
    }
    if (viewerMode_ != 1)
        viewer_->outlines.clear();
    viewer_->update();
    if (!l) {
        selectionLabel_->setText("No layer selected");
        effectPanel_->setEnabled(false);
        findChild<QDockWidget *>("effectControlsDock")->setWindowTitle("Effect Controls");
        return;
    }
    bool single = selection.size() == 1;
    auto *dock = findChild<QDockWidget *>("propertiesDock");
    for (auto *button : dock->findChildren<QPushButton *>())
        button->setEnabled(!l->locked);
    selectionLabel_->setText(single ? l->name + " · " + layerKindName(l->kind)
                                    : QString::number(selection.size()) + " layers selected");
    name_->setEnabled(single && !l->locked);
    if (!name_->hasFocus())
        name_->setText(l->name);
    visible_->setChecked(l->visible);
    locked_->setChecked(l->locked);
    propertyPanel_->setTime(now_);
    effectPanel_->setTime(now_);
    effectPanel_->setEnabled(!l->locked && !exporting_);
    findChild<QDockWidget *>("effectControlsDock")->setWindowTitle("Effect Controls  " + l->name);
    {
        const QSignalBlocker blocker(graphPropertyBox_);
        graphPropertyBox_->clear();
        graphProperties_.clear();
        for (const auto &row : propertyRows(*l, &editor_.project())) {
            graphPropertyBox_->addItem(row.group + " / " + row.label);
            graphProperties_.push_back(row.ref);
        }
        auto found = std::find(graphProperties_.begin(), graphProperties_.end(), graphProperty_);
        if (found == graphProperties_.end())
            graphProperty_ = Property::PositionX;
        found = std::find(graphProperties_.begin(), graphProperties_.end(), graphProperty_);
        graphPropertyBox_->setCurrentIndex(int(found - graphProperties_.begin()));
        graph_->setProperty(graphProperty_);
    }
    in_->setValue(seconds(l->in));
    out_->setValue(seconds(l->out));
    start_->setValue(seconds(l->start));
    for (auto *box : {in_, out_, start_})
        box->setProperty("userEdited", false);
    for (auto *b : {in_, out_, start_})
        b->setEnabled(single && !l->locked);
    parent_->clear();
    parent_->addItem("None", QVariant::fromValue<qulonglong>(0));
    for (const auto &other : composition(editor_.project(), comp_).layers)
        if (std::find(selection.begin(), selection.end(), other.id) == selection.end())
            parent_->addItem(other.name, QVariant::fromValue<qulonglong>(other.id));
    parent_->setCurrentIndex(parent_->findData(QVariant::fromValue<qulonglong>(l->parent.value_or(0))));
}
void MainWindow::requestFrame() {
    if (exporting_)
        return;
    const bool audioWaveform = viewerMode_ == 2 && footage_ &&
                               asset(editor_.project(), footage_).kind == AssetKind::Audio;
    viewerChannel_->setEnabled(!audioWaveform);
    viewerExposure_->setEnabled(!audioWaveform && displayOptions_.channel != DisplayChannel::Alpha);
    viewerGrayscale_->setEnabled(
        !audioWaveform && (displayOptions_.channel == DisplayChannel::Red ||
                           displayOptions_.channel == DisplayChannel::Green ||
                           displayOptions_.channel == DisplayChannel::Blue));
    Project preview = editor_.project();
    Id target = comp_;
    Time at = now_;
    QString context = "composition";
    if (viewerMode_ == 0) {
        if (editor_.selection().empty()) {
            worker_.cancel();
            inFlight_ = false;
            active_.requestId = ++requestId_;
            displayedImage_ = {};
            viewer_->image = {};
            viewer_->message = "Select a layer in the timeline";
            viewer_->update();
            return;
        }
        const Id id = editor_.selection().front();
        Layer selected = layer(preview, id);
        Composition *owner = nullptr;
        for (auto &c : preview.compositions)
            if (std::any_of(c.layers.begin(), c.layers.end(), [&](const Layer &l) { return l.id == id; })) {
                owner = &c;
                break;
            }
        if (!owner)
            return;
        target = owner->id;
        owner->width = selected.width;
        owner->height = selected.height;
        selected.parent.reset();
        selected.visible = true;
        for (int i = 0; i < 8; ++i)
            selected.channel(Property(i)) = {i == 4 || i == 5 || i == 7 ? 1. : 0., {}};
        owner->layers = {selected};
        context = "layer:" + idString(id);
        viewerTabs_->setTabText(0, "Layer  " + selected.name);
    } else if (viewerMode_ == 2) {
        if (!footage_) {
            worker_.cancel();
            inFlight_ = false;
            active_.requestId = ++requestId_;
            displayedImage_ = {};
            viewer_->image = {};
            viewer_->message = "Open media from the Project panel";
            viewer_->update();
            return;
        }
        preview = footageProject();
        target = 1;
        at = asset(editor_.project(), footage_).kind == AssetKind::Image ? time(0) : footageTime_;
        context = "footage:" + idString(footage_);
        const auto &media = asset(editor_.project(), footage_);
        viewerTabs_->setTabText(2, "Footage  " + QFileInfo(media.path).fileName());
        if (media.kind == AssetKind::Audio) {
            worker_.cancel();
            active_.requestId = ++requestId_;
            inFlight_ = false;
            QImage waveform(640, 180, QImage::Format_RGBA8888);
            waveform.fill(QColor("#1d1d1d"));
            QPainter painter(&waveform);
            painter.setPen(QColor("#76b59a"));
            for (size_t i = 0; i < media.waveform.size(); ++i) {
                double x = 10 + 620. * i / media.waveform.size(), h = media.waveform[i] * 70;
                painter.drawLine(QPointF(x, 90 - h), QPointF(x, 90 + h));
            }
            painter.setPen(QColor("#43a5f5"));
            double cursor = 10 + 620 * seconds(footageTime_) / seconds(media.duration);
            painter.drawLine(QPointF(cursor, 10), QPointF(cursor, 170));
            painter.end();
            displayedImage_ = waveform;
            viewer_->image = waveform;
            viewer_->compSize = waveform.size();
            viewer_->message.clear();
            frameLabel_->setText(frameText(std::llround(seconds(footageTime_) * 30), 30));
            viewer_->update();
            emit frameDisplayed();
            return;
        }
    }
    const auto &c = composition(preview, target);
    viewer_->compSize = {c.width, c.height};
    if (viewerMode_ != 1)
        viewer_->outlines.clear();
    const int scale = previewScale_->currentIndex() == 0 ? 2 : 1;
    const QSize size(std::max(1, c.width / scale), std::max(1, c.height / scale));
    const bool sameDisplay = active_.display.channel == displayOptions_.channel &&
                             active_.display.grayscale == displayOptions_.grayscale &&
                             active_.display.exposureStops == displayOptions_.exposureStops;
    if (playing_ && inFlight_ && active_.revision == editor_.revision() && active_.comp == target &&
        active_.size == size && active_.context == context && sameDisplay &&
        active_.options.motionBlur == MotionBlurOverride::CurrentSettings)
        return;
    active_ = {++requestId_, editor_.revision(), std::move(preview), target, at, size, baseDirectory_,
               context, displayOptions_, RenderOptions{}};
    inFlight_ = true;
    worker_.preview(active_);
    renderLabel_->setText("CPU · Rendering…");
}
void MainWindow::setTime(Time value) {
    if (!advancingPlayback_ && (playing_ || preparingPlayback_))
        stopPlayback();
    guarded([&] {
        const auto &c = composition(editor_.project(), comp_);
        if (value < time(0))
            value = time(0);
        if (!(value < c.duration))
            value = frameTime(std::max<std::int64_t>(0, frameCount(c.duration, c.fps) - 1), c.fps);
        now_ = value;
        {
            QSignalBlocker b(frameInput_);
            frameInput_->setValue(std::llround(seconds(now_) * seconds(c.fps)));
        }
        timeline_->setTime(now_);
        graph_->setTime(now_);
        refreshProperties();
        requestFrame();
    });
}
void MainWindow::chooseComposition(Id id) {
    if (exporting_)
        return;
    stopPlayback();
    sourceSlider_->hide();
    comp_ = id;
    timelineTabs_->setCurrentIndex(1);
    viewerMode_ = 1;
    {
        QSignalBlocker block(viewerTabs_);
        viewerTabs_->setCurrentIndex(1);
    }
    viewer_->interactive = true;
    editor_.select({});
    timeline_->setComposition(id);
    graph_->setComposition(id);
    setTime(time(0));
    refresh();
}
void MainWindow::selectGraphProperty(PropertyRef ref) {
    graphProperty_ = ref;
    graph_->setProperty(ref);
    const auto it = std::find(graphProperties_.begin(), graphProperties_.end(), ref);
    const QSignalBlocker blocker(graphPropertyBox_);
    graphPropertyBox_->setCurrentIndex(it == graphProperties_.end() ? -1
                                                                    : int(it - graphProperties_.begin()));
}
void MainWindow::addKey(PropertyRef p) {
    if (exporting_ || editor_.selection().empty())
        return;
    guarded([&] {
        editor_.apply("Add keyframe", [&](Project &project) {
            for (Id id : editor_.selection()) {
                auto &l = layer(project, id);
                if (l.locked)
                    throw std::runtime_error("Unlock layer first");
                auto t = sourceTime(l, now_);
                auto &ch = l.channel(p);
                putKey(ch, {t,
                            valueAt(ch, t),
                            discreteProperty(p) ? Interpolation::Hold : Interpolation::Linear,
                            {},
                            {}});
            }
        });
        timeline_->setFilter("U");
        appendLog("Add key " + propertyLabel(p));
    });
}
void MainWindow::addLayer(LayerKind kind) {
    if (exporting_)
        return;
    guarded([&] {
        Id id = 0;
        editor_.apply("Add " + layerKindName(kind), [&](Project &p) {
            auto &c = composition(p, comp_);
            Layer l;
            l.id = p.nextId++;
            id = l.id;
            l.kind = kind;
            l.name = layerKindName(kind) + " " + idString(id);
            l.out = c.duration;
            l.width = c.width;
            l.height = c.height;
            if (kind == LayerKind::Text) {
                l.width = 900;
                l.height = 400;
                l.channel(Property(2)).base = 150;
                l.channel(Property(3)).base = 250;
            }
            if (kind == LayerKind::Null) {
                l.width = l.height = 100;
            }
            c.layers.insert(c.layers.begin(), l);
        });
        editor_.select({id});
        appendLog("Add " + layerKindName(kind));
    });
}
void MainWindow::configureComposition(bool create) {
    if (exporting_)
        return;
    auto current = create ? Composition{} : composition(editor_.project(), comp_);
    QDialog dialog(this);
    dialog.setWindowTitle(create ? "New composition" : "Composition settings");
    auto *layout = new QFormLayout(&dialog);
    auto *name = new QLineEdit(current.name);
    auto *width = number(&dialog, 1, 8192, 0);
    width->setValue(current.width);
    auto *height = number(&dialog, 1, 8192, 0);
    height->setValue(current.height);
    auto *fps = new QLineEdit(QString::number(current.fps.numerator) + "/" +
                              QString::number(current.fps.denominator));
    fps->setToolTip("Exact rate, such as 30/1 or 30000/1001. Decimal rates are also accepted.");
    auto *duration = number(&dialog, .001, 86400, 3);
    duration->setValue(seconds(current.duration));
    const double displayedDuration = duration->value();
    auto *motionBlurEnabled = new QCheckBox;
    motionBlurEnabled->setObjectName("compositionMotionBlurEnabled");
    motionBlurEnabled->setChecked(current.motionBlurEnabled);
    const auto blurTip =
        "Transform-only blur. When nested, the root composition shutter owns the exposure; these settings "
        "apply when rendered directly.";
    motionBlurEnabled->setToolTip(blurTip);
    auto *shutterAngle = new QSpinBox;
    shutterAngle->setObjectName("motionBlurShutterAngle");
    shutterAngle->setRange(0, 720);
    shutterAngle->setSuffix("°");
    shutterAngle->setValue(current.shutterAngle);
    shutterAngle->setToolTip(blurTip);
    auto *shutterPhase = new QSpinBox;
    shutterPhase->setObjectName("motionBlurShutterPhase");
    shutterPhase->setRange(-360, 360);
    shutterPhase->setSuffix("°");
    shutterPhase->setValue(current.shutterPhase);
    shutterPhase->setToolTip(blurTip);
    auto *motionBlurSamples = new QSpinBox;
    motionBlurSamples->setObjectName("motionBlurSamples");
    motionBlurSamples->setRange(1, 64);
    motionBlurSamples->setValue(current.motionBlurSamples);
    motionBlurSamples->setToolTip(blurTip);
    layout->addRow("Name", name);
    layout->addRow("Width", width);
    layout->addRow("Height", height);
    layout->addRow("Frame rate", fps);
    layout->addRow("Duration (seconds)", duration);
    layout->addRow("Motion blur", motionBlurEnabled);
    layout->addRow("Shutter angle", shutterAngle);
    layout->addRow("Shutter phase", shutterPhase);
    layout->addRow("Samples", motionBlurSamples);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    auto pieces = fps->text().split('/');
    Time rate;
    bool ok = false;
    if (pieces.size() == 2) {
        bool second = false;
        auto n = pieces[0].toLongLong(&ok), d = pieces[1].toLongLong(&second);
        if (!ok || !second)
            throw std::runtime_error("Frame rate must be an integer fraction");
        rate = time(n, d);
    } else if (pieces.size() == 1) {
        double value = pieces[0].toDouble(&ok);
        if (!ok)
            throw std::runtime_error("Invalid frame rate");
        rate = fromSeconds(value);
    } else
        throw std::runtime_error("Frame rate must be a number or numerator/denominator");
    Id id = comp_;
    editor_.apply(create ? "New composition" : "Composition settings", [&](Project &p) {
        if (create) {
            current.id = p.nextId++;
            id = current.id;
            p.compositions.push_back(current);
        }
        auto &c = composition(p, id);
        c.name = name->text();
        c.width = int(width->value());
        c.height = int(height->value());
        const auto newDuration = create || duration->value() != displayedDuration
                                     ? fromSeconds(duration->value()) : c.duration;
        setCompositionTiming(c, rate, newDuration);
        c.motionBlurEnabled = motionBlurEnabled->isChecked();
        c.shutterAngle = shutterAngle->value();
        c.shutterPhase = shutterPhase->value();
        c.motionBlurSamples = motionBlurSamples->value();
        for (auto &outer : p.compositions)
            for (auto &l : outer.layers)
                if (l.kind == LayerKind::Precomp && l.source == id) {
                    l.width = c.width;
                    l.height = c.height;
                }
    });
    chooseComposition(id);
}
void MainWindow::importPng(const QString &path) {
    if (exporting_)
        return;
    auto a = inspectPng(path, editor_.project().nextId);
    Id id = 0;
    editor_.apply("Import PNG", [&](Project &p) {
        a.id = p.nextId++;
        p.assets.push_back(a);
        auto &c = composition(p, comp_);
        Layer l;
        l.id = p.nextId++;
        id = l.id;
        l.kind = LayerKind::Image;
        l.source = a.id;
        l.name = QFileInfo(path).baseName();
        l.width = a.width;
        l.height = a.height;
        l.out = c.duration;
        l.channel(Property(2)).base = (c.width - l.width) / 2.;
        l.channel(Property(3)).base = (c.height - l.height) / 2.;
        c.layers.insert(c.layers.begin(), l);
    });
    editor_.select({id});
    appendLog("Import original PNG " + path);
    if (a.assumedSrgb)
        report("Untagged PNG imported with explicit sRGB assumption");
}
void MainWindow::deleteLayers() {
    if (exporting_ || editor_.selection().empty())
        return;
    editor_.apply("Delete layers", [&](Project &p) {
        auto &list = composition(p, comp_).layers;
        auto ids = editor_.selection();
        for (Id id : ids)
            if (layer(p, id).locked)
                throw std::runtime_error("Unlock selected layers first");
        for (auto &l : list)
            if (l.parent && std::find(ids.begin(), ids.end(), *l.parent) != ids.end() &&
                std::find(ids.begin(), ids.end(), l.id) == ids.end())
                throw std::runtime_error("Unparent dependent layers before deleting their parent");
        std::erase_if(list,
                      [&](const Layer &l) { return std::find(ids.begin(), ids.end(), l.id) != ids.end(); });
    });
    editor_.select({});
}
void MainWindow::duplicate() {
    if (exporting_ || editor_.selection().empty())
        return;
    std::vector<Id> ids;
    editor_.apply("Duplicate layers", [&](Project &p) {
        for (Id id : editor_.selection())
            ids.push_back(duplicateLayer(p, comp_, id));
    });
    editor_.select(ids);
}
void MainWindow::precomposeSelection() {
    if (exporting_)
        return;
    Id id = 0;
    editor_.apply("Precompose", [&](Project &p) { id = precompose(p, comp_, editor_.selection()); });
    editor_.select({id});
    appendLog("Precompose selected layers");
}
void MainWindow::parentSelection() {
    if (exporting_)
        return;
    guarded([&] {
        Id parent = parent_->currentData().toULongLong();
        editor_.apply("Assign parent at current time", [&](Project &p) {
            for (Id id : editor_.selection())
                reparent(p, comp_, id, parent ? std::optional<Id>(parent) : std::nullopt, now_);
        });
        report(
            "Parent assigned. Appearance preserved at the current time; animated trajectories may change.");
    });
}
bool MainWindow::openProject(const QString &path) {
    if (exporting_)
        return false;
    try {
        auto p = loadProject(path);
        QDir base(projectDirectory(path));
        for (auto &a : p.assets)
            a.path = base.absoluteFilePath(a.path);
        validateProject(p);
        comp_ = p.compositions.front().id;
        viewerMode_ = 1;
        footage_ = 0;
        viewerTabs_->setTabText(0, "Layer (none)");
        viewerTabs_->setTabText(2, "Footage (none)");
        {
            QSignalBlocker block(viewerTabs_);
            viewerTabs_->setCurrentIndex(1);
        }
        viewer_->interactive = true;
        baseDirectory_ = base.absolutePath();
        path_ = path;
        identity_ = QString::fromLatin1(
            QCryptographicHash::hash(QFileInfo(path).absoluteFilePath().toUtf8(), QCryptographicHash::Sha256)
                .toHex());
        editor_.replace(p);
        timeline_->setComposition(comp_);
        graph_->setComposition(comp_);
        timelineTabs_->setCurrentIndex(1);
        setTime(time(0));
        appendLog("Open project " + path);
        lastError_.clear();
        notice_->setText("Native project opened");
        notice_->setToolTip({});
        return true;
    } catch (const std::exception &e) {
        report(e.what());
        return false;
    }
}
bool MainWindow::saveAs(const QString &path) {
    if (exporting_)
        return false;
    try {
        auto copy = editor_.project();
        QDir base(projectDirectory(path));
        for (auto &a : copy.assets)
            a.path = base.relativeFilePath(QDir(baseDirectory_).absoluteFilePath(a.path));
        saveProject(copy, path);
        path_ = QFileInfo(path).absoluteFilePath();
        baseDirectory_ = base.absolutePath();
        identity_ =
            QString::fromLatin1(QCryptographicHash::hash(path_.toUtf8(), QCryptographicHash::Sha256).toHex());
        editor_.undoStack().setClean();
        setWindowTitle(composition(editor_.project(), comp_).name + " — Before Effects");
        appendLog("Save As " + path_);
        return true;
    } catch (const std::exception &e) {
        report(e.what());
        return false;
    }
}
void MainWindow::saveRecoveryNow() {
    if (!editor_.dirty() || exporting_)
        return;
    guarded([&] {
        saveRecovery(editor_.project(), identity_, editor_.revision(),
                     QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/recovery");
    });
}
bool MainWindow::confirmDiscard() {
    if (exporting_) {
        report("Cancel or finish the active export before replacing the project");
        return false;
    }
    if (!editor_.dirty())
        return true;
    auto choice = QMessageBox::warning(this, "Unsaved project", "Save your changes before continuing?",
                                       QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                       QMessageBox::Save);
    if (choice == QMessageBox::Cancel)
        return false;
    if (choice == QMessageBox::Discard)
        return true;
    QString target = path_;
    if (target.isEmpty())
        target = QFileDialog::getSaveFileName(this, "Save project", {}, "Motion project (*.json)");
    return !target.isEmpty() && saveAs(target);
}
void MainWindow::closeEvent(QCloseEvent *event) {
    if (!confirmDiscard()) {
        event->ignore();
        return;
    }
    QSettings settings;
    settings.setValue("windowGeometry-ae-reference-v1", saveGeometry());
    settings.setValue("workspace-ae-reference-v1", saveState());
    QMainWindow::closeEvent(event);
}
void MainWindow::setFilter(QString filter) {
    timeline_->setFilter(filter);
    timelineTabs_->setCurrentIndex(1);
}
void MainWindow::togglePlayback() {
    if (playing_ || preparingPlayback_) {
        stopPlayback();
        requestFrame();
    } else
        beginPlayback();
}

bool MainWindow::prepareKeyAction() {
    if (graph_->isVisible()) {
        auto ref = graph_->keySelection();
        if (!ref) {
            report("Select a graph keyframe first");
            return false;
        }
        timeline_->selectKey(*ref);
    }
    return true;
}
bool MainWindow::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::FocusIn && uPending_) {
        uTimer_.stop();
        uPending_ = false;
    }
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride)
        return QMainWindow::eventFilter(object, event);
    auto *focus = QApplication::focusWidget();
    if (!focus || focus->window() != this)
        return false;
    auto *key = static_cast<QKeyEvent *>(event);
    if (editor_.gesturing() && (key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo))) {
        if (event->type() == QEvent::ShortcutOverride) {
            event->accept();
            return true;
        }
        QKeyEvent cancel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(focus, &cancel);
        if (editor_.gesturing())
            editor_.cancelGesture();
        return true;
    }
    if (event->type() == QEvent::ShortcutOverride)
        return false;
    if (qobject_cast<QLineEdit *>(focus) || qobject_cast<QTextEdit *>(focus) ||
        qobject_cast<QPlainTextEdit *>(focus) || qobject_cast<QAbstractSpinBox *>(focus))
        return false;
    if (exporting_)
        return false;
    if (key->matches(QKeySequence::Cut) &&
        (focus == timeline_ || timeline_->isAncestorOf(focus) || focus == graph_)) {
        if (prepareKeyAction())
            timeline_->cutKeys();
        return true;
    }
    if (key->matches(QKeySequence::Copy) &&
        (focus == timeline_ || timeline_->isAncestorOf(focus) || focus == graph_)) {
        if (prepareKeyAction())
            timeline_->copyKeys();
        return true;
    }
    if (key->matches(QKeySequence::Paste) &&
        (focus == timeline_ || timeline_->isAncestorOf(focus) || focus == graph_)) {
        timeline_->pasteKeys();
        return true;
    }
    if (key->modifiers() != Qt::NoModifier)
        return false;
    if (key->key() == Qt::Key_Space) {
        togglePlayback();
        return true;
    }
    if (key->key() == Qt::Key_B || key->key() == Qt::Key_N) {
        timeline_->setWorkAreaBoundary(key->key() == Qt::Key_N);
        return true;
    }
    if (key->key() == Qt::Key_U) {
        if (uPending_) {
            uPending_ = false;
            uTimer_.stop();
            setFilter("UU");
        } else {
            uPending_ = true;
            uTimer_.start(QApplication::doubleClickInterval());
        }
        return true;
    }
    if (key->key() == Qt::Key_F9) {
        if (graph_->isVisible())
            graph_->easeSelected();
        else
            timeline_->easeKeys();
        return true;
    }
    if (key->key() == Qt::Key_P || key->key() == Qt::Key_S || key->key() == Qt::Key_R ||
        key->key() == Qt::Key_T || key->key() == Qt::Key_A || key->key() == Qt::Key_E ||
        key->key() == Qt::Key_M || key->key() == Qt::Key_L) {
        setFilter(QString(QChar(key->key())));
        return true;
    }
    if (key->key() == Qt::Key_J || key->key() == Qt::Key_K) {
        timeline_->navigateKey(key->key() == Qt::Key_K);
        return true;
    }
    if (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right) {
        if (viewerMode_ == 2 && footage_) {
            stopPlayback();
            auto p = footageProject();
            auto &c = p.compositions.front();
            footageTime_ =
                std::clamp(footageTime_ + frameTime(key->key() == Qt::Key_Left ? -1 : 1, c.fps), time(0),
                           frameTime(std::max<std::int64_t>(0, frameCount(c.duration, c.fps) - 1), c.fps));
            {
                QSignalBlocker block(sourceSlider_);
                sourceSlider_->setValue(int(std::llround(seconds(footageTime_) * seconds(c.fps))));
            }
            requestFrame();
        } else
            setTime(now_ + frameTime(key->key() == Qt::Key_Left ? -1 : 1,
                                     composition(editor_.project(), comp_).fps));
        return true;
    }
    return false;
}
void MainWindow::exportTo(const QString &directory, bool still, bool movie) {
    if (exporting_)
        return;
    if (importing_) {
        report("Finish or cancel the import first");
        return;
    }
    stopPlayback();
    guarded([&] {
        const auto &c = composition(editor_.project(), comp_);
        RenderOptions options;
        options.motionBlur = static_cast<MotionBlurOverride>(renderMotionBlurOverride_->currentData().toInt());
        const auto range = compositionFrameRange(c, renderTimeSpan_->currentData().toBool());
        ExportRequest job{editor_.project(),
                          comp_,
                          still ? std::llround(seconds(now_) * seconds(c.fps)) : range.firstFrame,
                          still ? 1 : range.frameCount,
                          directory,
                          baseDirectory_,
                          movie,
                          options};
        queueItem_ = new QTreeWidgetItem(
            queueTree_, {c.name, "Rendering", "0 / " + QString::number(job.frameCount),
                         movie ? "H.264 / AAC · opaque sRGB" : "PNG · RGBA8 · sRGB", directory});
        queueItem_->setToolTip(2, QString("Frames %1–%2 · %3")
                                     .arg(job.firstFrame).arg(job.firstFrame + job.frameCount - 1)
                                     .arg(still ? "Current frame" : renderTimeSpan_->currentText()));
        findChild<QComboBox *>("renderFormat")->setCurrentIndex(movie ? 1 : 0);
        queueProgress_->setValue(0);
        findChild<QPushButton *>("queueCancel")->setEnabled(true);
        findChild<QPushButton *>("queueRender")->setEnabled(false);
        worker_.exportSequence(job);
        exporting_ = true;
        inFlight_ = false;
        menuBar()->setEnabled(false);
        for (auto *toolbar : findChildren<QToolBar *>())
            toolbar->setEnabled(false);
        projectTree_->setEnabled(false);
        timelineTabs_->setEnabled(false);
        findChild<QDockWidget *>("propertiesDock")->setEnabled(false);
        cancelExport_->show();
        viewerTabs_->setEnabled(false);
        sourceSlider_->setEnabled(false);
        report(movie ? "Preparing verified video and audio export…"
                     : "Preparing sources for verified PNG export…");
        appendLog("Export PNG " + directory);
    });
}
void MainWindow::authorProof(const QString &directory) {
    if (!QDir().mkpath(directory))
        throw std::runtime_error("Cannot create proof directory");
    comp_ = 1;
    editor_.replace({});
    timeline_->setComposition(1);
    graph_->setComposition(1);
    baseDirectory_ = directory;
    actionLog_.clear();
    QImage image(560, 560, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    image.setColorSpace(QColorSpace::SRgb);
    {
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#6ad9cc"));
        p.drawEllipse(QRectF(20, 20, 520, 520));
        p.setBrush(QColor("#122c39"));
        p.drawEllipse(QRectF(100, 100, 360, 360));
        p.setBrush(QColor("#f3bb72"));
        p.drawEllipse(QRectF(218, 218, 124, 124));
        p.setPen(QPen(QColor("#e3f6ed"), 8));
        p.drawLine(QPointF(280, 45), QPointF(280, 120));
        p.drawLine(QPointF(440, 280), QPointF(515, 280));
    }
    auto imagePath = QDir(directory).filePath("original-image.png");
    if (QFileInfo::exists(imagePath))
        throw std::runtime_error("Proof image already exists; choose a fresh authoring directory");
    if (!image.save(imagePath))
        throw std::runtime_error("Cannot save original PNG");
    addLayer(LayerKind::Solid);
    Id background = editor_.selection().front();
    editor_.apply("Background", [&](Project &p) {
        auto &l = layer(p, background);
        l.name = "Midnight background";
        l.setColor(QColor("#10232e"));
    });
    importPng(imagePath);
    Id graphic = editor_.selection().front();
    editor_.apply("Image mask and motion", [&](Project &p) {
        auto &l = layer(p, graphic);
        l.name = "Original orbit";
        l.channel(Property(0)).base = l.channel(Property(1)).base = 280;
        l.channel(Property(2)).keys = {{time(0), 1240}, {time(12), 1370}};
        l.channel(Property(3)).base = 550;
        l.channel(Property(6)).keys = {{time(0), -12}, {time(12), 35}};
        l.setMask(Mask{MaskShape::Rectangle, MaskMode::Add, false, 0, 0, 560, 560});
        nativeEase(l.channel(Property(2)), time(0), time(12));
    });
    appendLog("Edit image transform keys, native easing, rectangular mask");
    addLayer(LayerKind::Text);
    Id title = editor_.selection().front();
    editor_.apply("Original title", [&](Project &p) {
        auto &l = layer(p, title);
        l.name = "Motion / Рух";
        l.string("text.source") = "Motion\nРух";
        l.base("text.size") = 138;
        l.base("text.leading") = 1.08;
        l.setColor(QColor("#ecf1e7"));
        l.width = 900;
        l.height = 420;
        l.channel(Property(2)).keys = {{time(0), 100}, {time(2), 180}, {time(10), 180}, {time(12), 210}};
        l.channel(Property(3)).base = 335;
        l.channel(Property(7)).keys = {{time(0), 0}, {time(1), 1}, {time(10), 1}, {time(12), 0}};
        nativeEase(l.channel(Property(2)), time(0), time(12));
        nativeEase(l.channel(Property(7)), time(0), time(12));
    });
    appendLog("Edit two-line Noto Sans title, position and opacity keys");
    addLayer(LayerKind::Null);
    Id rig = editor_.selection().front();
    editor_.apply("Title parent rig", [&](Project &p) {
        layer(p, rig).name = "Title parent";
        reparent(p, 1, title, rig, time(0));
        auto &l = layer(p, rig);
        l.channel(Property(3)).keys = {{time(0), 0}, {time(6), -12}, {time(12), 0}};
        nativeEase(l.channel(Property(3)), time(0), time(12));
    });
    appendLog("Assign transform parent and animate rig");
    editor_.select({rig, title});
    precomposeSelection();
    if (!saveAs(QDir(directory).filePath("original-scene.json")))
        throw std::runtime_error(lastError_.toStdString());
    atomicWrite(QDir(directory).filePath("authoring-log.txt"), actionLog_.toUtf8());
    editor_.select({graphic});
    setTime(time(5));
    setFilter("U");
}
} // namespace motion
