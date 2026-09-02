#include "library/MediaInspector.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <cmath>

namespace {

constexpr int kInspectTimeoutMs = 5000;

QString present(QString value) {
  value = value.trimmed();
  return value.compare(QStringLiteral("undefined"), Qt::CaseInsensitive) == 0
             ? QString()
             : value;
}

double number(const QString& value, bool* okay = nullptr) {
  const QStringList parts = value.trimmed().split(QLatin1Char('/'));
  bool numeratorOkay = false;
  const double numerator = parts.value(0).toDouble(&numeratorOkay);
  bool denominatorOkay = true;
  const double denominator =
      parts.size() == 2 ? parts.at(1).toDouble(&denominatorOkay) : 1.0;
  const bool valid = numeratorOkay && denominatorOkay && denominator != 0;
  if (okay) {
    *okay = valid;
  }
  return valid ? numerator / denominator : 0;
}

QString compactNumber(double value, int precision = 1) {
  return QLocale::c().toString(value, 'f',
                               std::abs(value - std::round(value)) < 0.001 ? 0 : precision);
}

QString codecLabel(QString codec) {
  codec = codec.toLower();
  if (codec == QStringLiteral("h264")) {
    return QStringLiteral("H.264");
  }
  if (codec == QStringLiteral("hevc")) {
    return QStringLiteral("HEVC");
  }
  if (codec == QStringLiteral("av1")) {
    return QStringLiteral("AV1");
  }
  if (codec == QStringLiteral("vp9")) {
    return QStringLiteral("VP9");
  }
  if (codec == QStringLiteral("aac")) {
    return QStringLiteral("AAC");
  }
  if (codec == QStringLiteral("opus")) {
    return QStringLiteral("Opus");
  }
  return codec.toUpper();
}

QString joined(const QStringList& parts) {
  QStringList kept;
  for (const QString& part : parts) {
    if (!part.isEmpty()) {
      kept.append(part);
    }
  }
  return kept.join(QStringLiteral("  ·  "));
}

} // namespace

MediaInspector::MediaInspector(QObject* parent) : QObject(parent) {}

MediaInspector::~MediaInspector() {
  if (m_process && m_process->state() != QProcess::NotRunning) {
    m_process->kill();
    m_process->waitForFinished(1000);
  }
}

void MediaInspector::cancelCurrent() {
  if (!m_process) {
    return;
  }
  QProcess* previous = m_process;
  m_process = nullptr;
  disconnect(previous, nullptr, this, nullptr);
  if (previous->state() != QProcess::NotRunning) {
    previous->kill();
  }
  connect(previous, &QProcess::finished, previous, &QObject::deleteLater);
  if (previous->state() == QProcess::NotRunning) {
    previous->deleteLater();
  }
}

void MediaInspector::setLoading(bool loading) {
  if (m_loading == loading) {
    return;
  }
  m_loading = loading;
  emit loadingChanged();
}

void MediaInspector::inspect(const QString& path, bool video) {
  ++m_generation;
  cancelCurrent();

  const bool changed = m_path != path || !m_lines.isEmpty();
  m_path = path;
  m_lines.clear();
  if (changed) {
    emit detailsChanged();
  }

  if (path.isEmpty() || !QFileInfo(path).isFile()) {
    setLoading(false);
    return;
  }

  const QString program = QStandardPaths::findExecutable(
      video ? QStringLiteral("ffprobe") : QStringLiteral("magick"));
  if (program.isEmpty()) {
    setLoading(false);
    return;
  }

  auto* process = new QProcess(this);
  m_process = process;
  const quint64 generation = m_generation;
  const QString suffix = QFileInfo(path).suffix();
  process->setProcessChannelMode(QProcess::SeparateChannels);
  setLoading(true);

  connect(process, &QProcess::finished, this,
          [this, process, generation, video, suffix](int exitCode,
                                                     QProcess::ExitStatus status) {
            const bool current = m_process == process && generation == m_generation;
            if (current) {
              m_process = nullptr;
              if (status == QProcess::NormalExit && exitCode == 0) {
                m_lines = video ? parseVideo(process->readAllStandardOutput(), suffix)
                                : parseImage(process->readAllStandardOutput());
                emit detailsChanged();
              }
              setLoading(false);
            }
            process->deleteLater();
          });
  connect(process, &QProcess::errorOccurred, this,
          [this, process, generation](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart && m_process == process &&
                generation == m_generation) {
              m_process = nullptr;
              setLoading(false);
              process->deleteLater();
            }
          });

  if (video) {
    process->start(program,
                   {QStringLiteral("-v"), QStringLiteral("error"),
                    QStringLiteral("-show_entries"),
                    QStringLiteral("format=format_name,bit_rate:stream="
                                   "codec_type,codec_name,profile,pix_fmt,"
                                   "r_frame_rate,bit_rate,sample_rate,channel_layout"),
                    QStringLiteral("-of"), QStringLiteral("json"), path});
  } else {
    // Fixed fields and fixed order make this independent of locale and of the
    // filename. Missing EXIF properties are empty lines under -quiet.
    const QString format = QStringLiteral(
        "%m\n%[EXIF:DateTimeOriginal]\n%[EXIF:DateTimeDigitized]\n"
        "%[EXIF:Make]\n%[EXIF:Model]\n%[EXIF:LensModel]\n"
        "%[EXIF:FNumber]\n%[EXIF:ExposureTime]\n%[EXIF:ISOSpeedRatings]\n"
        "%[EXIF:FocalLength]\n%z\n%[colorspace]\n");
    process->start(program, {QStringLiteral("identify"), QStringLiteral("-ping"),
                             QStringLiteral("-quiet"),
                             QStringLiteral("-format"), format, path});
  }
  QTimer::singleShot(kInspectTimeoutMs, process, [process] {
    if (process->state() != QProcess::NotRunning) {
      process->kill();
    }
  });
}

QStringList MediaInspector::parseImage(const QByteArray& output) {
  const QStringList fields =
      QString::fromUtf8(output).split(QLatin1Char('\n'), Qt::KeepEmptyParts);
  if (fields.isEmpty()) {
    return {};
  }

  QStringList lines;
  const QString format = present(fields.value(0)).toUpper();
  const QString depth = present(fields.value(10));
  const QString colorSpace = present(fields.value(11));
  const QString formatLine = joined(
      {format, colorSpace, depth.isEmpty() ? QString() : depth + QStringLiteral("-bit")});
  if (!formatLine.isEmpty()) {
    lines.append(formatLine);
  }

  QString taken = present(fields.value(1));
  if (taken.isEmpty()) {
    taken = present(fields.value(2));
  }
  const QDateTime date =
      QDateTime::fromString(taken, QStringLiteral("yyyy:MM:dd HH:mm:ss"));
  if (date.isValid()) {
    lines.append(QStringLiteral("Taken  ·  ") +
                 QLocale::system().toString(date, QLocale::ShortFormat));
  }

  const QString make = present(fields.value(3));
  const QString model = present(fields.value(4));
  QString camera = model;
  if (!make.isEmpty() && !model.startsWith(make, Qt::CaseInsensitive)) {
    camera = make + QLatin1Char(' ') + model;
  } else if (camera.isEmpty()) {
    camera = make;
  }
  if (!camera.isEmpty()) {
    lines.append(QStringLiteral("Camera  ·  ") + camera.simplified());
  }

  const QString lens = present(fields.value(5));
  if (!lens.isEmpty()) {
    lines.append(QStringLiteral("Lens  ·  ") + lens);
  }

  QStringList exposure;
  bool okay = false;
  const double aperture = number(present(fields.value(6)), &okay);
  if (okay && aperture > 0) {
    exposure.append(QStringLiteral("f/") + compactNumber(aperture));
  }
  const QString exposureTime = present(fields.value(7));
  if (!exposureTime.isEmpty()) {
    exposure.append(exposureTime + QStringLiteral(" s"));
  }
  const QString iso = present(fields.value(8));
  if (!iso.isEmpty()) {
    exposure.append(QStringLiteral("ISO ") + iso);
  }
  const double focalLength = number(present(fields.value(9)), &okay);
  if (okay && focalLength > 0) {
    exposure.append(compactNumber(focalLength) + QStringLiteral(" mm"));
  }
  if (!exposure.isEmpty()) {
    lines.append(exposure.join(QStringLiteral("  ·  ")));
  }
  return lines;
}

QStringList MediaInspector::parseVideo(const QByteArray& output, const QString& suffix) {
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(output, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    return {};
  }

  const QJsonObject root = document.object();
  const QJsonObject format = root.value(QStringLiteral("format")).toObject();
  QJsonObject video;
  QJsonObject audio;
  for (const QJsonValue& value : root.value(QStringLiteral("streams")).toArray()) {
    const QJsonObject stream = value.toObject();
    const QString type = stream.value(QStringLiteral("codec_type")).toString();
    if (type == QStringLiteral("video") && video.isEmpty()) {
      video = stream;
    } else if (type == QStringLiteral("audio") && audio.isEmpty()) {
      audio = stream;
    }
  }

  QStringList lines;
  const QString container = suffix.isEmpty()
                                ? format.value(QStringLiteral("format_name")).toString()
                                      .section(QLatin1Char(','), 0, 0).toUpper()
                                : suffix.toUpper();
  const QString codec =
      codecLabel(video.value(QStringLiteral("codec_name")).toString());
  const QString profile = present(video.value(QStringLiteral("profile")).toString());
  const QString pixels = present(video.value(QStringLiteral("pix_fmt")).toString());
  const QString videoLine = joined({container, codec, profile, pixels});
  if (!videoLine.isEmpty()) {
    lines.append(videoLine);
  }

  QStringList rate;
  bool okay = false;
  const double frames =
      number(video.value(QStringLiteral("r_frame_rate")).toString(), &okay);
  if (okay && frames > 0) {
    rate.append(compactNumber(frames, 2) + QStringLiteral(" fps"));
  }
  QString bitRate = format.value(QStringLiteral("bit_rate")).toString();
  if (bitRate.isEmpty()) {
    bitRate = video.value(QStringLiteral("bit_rate")).toString();
  }
  const double bits = bitRate.toDouble(&okay);
  if (okay && bits > 0) {
    rate.append(compactNumber(bits / 1'000'000.0) + QStringLiteral(" Mbps"));
  }
  if (!rate.isEmpty()) {
    lines.append(rate.join(QStringLiteral("  ·  ")));
  }

  if (!audio.isEmpty()) {
    QString sampleRate;
    const double samples =
        audio.value(QStringLiteral("sample_rate")).toString().toDouble(&okay);
    if (okay && samples > 0) {
      sampleRate = compactNumber(samples / 1000.0) + QStringLiteral(" kHz");
    }
    const QString audioLine = joined(
        {codecLabel(audio.value(QStringLiteral("codec_name")).toString()),
         present(audio.value(QStringLiteral("channel_layout")).toString()), sampleRate});
    if (!audioLine.isEmpty()) {
      lines.append(audioLine);
    }
  }
  return lines;
}
