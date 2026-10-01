// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QTreeWidget>
#include <QUrl>
#include <functional>
namespace motion {
class MediaTree final : public QTreeWidget {
  public:
    std::function<void(QString)> importFile;
    MediaTree() {
        setDragEnabled(true);
        setAcceptDrops(true);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::CopyAction);
    }

  protected:
    QMimeData *mimeData(const QList<QTreeWidgetItem *> &items) const override {
        auto *data = new QMimeData;
        if (items.size() != 1)
            return data;
        auto *item = items.front();
        auto kind = item->data(0, Qt::UserRole + 1).toString();
        if (kind == "asset" || kind == "comp") {
            data->setData("application/x-motionproof-item",
                          QJsonDocument(QJsonObject{{"id", item->data(0, Qt::UserRole).toString()},
                                                    {"kind", kind},
                                                    {"token", property("dragToken").toString()}})
                              .toJson(QJsonDocument::Compact));
            if (kind == "asset")
                data->setUrls({QUrl::fromLocalFile(item->data(0, Qt::UserRole + 3).toString())});
        }
        return data;
    }
    void dragEnterEvent(QDragEnterEvent *event) override {
        if (event->mimeData()->hasUrls())
            event->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent *event) override {
        if (event->mimeData()->hasUrls())
            event->acceptProposedAction();
    }
    void dropEvent(QDropEvent *event) override {
        if (event->mimeData()->hasFormat("application/x-motionproof-item"))
            return;
        auto urls = event->mimeData()->urls();
        if (urls.size() == 1 && urls.front().isLocalFile() && importFile) {
            importFile(urls.front().toLocalFile());
            event->acceptProposedAction();
        }
    }
};
} // namespace motion
