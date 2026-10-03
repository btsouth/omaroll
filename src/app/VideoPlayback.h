#pragma once

#include <QProcessEnvironment>
#include <QFile>
#include <QLibrary>
#include <utility>

// Qt's FFmpeg backend probes both decode and encode hardware lists at startup.
// A viewer needs decoding, not recording; avoid the general Linux device scan.
// These documented Qt knobs are private API and need rechecking on Qt upgrades.
class VideoPlaybackEnvironment {
public:
  explicit VideoPlaybackEnvironment(QProcessEnvironment inherited)
      : m_inherited(std::move(inherited)) {}

  static bool cudaDriverAvailable(
      const QString& marker = QStringLiteral("/proc/driver/nvidia/version"),
      const QString& libraryName = QStringLiteral("libnvcuvid.so")) {
#ifdef Q_OS_LINUX
    // Match Qt's CUDA precheck (QTBUG-122199). Explicit device lists bypass it.
    if (!QFile::exists(marker)) return false;
    QLibrary library(libraryName);
    const bool available = library.load();
    if (available) library.unload();
    return available;
#else
    return false;
#endif
  }

  QProcessEnvironment playbackEnvironment(bool cudaAvailable) const {
    auto environment = m_inherited;
#ifdef Q_OS_LINUX
    if (!environment.contains(decodingKey)) {
      environment.insert(decodingKey, cudaAvailable ? QStringLiteral("cuda,vaapi")
                                                   : QStringLiteral("vaapi"));
    }
    if (!environment.contains(encodingKey)) {
      environment.insert(encodingKey, QStringLiteral(","));
    }
#endif
    return environment;
  }

  QProcessEnvironment externalEnvironment(QProcessEnvironment environment) const {
#ifdef Q_OS_LINUX
    // Editors and desktop helpers inherit the caller's choices, not ours.
    for (const auto& key : {decodingKey, encodingKey}) {
      if (m_inherited.contains(key)) {
        environment.insert(key, m_inherited.value(key));
      } else {
        environment.remove(key);
      }
    }
#endif
    return environment;
  }

  void apply() const {
#ifdef Q_OS_LINUX
    const bool cudaAvailable = !m_inherited.contains(decodingKey) && cudaDriverAvailable();
    const auto environment = playbackEnvironment(cudaAvailable);
    qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", environment.value(decodingKey).toUtf8());
    qputenv("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES", environment.value(encodingKey).toUtf8());
#endif
  }

private:
  inline static const QString decodingKey = QStringLiteral("QT_FFMPEG_DECODING_HW_DEVICE_TYPES");
  inline static const QString encodingKey = QStringLiteral("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES");
  QProcessEnvironment m_inherited;
};

inline const VideoPlaybackEnvironment& videoPlaybackEnvironment() {
  // Capture before the application's defaults are applied or workers start.
  static const VideoPlaybackEnvironment environment(QProcessEnvironment::systemEnvironment());
  return environment;
}

inline QProcessEnvironment externalProcessEnvironment() {
  return videoPlaybackEnvironment().externalEnvironment(QProcessEnvironment::systemEnvironment());
}

inline void configureVideoPlayback() {
  static const bool configured = [] {
    videoPlaybackEnvironment().apply();
    return true;
  }();
  Q_UNUSED(configured);
}
