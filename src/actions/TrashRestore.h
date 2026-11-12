#pragma once

#include <QString>
#include <QVariantMap>

// Moving a file to the XDG Trash in a way that can be taken back.
//
// moveToTrash records where the file went and the identity it has there, so
// an undo puts back exactly that file: never a different file that was later
// given the same name in the Trash, and never over a file that has since
// taken the original name. Both are safe to call off the GUI thread.
namespace TrashRestore {

// Moves the entry at |path| (a symlink itself, not its target) to the Trash.
// On success returns {path, trashPath, device, inode, stamp}; trashPath is
// empty if the platform did not say where the file went, which makes it not
// undoable.
// On failure returns {} and sets |error| to a sentence for the status line.
[[nodiscard]] QVariantMap moveToTrash(const QString& path, QString* error = nullptr);

struct Result {
  bool ok = false;
  QString message;
  // Refused for a reason the user can fix (the name is taken, the folder is
  // gone), so the undo is worth offering again.
  bool retryable = false;
  // The file came back but one of its sidecars did not, and stays in the
  // Trash: "its sidecar stayed in Trash because <name> exists".
  QString sidecarNote = QString();
};

// Puts a file recorded by moveToTrash back where it was, and removes its
// .trashinfo. Refuses, leaving everything as it is, when the trashed file is
// gone or is a different file, or when something now occupies the original
// path. Records under "sidecars" (an .xmp trashed with its file) are put
// back too once the file is; one that cannot be stays in the Trash and is
// named in sidecarNote.
[[nodiscard]] Result restore(const QVariantMap& record);

} // namespace TrashRestore
