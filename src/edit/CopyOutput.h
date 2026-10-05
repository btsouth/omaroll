#pragma once

#include <QFile>
#include <QString>

// Encode privately beside the destination, then publish a numbered copy
// without replacing an existing entry. Directory descriptors pin all writes
// and cleanup even if a folder or one of its ancestors is moved.
class CopyOutput final {
public:
  explicit CopyOutput(const QString& preferredPath);
  ~CopyOutput();
  CopyOutput(const CopyOutput&) = delete;
  CopyOutput& operator=(const CopyOutput&) = delete;

  [[nodiscard]] QFile* device() { return m_file.isOpen() ? &m_file : nullptr; }
  [[nodiscard]] QString encoderPath() const;
  [[nodiscard]] QString publish();

private:
  QString m_path;
  QByteArray m_temporaryName;
  QFile m_file;
  int m_directory = -1;
  int m_temporaryDirectory = -1;
};
