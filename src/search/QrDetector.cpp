#include "search/QrDetector.h"

#include "library/CaptureModel.h"

#include <QStandardPaths>

namespace {

constexpr int kQrTimeoutMs = 10'000;

} // namespace

QrDetector::QrDetector(CaptureModel* model, QObject* parent)
    : QObject(parent), m_model(model),
      m_program(QStandardPaths::findExecutable(QStringLiteral("zbarimg"))) {
  m_timeout.setSingleShot(true);
  m_timeout.setInterval(kQrTimeoutMs);
  connect(&m_timeout, &QTimer::timeout, &m_process, &QProcess::kill);
  connect(&m_process, &QProcess::finished, this,
          [this](int exitCode, QProcess::ExitStatus status) {
            finishCurrent(status == QProcess::NormalExit, exitCode == 0);
          });
  connect(&m_process, &QProcess::errorOccurred, this,
          [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
              finishCurrent(false, false);
            }
          });
}

QrDetector::~QrDetector() {
  if (m_process.state() != QProcess::NotRunning) {
    m_process.kill();
    m_process.waitForFinished(1000);
  }
}

void QrDetector::inspect(const QString& path) {
  m_path = path;
  m_detected = false;
  m_checking = false;
  m_requested.reset();

  if (!available() || !m_model) {
    emit stateChanged();
    if (m_process.state() != QProcess::NotRunning) {
      m_process.kill();
    }
    return;
  }
  const int row = m_model->rowOf(path);
  if (row < 0 || m_model->recordAt(row).isVideo()) {
    emit stateChanged();
    if (m_process.state() != QProcess::NotRunning) {
      m_process.kill();
    }
    return;
  }

  const auto& record = m_model->recordAt(row);
  const Candidate candidate{record.path, record.modified, record.bytes};
  const auto cached = m_cache.constFind(candidate.path);
  if (cached != m_cache.cend() && cached->modified == candidate.modified &&
      cached->bytes == candidate.bytes) {
    m_detected = cached->detected;
    emit stateChanged();
    if (m_process.state() != QProcess::NotRunning &&
        (!m_current || !sameIdentity(*m_current, candidate))) {
      m_process.kill();
    }
    return;
  }

  if (m_current && sameIdentity(*m_current, candidate)) {
    m_checking = true;
    emit stateChanged();
    return;
  }

  m_requested = candidate;
  m_checking = true;
  emit stateChanged();
  if (m_process.state() != QProcess::NotRunning) {
    m_process.kill();
  } else {
    startRequested();
  }
}

void QrDetector::clear() {
  m_requested.reset();
  m_path.clear();
  m_checking = false;
  m_detected = false;
  emit stateChanged();
  if (m_process.state() != QProcess::NotRunning) {
    m_process.kill();
  }
}

void QrDetector::startRequested() {
  if (!m_requested || m_process.state() != QProcess::NotRunning) {
    return;
  }
  const Candidate candidate = *m_requested;
  m_requested.reset();
  if (!stillCurrent(candidate)) {
    if (candidate.path == m_path) {
      m_checking = false;
      emit stateChanged();
    }
    return;
  }

  m_current = candidate;
  m_process.setProgram(m_program);
  m_process.setArguments({QStringLiteral("-q"), QStringLiteral("--raw"),
                          QStringLiteral("-Sdisable"), QStringLiteral("-Sqrcode.enable"),
                          candidate.path});
  m_process.setProcessChannelMode(QProcess::SeparateChannels);
  m_process.start(QIODevice::ReadOnly);
  m_timeout.start();
}

void QrDetector::finishCurrent(bool completedNormally, bool successful) {
  if (!m_current) {
    QTimer::singleShot(0, this, &QrDetector::startRequested);
    return;
  }
  m_timeout.stop();
  const Candidate candidate = *m_current;
  m_current.reset();

  QByteArray output = m_process.readAllStandardOutput();
  const bool found = completedNormally && successful && !output.trimmed().isEmpty();
  output.fill('\0');
  output.clear();

  if (completedNormally && stillCurrent(candidate)) {
    m_cache.insert(candidate.path, {candidate.modified, candidate.bytes, found});
  }
  if (candidate.path == m_path && stillCurrent(candidate)) {
    m_detected = found;
    m_checking = false;
    emit stateChanged();
  }
  QTimer::singleShot(0, this, &QrDetector::startRequested);
}

bool QrDetector::stillCurrent(const Candidate& candidate) const {
  if (!m_model) {
    return false;
  }
  const int row = m_model->rowOf(candidate.path);
  if (row < 0) {
    return false;
  }
  const auto& record = m_model->recordAt(row);
  return !record.isVideo() && record.modified == candidate.modified &&
         record.bytes == candidate.bytes;
}

bool QrDetector::sameIdentity(const Candidate& left, const Candidate& right) {
  return left.path == right.path && left.modified == right.modified && left.bytes == right.bytes;
}
