#include "app/AppSettings.h"
#include "app/LaunchRequest.h"
#include "app/HyprlandSetup.h"
#include "app/SingleInstance.h"
#include "library/MediaMetadataIndex.h"
#include "search/DocumentTextIndex.h"
#include "search/SearchQuery.h"
#include "thumbs/Storyboard.h"
#include "sources/CameraRaw.h"
#include "sources/ImageBudget.h"
#include "subtitles/SubtitleIndex.h"
#include "pdf/PdfSupport.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QBuffer>
#include <QImageReader>
#include <cstdlib>

void fuzzMatroska(const QByteArray&);
namespace {
struct Environment {
  int argc = 1;
  char name[8] = "fuzzer";
  char* argv[2] = {name, nullptr};
  QCoreApplication app{argc, argv};
  QTemporaryDir dir;
  Environment() {
    if (!dir.isValid()) std::abort();
    qputenv("XDG_DATA_HOME", dir.path().toUtf8());
    qputenv("XDG_CACHE_HOME", dir.filePath(QStringLiteral("cache")).toUtf8());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    // Warnings about rejected input are expected on nearly every run and
    // only slow the fuzzer down. Crashes and sanitizer reports still print.
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString&) {});
  }
};
}
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static Environment env;
  const QByteArray bytes(reinterpret_cast<const char*>(data), qsizetype(size));
#if FUZZ_KIND == 1
  (void)LaunchRequest::parseStart(QString::fromUtf8(bytes));
#elif FUZZ_KIND == 2
  QStringList paths, skipped; bool library; qint64 start;
  (void)SingleInstance::parseFrame(bytes, paths, library, start, skipped);
#elif FUZZ_KIND == 3
  fuzzMatroska(bytes);
#elif FUZZ_KIND == 4
  (void)PdfSupport::parsePageWords(bytes);
#elif FUZZ_KIND == 5
  QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer);
  const ImageBudget::Estimate estimate = ImageBudget::estimate(reader);
  // The header's dimensions feed the memory arithmetic, so run that too.
  (void)ImageBudget::plan(estimate, 2560);
  (void)ImageBudget::bytesAt(estimate, estimate.stored / 3);
  (void)ImageBudget::decodedSize(estimate, estimate.stored / 3);
#elif FUZZ_KIND == 9
  // The search box: field tokens, quotes, escapes and exclusions.
  const QString text = QString::fromUtf8(bytes);
  const SearchQuery query = SearchQuery::parse(text);
  (void)query.hasText();
  (void)query.textValues(true);
  (void)SearchQuery::parse(SearchQuery::fromPlainWords(query.plainWords()));
  for (const SearchQuery::Term& term : query.terms) {
    // A value written as a token reads back as itself.
    const QString quoted = SearchQuery::quote(term.typed);
    if (quoted.startsWith(QLatin1Char('"'))) {
      qsizetype next = 0;
      bool closed = false;
      if (SearchQuery::readQuoted(quoted, 1, &next, &closed) != term.typed || !closed || next != quoted.size())
        std::abort();
    }
  }
  for (qsizetype at = 0; at < text.size(); at += 7) {
    qsizetype next = 0;
    (void)SearchQuery::readQuoted(text, at, &next);
    if (next < at || next > text.size()) std::abort();
  }
#elif FUZZ_KIND == 10
  StoryboardCache::fuzzLoad(bytes);
#elif FUZZ_KIND == 12
  // The quick EXIF date reader must agree with the general one.
  const QString value = QString::fromUtf8(bytes);
  if (MediaMetadataIndex::parseExifDate(value) != MediaMetadataIndex::parseExifDateGeneral(value)) std::abort();
#elif FUZZ_KIND == 13
  // hyprland.lua, then bindings.lua, then Omarchy's default bindings,
  // separated by NUL bytes. Install and remove leave hyprland.lua as it was.
  const QList<QByteArray> parts = bytes.split('\0');
  QDir root(env.dir.filePath(QStringLiteral("hyprland")));
  root.removeRecursively();
  const QString hypr = root.filePath(QStringLiteral("hypr"));
  const QString omarchy = root.filePath(QStringLiteral("omarchy"));
  if (!QDir().mkpath(hypr) || !QDir().mkpath(omarchy + QStringLiteral("/default/hypr/bindings"))) std::abort();
  const auto put = [](const QString& path, const QByteArray& content) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size()) std::abort();
  };
  const QString config = hypr + QStringLiteral("/hyprland.lua");
  put(config, parts.value(0));
  if (parts.size() > 1) put(hypr + QStringLiteral("/bindings.lua"), parts.at(1));
  if (parts.size() > 2) put(omarchy + QStringLiteral("/default/hypr/bindings/utilities.lua"), parts.at(2));
  (void)HyprlandSetup::screenshotKey(hypr, omarchy);
  (void)HyprlandSetup::rulesState(hypr);
  // Removal also drops a line of the user's own that only requires the rules,
  // so the round trip is exact only for a config without one.
  if (HyprlandSetup::installRules(hypr).ok && !parts.value(0).contains("omaroll")) {
    if (HyprlandSetup::rulesState(hypr) != HyprlandSetup::RulesState::Installed) std::abort();
    if (HyprlandSetup::removeRules(hypr).ok) {
      QFile file(config);
      if (!file.open(QIODevice::ReadOnly) || file.readAll() != parts.value(0)) std::abort();
    }
  }
  root.removeRecursively();
#else
  const QString path = env.dir.filePath(QStringLiteral("input.raw"));
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) std::abort();
  file.close();
#if FUZZ_KIND == 6
  (void)CameraRaw::readMetadata(path);
#elif FUZZ_KIND == 7
  SubtitleIndex::fuzzParse(path);
#elif FUZZ_KIND == 8
  // Each input starts from an empty profile and backups folder, so a finding
  // reproduces from its own input alone.
  QDir profile(env.dir.filePath(QStringLiteral("omaroll")));
  profile.removeRecursively();
  {
    AppSettings settings;
    (void)settings.importOrganization(path);
  }
  profile.removeRecursively();
#elif FUZZ_KIND == 11
  DocumentTextIndex::fuzzReadCache(path);
#endif
#endif
  return 0;
}
