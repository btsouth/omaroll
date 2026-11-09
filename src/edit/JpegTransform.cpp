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

Result apply(const QString& source, const QString& output, int quarterTurns,
             bool flipHorizontal, bool flipVertical) {
  const QStringList transform = transformArguments(quarterTurns, flipHorizontal, flipVertical);
  if (transform.isEmpty()) {
    return Result::Failed;
  }
  const QString executable = QStandardPaths::findExecutable(QStringLiteral("jpegtran"));
  if (executable.isEmpty() || !QFileInfo(source).isFile()) {
    return Result::Failed;
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
    process.kill();
    process.waitForFinished();
    // jpegtran's words for a -perfect refusal; a damaged file exits 2 and a
    // timeout or crash is not a normal exit at all.
    const bool notPerfect = process.exitStatus() == QProcess::NormalExit &&
                            process.exitCode() == 1 &&
                            process.readAllStandardError().contains("transformation is not perfect");
    return notPerfect ? Result::NotPerfect : Result::Failed;
  }
  return QFileInfo(output).size() > 0 ? Result::Done : Result::Failed;
}

namespace {

// The linear part of a square symmetry on screen, y pointing down: column
// vectors, so a product applies right to left.
struct Matrix {
  int a = 1, b = 0, c = 0, d = 1;
  Matrix operator*(const Matrix& o) const {
    return {a * o.a + b * o.c, a * o.b + b * o.d, c * o.a + d * o.c, c * o.b + d * o.d};
  }
  bool operator==(const Matrix&) const = default;
};
constexpr Matrix kClockwise{0, -1, 1, 0};
constexpr Matrix kMirror{-1, 0, 0, 1};
constexpr Matrix kFlip{1, 0, 0, -1};

Matrix turns(int count) {
  Matrix result;
  for (int i = 0; i < ((count % 4) + 4) % 4; ++i) result = kClockwise * result;
  return result;
}

} // namespace

Symmetry compose(QImageIOHandler::Transformations orientation, int quarterTurns,
                 bool flipHorizontal, bool flipVertical) {
  // Qt applies a tag's mirror and flip first, then its quarter turn.
  Matrix tag;
  if (orientation & QImageIOHandler::TransformationMirror) tag = kMirror * tag;
  if (orientation & QImageIOHandler::TransformationFlip) tag = kFlip * tag;
  if (orientation & QImageIOHandler::TransformationRotate90) tag = kClockwise * tag;
  Matrix view = turns(quarterTurns);
  if (flipHorizontal) view = kMirror * view;
  if (flipVertical) view = kFlip * view;
  const Matrix net = view * tag;
  for (int count = 0; count < 4; ++count) {
    if (turns(count) == net) return {count, false};
    if (kMirror * turns(count) == net) return {count, true};
  }
  return {};
}

} // namespace JpegTransform
