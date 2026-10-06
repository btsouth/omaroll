#include "library/FolderTreeModel.h"

#include "library/CaptureFilterModel.h"
#include "library/CaptureModel.h"

#include <QDir>

#include <algorithm>

FolderTreeModel::FolderTreeModel(QObject* parent) : QAbstractItemModel(parent) {
  m_rebuildTimer.setSingleShot(true);
  m_rebuildTimer.setInterval(0);
}

void FolderTreeModel::bind(CaptureModel* library, CaptureFilterModel* captures) {
  connect(&m_rebuildTimer, &QTimer::timeout, this, [this, library, captures] {
    QHash<QString, int> counts;
    // The proxy already has recursive ancestor counts, including source roots
    // and intermediate directories that contain no media of their own.
    for (const QString& folder : captures->folders()) {
      QString path = folder;
      while (!path.isEmpty()) {
        if (counts.contains(path)) break;
        counts.insert(path, captures->folderItemCount(path));
        if (path == QStringLiteral("/")) break;
        const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
        path = slash <= 0 ? QStringLiteral("/") : path.left(slash);
      }
    }
    setFolders(library->folderSources(), captures->folders(), counts);
  });
  connect(library, &CaptureModel::folderSourcesChanged, &m_rebuildTimer,
          qOverload<>(&QTimer::start));
  connect(captures, &CaptureFilterModel::foldersChanged, &m_rebuildTimer,
          qOverload<>(&QTimer::start));
  m_rebuildTimer.start();
}

void FolderTreeModel::setFolders(const QVariantList& sources, const QStringList& folders,
                                const QHash<QString, int>& counts) {
  Node nextRoot;
  QHash<QString, Node*> nextByPath;
  QStringList sorted = folders;
  std::sort(sorted.begin(), sorted.end());
  for (const QVariant& source : sources) {
    const auto row = source.toMap();
    const QString path = QDir::cleanPath(row.value(QStringLiteral("path")).toString());
    if (!path.startsWith(QLatin1Char('/'))) continue;
    auto root = std::make_unique<Node>();
    root->name = row.value(QStringLiteral("label")).toString();
    root->path = path;
    root->count = counts.value(path);
    root->parent = &m_root;
    root->row = static_cast<int>(nextRoot.children.size());
    QHash<QString, Node*> byPath{{path, root.get()}};
    nextByPath.insert(path, root.get());
    const QString prefix = path.endsWith(QLatin1Char('/')) ? path : path + QLatin1Char('/');
    for (const QString& folder : sorted) {
      if (!folder.startsWith(prefix)) continue;
      Node* current = root.get();
      for (const QString& part : folder.mid(prefix.size()).split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        const QString childPath = current->path == QStringLiteral("/")
                                      ? current->path + part : current->path + QLatin1Char('/') + part;
        Node* child = byPath.value(childPath);
        if (!child) {
          auto entry = std::make_unique<Node>();
          entry->name = part;
          entry->path = childPath;
          entry->count = counts.value(childPath);
          entry->row = static_cast<int>(current->children.size());
          entry->parent = current;
          child = entry.get();
          current->children.push_back(std::move(entry));
          byPath.insert(childPath, child);
          nextByPath.insert(childPath, child);
        }
        current = child;
      }
    }
    nextRoot.children.push_back(std::move(root));
  }
  const auto sameStructure = [](const auto& self, const Node& current, const Node& next) -> bool {
    if (current.path != next.path || current.name != next.name ||
        current.children.size() != next.children.size()) return false;
    for (size_t i = 0; i < current.children.size(); ++i) {
      if (!self(self, *current.children[i], *next.children[i])) return false;
    }
    return true;
  };
  if (sameStructure(sameStructure, m_root, nextRoot)) {
    const auto updateCounts = [this](const auto& self, Node& current, const Node& next) -> void {
      if (current.count != next.count) {
        current.count = next.count;
        const QModelIndex changed = createIndex(current.row, 0, &current);
        emit dataChanged(changed, changed, {MediaCountRole});
      }
      for (size_t i = 0; i < current.children.size(); ++i) {
        self(self, *current.children[i], *next.children[i]);
      }
    };
    updateCounts(updateCounts, m_root, nextRoot);
    return;
  }
  beginResetModel();
  m_root.children = std::move(nextRoot.children);
  m_byPath = std::move(nextByPath);
  endResetModel();
}

FolderTreeModel::Node* FolderTreeModel::node(const QModelIndex& index) const {
  return index.isValid() ? static_cast<Node*>(index.internalPointer())
                         : const_cast<Node*>(&m_root);
}

QModelIndex FolderTreeModel::index(int row, int column, const QModelIndex& parent) const {
  if (column != 0 || row < 0 || row >= rowCount(parent)) return {};
  return createIndex(row, column, node(parent)->children.at(row).get());
}

QModelIndex FolderTreeModel::parent(const QModelIndex& index) const {
  if (!index.isValid()) return {};
  Node* parent = node(index)->parent;
  return parent == &m_root ? QModelIndex() : createIndex(parent->row, 0, parent);
}

int FolderTreeModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() && parent.column() != 0 ? 0
             : static_cast<int>(node(parent)->children.size());
}

QVariant FolderTreeModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid()) return {};
  const Node* entry = node(index);
  switch (role) {
    case Qt::DisplayRole:
    case FolderNameRole: return entry->name;
    case FolderPathRole: return entry->path;
    case MediaCountRole: return entry->count;
    default: return {};
  }
}

QHash<int, QByteArray> FolderTreeModel::roleNames() const {
  return {{FolderNameRole, "folderName"}, {FolderPathRole, "folderPath"},
          {MediaCountRole, "mediaCount"}};
}

QModelIndex FolderTreeModel::indexForPath(const QString& path) const {
  const Node* entry = m_byPath.value(path);
  return entry ? createIndex(entry->row, 0, entry) : QModelIndex();
}

QString FolderTreeModel::pathForIndex(const QModelIndex& index) const {
  return data(index, FolderPathRole).toString();
}
