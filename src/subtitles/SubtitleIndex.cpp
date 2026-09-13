#include "subtitles/SubtitleIndex.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

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
  qint64 seconds = 0;
  if (parts.size() == 3) {
    seconds = parts.at(0).toLongLong() * 3600 + parts.at(1).toLongLong() * 60 +
              parts.at(2).toLongLong();
  } else if (parts.size() == 2) {
    seconds = parts.at(0).toLongLong() * 60 + parts.at(1).toLongLong();
  } else if (parts.size() == 1) {
    seconds = parts.at(0).toLongLong();
  } else {
    return -1;
  }
  return seconds * 1000 + milliseconds;
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

SubtitleIndex::SubtitleIndex(QObject* parent) : QObject(parent) {}

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

const QList<SubtitleIndex::Cue>& SubtitleIndex::cuesFor(const QString& path) {
  const QFileInfo info(path);
  Parsed& parsed = m_cache[path];
  const qint64 modified = info.lastModified().toMSecsSinceEpoch();
  if (parsed.valid && parsed.modified == modified && parsed.bytes == info.size()) {
    return parsed.cues;
  }

  parsed = Parsed{};
  parsed.modified = modified;
  parsed.bytes = info.size();
  parsed.valid = true;

  if (!info.isFile() || info.size() <= 0 || info.size() > kMaximumFileBytes) {
    return parsed.cues;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return parsed.cues;
  }
  QString text = QString::fromUtf8(file.readAll());
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
  return parsed.cues;
}

QString SubtitleIndex::textAt(const QString& subtitlePath, qint64 positionMs) {
  if (subtitlePath.isEmpty() || positionMs < 0) {
    return {};
  }
  const QList<Cue>& cues = cuesFor(subtitlePath);
  QStringList active;
  for (const Cue& cue : cues) {
    if (cue.start > positionMs) {
      break;
    }
    if (positionMs <= cue.end) {
      active.append(cue.text);
    }
  }
  return active.join(QLatin1Char('\n'));
}

int SubtitleIndex::cueCount(const QString& subtitlePath) {
  return static_cast<int>(cuesFor(subtitlePath).size());
}
