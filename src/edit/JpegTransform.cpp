#include "edit/JpegTransform.h"

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>

namespace {

// The eight square symmetries, as jpegtran options. The pipeline order is a
// quarter turn first, then the flips, so the combinations collapse onto
// jpegtran's rotation, flip, transpose and transverse options.
QStringList transformArguments(int quarterTurns, bool flipHorizontal, bool flipVertical) {
  const int turns = ((quarterTurns % 4) + 4) % 4;
  if (turns == 0) {
    if (!flipHorizontal && !flipVertical) {
      return {};
    }
    if (flipHorizontal && flipVertical) {
      return {QStringLiteral("-rotate"), QStringLiteral("180")};
    }
    return {QStringLiteral("-flip"),
            flipHorizontal ? QStringLiteral("horizontal") : QStringLiteral("vertical")};
  }
  if (turns == 1) {
    if (!flipHorizontal && !flipVertical) {
      return {QStringLiteral("-rotate"), QStringLiteral("90")};
    }
    if (flipHorizontal && !flipVertical) {
      return {QStringLiteral("-transpose")};
    }
    if (!flipHorizontal && flipVertical) {
      return {QStringLiteral("-transverse")};
    }
    return {QStringLiteral("-rotate"), QStringLiteral("270")};
  }
  if (turns == 2) {
    if (!flipHorizontal && !flipVertical) {
      return {QStringLiteral("-rotate"), QStringLiteral("180")};
    }
    if (flipHorizontal && !flipVertical) {
      return {QStringLiteral("-flip"), QStringLiteral("vertical")};
    }
    if (!flipHorizontal && flipVertical) {
      return {QStringLiteral("-flip"), QStringLiteral("horizontal")};
    }
    return {}; // turn 180 plus both flips is the identity
  }
  // turns == 3
  if (!flipHorizontal && !flipVertical) {
    return {QStringLiteral("-rotate"), QStringLiteral("270")};
  }
  if (flipHorizontal && !flipVertical) {
    return {QStringLiteral("-transverse")};
  }
  if (!flipHorizontal && flipVertical) {
    return {QStringLiteral("-transpose")};
  }
  return {QStringLiteral("-rotate"), QStringLiteral("90")};
}

} // namespace

namespace JpegTransform {

bool available() {
  return !QStandardPaths::findExecutable(QStringLiteral("jpegtran")).isEmpty();
}

bool apply(const QString& source, const QString& output, int quarterTurns, bool flipHorizontal,
           bool flipVertical) {
  const QStringList transform = transformArguments(quarterTurns, flipHorizontal, flipVertical);
  if (transform.isEmpty()) {
    return false;
  }
  const QString executable = QStandardPaths::findExecutable(QStringLiteral("jpegtran"));
  if (executable.isEmpty() || !QFileInfo(source).isFile()) {
    return false;
  }

  // -perfect makes jpegtran refuse rather than quietly drop a partial edge
  // block on dimensions that are not a whole number of blocks; -copy all keeps
  // the ICC profile and the rest of the metadata.
  QStringList arguments = transform;
  arguments << QStringLiteral("-perfect") << QStringLiteral("-copy") << QStringLiteral("all")
            << QStringLiteral("-outfile") << output << source;

  QProcess process;
  process.start(executable, arguments);
  if (!process.waitForFinished(15000) || process.exitStatus() != QProcess::NormalExit ||
      process.exitCode() != 0) {
    QFile::remove(output);
    return false;
  }
  return QFileInfo(output).size() > 0;
}

} // namespace JpegTransform
