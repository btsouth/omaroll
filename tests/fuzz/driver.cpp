#include "app/AppSettings.h"
#include "app/LaunchRequest.h"
#include "app/HyprlandSetup.h"
#include "app/MimeDefaults.h"
#include "edit/JpegExif.h"
#include "library/HashCache.h"
#include "viewer/PictureDiagnosis.h"
#include "app/SingleInstance.h"
#include "library/MediaMetadataIndex.h"
#include "search/DocumentTextIndex.h"
#include "search/SearchQuery.h"
#include "thumbs/Storyboard.h"
#include "sources/CameraRaw.h"
#include "sources/ImageBudget.h"
#include "sources/PinnedImageReader.h"
#include "subtitles/SubtitleIndex.h"
#include "pdf/PdfSupport.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QBuffer>
#include <QImageReader>
#include <cstdlib>

void fuzzMatroska(const QByteArray&);
void fuzzWebp(const QString&);
namespace {
const QString kOmarollDesktop = QStringLiteral("io.github.tsouth89.omaroll.desktop");

QString configHome() { return qEnvironmentVariable("XDG_CONFIG_HOME"); }

// xdg-mime 1.2's make_default_generic, byte for byte: its awk script run on
// $XDG_CONFIG_HOME/mimeapps.list.
QByteArray xdgMimeDefault(const QByteArray& in, const QByteArray& type, const QByteArray& app) {
  QList<QByteArray> records = in.split('\n');
  if (in.isEmpty() || in.endsWith('\n')) records.removeLast();
  const QByteArray prefix = type + '=';
  QByteArray out;
  const auto print = [&out](const QByteArray& line) { out += line + '\n'; };
  bool inDefault = false, added = false, found = false;
  int blanks = 0;
  for (QByteArray line : std::as_const(records)) {
    bool suppress = false;
    if (line.startsWith("[Default Applications]")) {
      inDefault = found = true;
    } else if (line.startsWith('[')) {
      if (!added && inDefault) {
        print(prefix + app);
        added = true;
      }
      inDefault = false;
    } else if (line.isEmpty()) {
      suppress = true;
      ++blanks;
    } else if (inDefault && !added && line.startsWith(prefix)) {
      line = prefix + app;
      added = true;
    }
    if (!suppress) {
      for (; blanks > 0; --blanks) print({});
      print(line);
    }
  }
  if (!added) {
    if (!found) {
      print({});
      print("[Default Applications]");
    }
    print(prefix + app);
  }
  for (; blanks > 0; --blanks) print({});
  return out;
}

// xdg-mime query default, from the user's list alone: the first app of the
// first "type=" line in a defaults section. Every app counts as installed.
QString xdgMimeQuery(const QByteArray& in, const QByteArray& type) {
  const QByteArray prefix = type + '=';
  bool inDefault = false;
  for (const QByteArray& line : in.split('\n')) {
    if (line.startsWith("[Default Applications]")) inDefault = true;
    else if (line.startsWith('[')) inDefault = false;
    else if (inDefault && line.startsWith(prefix)) {
      for (const QByteArray& app : line.mid(prefix.size()).split(';'))
        if (!app.isEmpty()) return QString::fromUtf8(app).trimmed();
      return {};
    }
  }
  return {};
}

bool fakeXdgMime(const QStringList& args, QString* output) {
  const QString list = configHome() + QStringLiteral("/mimeapps.list");
  QFile file(list);
  QByteArray text;
  if (file.exists()) {
    if (!file.open(QIODevice::ReadOnly)) return false;
    text = file.readAll();
    file.close();
  }
  if (args.size() == 3 && args.at(0) == u"query" && args.at(1) == u"default") {
    if (output) *output = xdgMimeQuery(text, args.at(2).toUtf8());
    return true;
  }
  if (args.size() == 3 && args.at(0) == u"default") {
    const QByteArray after = xdgMimeDefault(text, args.at(2).toUtf8(), args.at(1).toUtf8());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(after) != after.size()) std::abort();
    return true;
  }
  std::abort();
}
struct Environment {
  int argc = 1;
  char name[8] = "fuzzer";
  char* argv[2] = {name, nullptr};
  QCoreApplication app{argc, argv};
  // In memory when it can be: QSaveFile and QSettings sync every write, and
  // on a disk that makes the harnesses that save files a hundred times slower.
  QTemporaryDir dir{qEnvironmentVariableIsEmpty("TMPDIR") && QFileInfo(QStringLiteral("/dev/shm")).isWritable()
                        ? QStringLiteral("/dev/shm/omaroll-fuzz-XXXXXX")
                        : QDir::tempPath() + QStringLiteral("/omaroll-fuzz-XXXXXX")};
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
  // Read as Omaroll reads a file: the decoder its name maps to, then the
  // listed one for its contents if that refuses. The first byte picks the
  // name. Plugins for formats Omaroll does not list never see an input, as in
  // the app; Qt's own guessing reached one of them and crashed (kimg_tim).
  QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly);
  const QList<QByteArray>& formats = ImageFormats::listed();
  const QByteArray named = formats.isEmpty() || bytes.isEmpty()
                               ? QByteArray() : formats.at(quint8(bytes.at(0)) % formats.size());
  PinnedImageReader reader(&buffer, named);
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
#elif FUZZ_KIND == 14
  // The EXIF rewriter for saved copies: the first four bytes are the new
  // size. It keeps the length, also when run again on its own output. The
  // metadata it carries over reads back the same from the copy it goes into.
  QByteArray jpeg = bytes.mid(4);
  const QSize target(bytes.size() >= 4 ? (quint8(bytes.at(0)) << 8 | quint8(bytes.at(1))) : -1,
                   bytes.size() >= 4 ? (quint8(bytes.at(2)) << 8 | quint8(bytes.at(3))) : -1);
  const QByteArray segments = JpegExif::metadataSegments(jpeg);
  const bool exif = JpegExif::normalize(jpeg, target);
  if (jpeg.size() != bytes.mid(4).size()) std::abort();
  const QByteArray once = jpeg;
  if (JpegExif::normalize(jpeg, target) != exif || jpeg.size() != once.size()) std::abort();
  const QByteArray copy = JpegExif::withSegments(QByteArray("\xFF\xD8\xFF\xD9", 4), segments);
  if (JpegExif::metadataSegments(copy) != segments) std::abort();
  (void)JpegExif::withSegments(once, segments);
#elif FUZZ_KIND == 16
  // A hash cache: the first six bytes are the magic and version it is read
  // with, so most inputs get past the header. What loads saves and loads
  // back the same.
  const QString path = env.dir.filePath(QStringLiteral("hashes.bin"));
  {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) std::abort();
  }
  const auto be = [&bytes](int at, int count) {
    quint32 value = 0;
    for (int i = 0; i < count; ++i) value = value << 8 | (at + i < bytes.size() ? quint8(bytes.at(at + i)) : 0);
    return value;
  };
  const quint32 magic = be(0, 4);
  const quint16 version = quint16(be(4, 2));
  const qint64 limit = 1 << 20;
  const HashCache::Entries entries = HashCache::load(path, magic, version, limit);
  if (!entries.isEmpty()) {
    if (!HashCache::save(path, magic, version, entries, entries.keys(), limit)) std::abort();
    const HashCache::Entries again = HashCache::load(path, magic, version, limit);
    if (again.size() != entries.size()) std::abort();
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
      const auto found = again.constFind(it.key());
      if (found == again.cend() || found->bytes != it->bytes || found->modified != it->modified || found->data != it->data)
        std::abort();
    }
  }
  QFile::remove(path);
#elif FUZZ_KIND == 18
  // The user's mimeapps.list after the first byte, which says whether the
  // file exists (bit 0) and whether set runs twice (bit 1). Two set and unset
  // cycles through the real MimeDefaults, with xdg-mime modelled in process,
  // leave it byte for byte as it was, or absent if it was absent. A list
  // that already names Omaroll is not the user's own state, so it is only
  // run.
  static int round = 0;
  static const bool installed = [&] {
    MimeDefaults::setFuzzXdgMime(fakeXdgMime);
    QDir().mkpath(env.dir.filePath(QStringLiteral("applications")));
    QFile file(env.dir.filePath(QStringLiteral("applications/") + kOmarollDesktop));
    const QByteArray entry = "[Desktop Entry]\nType=Application\nName=Omaroll\nExec=omaroll %F\n"
                             "MimeType=image/png;image/jpeg;video/mp4;application/pdf;\n";
    return file.open(QIODevice::WriteOnly) && file.write(entry) == entry.size();
  }();
  if (!installed) std::abort();
  const quint8 flags = bytes.isEmpty() ? 0 : quint8(bytes.at(0));
  const QByteArray original = bytes.mid(1);
  const bool exists = !(flags & 1);
  // A new folder each time: QSettings caches a record by its path.
  const QString config = env.dir.filePath(QStringLiteral("config-%1").arg(++round));
  if (!QDir().mkpath(config)) std::abort();
  qputenv("XDG_CONFIG_HOME", config.toUtf8());
  qputenv("XDG_CURRENT_DESKTOP", "");
  const QString list = config + QStringLiteral("/mimeapps.list");
  if (exists) {
    QFile file(list);
    if (!file.open(QIODevice::WriteOnly) || file.write(original) != original.size()) std::abort();
  }
  MimeDefaults defaults;
  const bool own = !original.contains("omaroll");
  for (int cycle = 0; cycle < 2; ++cycle) {
    for (int set = 0; set < ((flags & 2) ? 2 : 1); ++set) (void)defaults.apply(false, QStringLiteral("all"));
    const QString report = defaults.apply(true, QStringLiteral("all"));
    if (!own) continue;
    if (!defaults.succeeded()) {
      fprintf(stderr, "unset failed:\n%s\n", qPrintable(report));
      std::abort();
    }
    QFile file(list);
    if (file.exists() != exists) {
      fprintf(stderr, "cycle %d: the list %s\n", cycle, exists ? "is gone" : "was left behind");
      std::abort();
    }
    if (exists && (!file.open(QIODevice::ReadOnly) || file.readAll() != original)) {
      file.seek(0);
      fprintf(stderr, "cycle %d: the list changed:\n%s\n", cycle, file.readAll().toPercentEncoding(" =;[]/.\n").constData());
      std::abort();
    }
  }
  QDir(config).removeRecursively();
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
#elif FUZZ_KIND == 15
  (void)PictureDiagnosis::truncation(path);
#elif FUZZ_KIND == 17
  fuzzWebp(path);
#endif
#endif
  return 0;
}
