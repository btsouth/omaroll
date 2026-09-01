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
  IsVideoRole,
  // First row of its day, so the delegate knows to draw a header above itself.
  IsDayStartRole,
};

} // namespace CaptureRoles
