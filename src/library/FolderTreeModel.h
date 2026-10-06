#pragma once

#include <QAbstractItemModel>
#include <QHash>
#include <QTimer>
#include <QVariantList>

#include <memory>
#include <vector>

class CaptureModel;
class CaptureFilterModel;

// A projection of the indexed library, with no filesystem access. Counts include
// descendants, just like the folder filter. Overlapping sources keep their roots.
class FolderTreeModel final : public QAbstractItemModel {
  Q_OBJECT
public:
  enum Role { FolderNameRole = Qt::UserRole + 1, FolderPathRole, MediaCountRole };
  explicit FolderTreeModel(QObject* parent = nullptr);
  void bind(CaptureModel* library, CaptureFilterModel* captures);
  void setFolders(const QVariantList& sources, const QStringList& folders,
                  const QHash<QString, int>& counts);

  QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
  QModelIndex parent(const QModelIndex& index) const override;
  int rowCount(const QModelIndex& parent = {}) const override;
  int columnCount(const QModelIndex& = {}) const override { return 1; }
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QHash<int, QByteArray> roleNames() const override;
  Q_INVOKABLE QModelIndex indexForPath(const QString& path) const;
  Q_INVOKABLE QString pathForIndex(const QModelIndex& index) const;

private:
  struct Node {
    QString name;
    QString path;
    int count = 0;
    int row = 0;
    Node* parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;
  };
  Node* node(const QModelIndex& index) const;
  Node m_root;
  QHash<QString, Node*> m_byPath;
  QTimer m_rebuildTimer;
};
