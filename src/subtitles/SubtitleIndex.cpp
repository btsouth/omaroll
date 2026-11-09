#include "subtitles/SubtitleIndex.h"

#include "sources/FileVersion.h"

#include <QDir>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QtNumeric>

#include <algorithm>

namespace {

// Milliseconds from "HH:MM:SS,mmm", "HH:MM:SS.mmm", "MM:SS.mmm" or "SS.mmm";
// the hour and the millisecond fraction are both optional. -1 when unparsable.
qint64 parseTimestamp(const QString& value) {
  QString token = value.trimmed();
  token.replace(QLatin1Char(','), QLatin1Char('.'));
  const QStringList dot = token.split(QLatin1Char('.'));
  if (dot.isEmpty()) {
    return -1;
  }
  qint64 milliseconds = 0;
  if (dot.size() > 1) {
    const QString fraction = dot.at(1).leftJustified(3, QLatin1Char('0')).left(3);
    bool ok = false;
    milliseconds = fraction.toInt(&ok);
    if (!ok) {
      milliseconds = 0;
    }
  }
  const QStringList parts = dot.first().split(QLatin1Char(':'));
  if (parts.size() > 3) {
    return -1;
  }
  // A malformed file can name any number of hours; one past what milliseconds
  // can hold is unparsable rather than an overflow.
  qint64 seconds = 0;
  for (const QString& part : parts) {
    if (qMulOverflow(seconds, qint64(60), &seconds) ||
        qAddOverflow(seconds, part.toLongLong(), &seconds)) {
      return -1;
    }
  }
  qint64 result = 0;
  if (qMulOverflow(seconds, qint64(1000), &result) || qAddOverflow(result, milliseconds, &result)) {
    return -1;
  }
  return result;
}

// Drop the inline markup both formats allow, plus the brace tags ASS leaves
// behind when a file is converted without stripping them.
QString stripMarkup(QString text) {
  text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
  text.remove(QRegularExpression(QStringLiteral(R"(\{[^}]*\})")));
  return text.trimmed();
}

QList<SubtitleIndex::Cue> parseSrt(const QStringList& lines) {
  QList<SubtitleIndex::Cue> cues;
  int i = 0;
  while (i < lines.size()) {
    while (i < lines.size() && lines.at(i).trimmed().isEmpty()) {
      ++i;
    }
    if (i >= lines.size()) {
      break;
    }
    if (!lines.at(i).contains(QLatin1String("-->"))) {
      ++i; // the ordinal line
      while (i < lines.size() && lines.at(i).trimmed().isEmpty()) {
        ++i;
      }
      if (i >= lines.size()) {
        break;
      }
    }
    const QString timing = lines.at(i);
    const qsizetype arrow = timing.indexOf(QLatin1String("-->"));
    if (arrow < 0) {
      ++i;
      continue;
    }
    const qint64 start = parseTimestamp(timing.left(arrow));
    const qint64 end =
        parseTimestamp(timing.mid(arrow + 3).trimmed().section(QLatin1Char(' '), 0, 0));
    if (start < 0) {
      ++i;
      continue;
    }
    ++i;
    QStringList body;
    while (i < lines.size() && !lines.at(i).trimmed().isEmpty()) {
      body.append(lines.at(i));
      ++i;
    }
    SubtitleIndex::Cue cue;
    cue.start = start;
    cue.end = end >= start ? end : start;
    cue.text = stripMarkup(body.join(QLatin1Char('\n')));
    if (!cue.text.isEmpty()) {
      cues.append(cue);
    }
  }
  return cues;
}

QList<SubtitleIndex::Cue> parseVtt(const QStringList& lines) {
  QList<SubtitleIndex::Cue> cues;
  int i = 0;
  if (i < lines.size() && lines.at(i).startsWith(QLatin1String("WEBVTT"))) {
    ++i;
  }
  while (i < lines.size()) {
    const QString line = lines.at(i).trimmed();
    if (line.isEmpty()) {
      ++i;
      continue;
    }
    if (line.startsWith(QLatin1String("NOTE")) || line.startsWith(QLatin1String("STYLE")) ||
        line.startsWith(QLatin1String("REGION"))) {
      while (i < lines.size() && !lines.at(i).trimmed().isEmpty()) {
        ++i;
      }
      continue;
    }
    if (!line.contains(QLatin1String("-->"))) {
      ++i; // a cue identifier
      continue;
    }
    const qsizetype arrow = line.indexOf(QLatin1String("-->"));
    const qint64 start = parseTimestamp(line.left(arrow));
    const qint64 end =
        parseTimestamp(line.mid(arrow + 3).trimmed().section(QLatin1Char(' '), 0, 0));
    if (start < 0) {
      ++i;
      continue;
    }
    ++i;
    QStringList body;
    while (i < lines.size() && !lines.at(i).trimmed().isEmpty()) {
      body.append(lines.at(i));
      ++i;
    }
    SubtitleIndex::Cue cue;
    cue.start = start;
    cue.end = end >= start ? end : start;
    cue.text = stripMarkup(body.join(QLatin1Char('\n')));
    if (!cue.text.isEmpty()) {
      cues.append(cue);
    }
  }
  return cues;
}

QString friendlyLanguage(const QString& tag) {
  static const QHash<QString, QString> names = {
      {QStringLiteral("EN"), QStringLiteral("English")},
      {QStringLiteral("ES"), QStringLiteral("Spanish")},
      {QStringLiteral("FR"), QStringLiteral("French")},
      {QStringLiteral("DE"), QStringLiteral("German")},
      {QStringLiteral("IT"), QStringLiteral("Italian")},
      {QStringLiteral("PT"), QStringLiteral("Portuguese")},
      {QStringLiteral("NL"), QStringLiteral("Dutch")},
      {QStringLiteral("PL"), QStringLiteral("Polish")},
      {QStringLiteral("RU"), QStringLiteral("Russian")},
      {QStringLiteral("JA"), QStringLiteral("Japanese")},
      {QStringLiteral("KO"), QStringLiteral("Korean")},
      {QStringLiteral("ZH"), QStringLiteral("Chinese")},
      {QStringLiteral("AR"), QStringLiteral("Arabic")},
  };
  return names.value(tag, tag);
}

} // namespace

SubtitleIndex::SubtitleIndex(QObject* parent) : QObject(parent) {
  connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &SubtitleIndex::invalidate);
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
          [this](const QString& directory) {
    const auto paths = m_cache.keys() + m_pending.keys();
    for (const QString& path : paths) {
      if (QFileInfo(path).absolutePath() == directory) {
        invalidate(path);
      }
    }
  });
}

void SubtitleIndex::invalidate(const QString& path) {
  m_cache.remove(path);
  m_pending.remove(path);
  ++m_revision;
  emit changed();
}

QStringList SubtitleIndex::files(const QString& videoPath) {
  const QFileInfo video(videoPath);
  if (!video.isFile()) {
    return {};
  }
  const QString base = video.completeBaseName();
  if (base.isEmpty()) {
    return {};
  }
  QDir directory(video.absolutePath());
  const QStringList names = directory.entryList(QDir::Files | QDir::Readable);
  QStringList found;
  for (const QString& name : names) {
    const QString lower = name.toLower();
    const bool subtitle = lower.endsWith(QLatin1String(".srt")) ||
                          lower.endsWith(QLatin1String(".vtt"));
    if (!subtitle || !name.startsWith(base, Qt::CaseInsensitive)) {
      continue;
    }
    // "movie2.srt" must not be taken for a sidecar of "movie"; only a dot (or
    // an exact match, impossible given the extension) may follow the base.
    if (name.size() <= base.size() || name.at(base.size()) != QLatin1Char('.')) {
      continue;
    }
    found.append(directory.absoluteFilePath(name));
  }
  found.sort(Qt::CaseInsensitive);
  return found;
}

QString SubtitleIndex::label(const QString& subtitlePath) const {
  const QString stem = QFileInfo(subtitlePath).completeBaseName();
  const qsizetype dot = stem.lastIndexOf(QLatin1Char('.'));
  if (dot >= 0 && dot + 1 < stem.size()) {
    return friendlyLanguage(stem.mid(dot + 1).toUpper());
  }
  return QStringLiteral("Subtitles");
}

const SubtitleIndex::Parsed& SubtitleIndex::cuesFor(const QString& path) {
  static const Parsed empty;
  const auto found = m_cache.constFind(path);
  if (found != m_cache.cend()) {
    return *found;
  }
  if (!m_pending.contains(path)) {
    const quint64 generation = ++m_generation;
    m_pending.insert(path, generation);
    const QString directory = QFileInfo(path).absolutePath();
    if (!m_watcher.directories().contains(directory)) m_watcher.addPath(directory);
    if (!m_watcher.files().contains(path)) m_watcher.addPath(path);
    auto* watcher = new QFutureWatcher<Parsed>(this);
    connect(watcher, &QFutureWatcher<Parsed>::finished, this, [this, watcher, path, generation] {
      if (m_pending.value(path) == generation) {
        m_pending.remove(path);
        m_cache.insert(path, watcher->result());
        ++m_revision;
        emit changed();
      }
      watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([path] { return parse(path); }));
  }
  return empty;
}

SubtitleIndex::Parsed SubtitleIndex::parse(const QString& path) {
  Parsed parsed;
  const QFileInfo info(path);
  if (!info.isFile() || info.size() <= 0 || info.size() > kMaximumFileBytes) {
    return parsed;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return parsed;
  }
  const QByteArray bytes = file.read(kMaximumFileBytes + 1);
  if (bytes.size() > kMaximumFileBytes) {
    return parsed;
  }
  QString text = QString::fromUtf8(bytes);
  if (!text.isEmpty() && text.at(0) == QChar(0xFEFF)) {
    text.remove(0, 1);
  }
  text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
  text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
  const QStringList lines = text.split(QLatin1Char('\n'));
  const bool webvtt = info.suffix().compare(QLatin1String("vtt"), Qt::CaseInsensitive) == 0 ||
                      (!lines.isEmpty() && lines.first().startsWith(QLatin1String("WEBVTT")));
  parsed.cues = webvtt ? parseVtt(lines) : parseSrt(lines);
  std::stable_sort(parsed.cues.begin(), parsed.cues.end(),
                   [](const Cue& a, const Cue& b) { return a.start < b.start; });
  while (parsed.leaves < parsed.cues.size()) {
    parsed.leaves *= 2;
  }
  parsed.maximumEnds.fill(-1, parsed.leaves * 2);
  for (int i = 0; i < parsed.cues.size(); ++i) {
    parsed.maximumEnds[parsed.leaves + i] = parsed.cues.at(i).end;
  }
  for (int i = parsed.leaves - 1; i > 0; --i) {
    parsed.maximumEnds[i] = std::max(parsed.maximumEnds.at(i * 2), parsed.maximumEnds.at(i * 2 + 1));
  }
  return parsed;
}

void SubtitleIndex::collect(const Parsed& parsed, int node, int begin, int end,
                            qint64 position, QStringList& active) {
  if (begin >= parsed.cues.size() || parsed.maximumEnds.at(node) < position ||
      parsed.cues.at(begin).start > position) {
    return;
  }
  if (end - begin == 1) {
    active.append(parsed.cues.at(begin).text);
    return;
  }
  const int middle = begin + (end - begin) / 2;
  collect(parsed, node * 2, begin, middle, position, active);
  collect(parsed, node * 2 + 1, middle, end, position, active);
}

QString SubtitleIndex::textAt(const QString& subtitlePath, qint64 positionMs) {
  if (subtitlePath.isEmpty() || positionMs < 0) {
    return {};
  }
  const Parsed& parsed = cuesFor(subtitlePath);
  QStringList active;
  if (!parsed.cues.isEmpty()) {
    collect(parsed, 1, 0, parsed.leaves, positionMs, active);
  }
  return active.join(QLatin1Char('\n'));
}

int SubtitleIndex::cueCount(const QString& subtitlePath) {
  return static_cast<int>(cuesFor(subtitlePath).cues.size());
}

QString SubtitleIndex::embeddedState(const QString& videoPath, int track) const {
  const auto found = m_extractions.constFind(embeddedKey(videoPath, track));
  return found == m_extractions.cend() ? QString() : found->state;
}

QString SubtitleIndex::embedded(const QString& videoPath, int track) {
  if (videoPath.isEmpty() || track < 0) return {};
  const QString key = embeddedKey(videoPath, track);
  const QString version = FileVersion::key(videoPath);
  const auto found = m_extractions.constFind(key);
  if (found != m_extractions.cend() && found->version == version) {
    return found->state == u"ready" ? found->path : QString();
  }
  const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
  if (!m_extracted) m_extracted = std::make_unique<QTemporaryDir>();
  if (ffmpeg.isEmpty() || !m_extracted->isValid()) {
    m_extractions.insert(key, {version, QStringLiteral("failed"), {}});
    // Called from bindings: say so on the next turn, not inside this one.
    QMetaObject::invokeMethod(this, [this] {
      ++m_revision;
      emit changed();
    }, Qt::QueuedConnection);
    return {};
  }
  const QString output = m_extracted->filePath(QStringLiteral("%1.srt").arg(++m_extractedCount));
  m_extractions.insert(key, {version, QStringLiteral("working"), output});
  auto* watcher = new QFutureWatcher<bool>(this);
  connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher, key, version, output] {
    const auto current = m_extractions.find(key);
    if (current != m_extractions.end() && current->version == version && current->path == output) {
      current->state = watcher->result() ? QStringLiteral("ready") : QStringLiteral("failed");
      ++m_revision;
      emit changed();
    }
    watcher->deleteLater();
  });
  watcher->setFuture(QtConcurrent::run([ffmpeg, videoPath, track, output] {
    // Text tracks only: a picture-based one (PGS, VobSub) has no text to
    // write, and ffmpeg refuses.
    const QString partial = output + QStringLiteral(".part");
    QProcess process;
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-v"),
                           QStringLiteral("error"), QStringLiteral("-y"), QStringLiteral("-i"), videoPath,
                           QStringLiteral("-map"), QStringLiteral("0:s:%1").arg(track), QStringLiteral("-c:s"),
                           QStringLiteral("srt"), QStringLiteral("-f"), QStringLiteral("srt"), partial});
    if (!process.waitForStarted(5000)) return false;
    if (!process.waitForFinished(120000)) {
      process.kill();
      process.waitForFinished(1000);
      QFile::remove(partial);
      return false;
    }
    const bool written = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0
                         && QFileInfo(partial).isFile();
    if (!written) {
      QFile::remove(partial);
      return false;
    }
    QFile::remove(output);
    return QFile::rename(partial, output);
  }));
  return {};
}
