#pragma once

#include <QAbstractItemModel>

namespace CaptureRoles {

enum Role {
  PathRole = Qt::UserRole + 1,
  FileNameRole,
  KindRole,
  KindLabelRole,
  CapturedRole,
  // Stable "2026-08-31" key plus the human label, so QML groups by the first
  // and prints the second without reformatting per delegate.
  DayKeyRole,
  DayLabelRole,
  TimeLabelRole,
  SizeLabelRole,
  BytesRole,
  StampRole,
  IsVideoRole,
  FavoriteRole,
  HiddenRole,
  // Added by CaptureFilterModel while a search result needs OCR context.
  OcrSnippetRole,
};

} // namespace CaptureRoles
