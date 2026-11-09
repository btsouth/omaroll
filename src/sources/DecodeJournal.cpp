#include "sources/DecodeJournal.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QtGlobal>

#include <atomic>
#include <bit>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// One slot per decoding thread. Omaroll's pools run a few dozen threads at
// most; a thread past the last slot decodes unrecorded.
constexpr int kSlots = 64;
constexpr qsizetype kSlotBytes = 4096;
constexpr qsizetype kHeaderBytes = 4096;
constexpr qsizetype kFileBytes = kHeaderBytes + kSlots * kSlotBytes;
constexpr char kMagic[8] = {'O', 'M', 'R', 'D', 'E', 'C', '0', '1'};

struct Header {
  char magic[8];
  quint32 version;
  // The slot of the thread a fatal signal arrived on, or -1.
  qint32 crashedSlot;
  qint32 signal;
};

struct Slot {
  // The path's length, written last; zero while the thread decodes nothing.
  quint32 length;
  quint32 reserved;
  // Taken by the crash handler, so a decode costs no stat.
  qint64 size;
  qint64 modifiedNs;
  char path[kSlotBytes - 24];
};
static_assert(sizeof(Slot) == kSlotBytes);
static_assert(sizeof(Header) <= kHeaderBytes);
constexpr quint32 kPathBytes = sizeof(Slot::path);

std::atomic<char*> g_base{nullptr};
int g_fd = -1;
std::atomic<quint64> g_used{0};

Header* header(char* base) { return reinterpret_cast<Header*>(base); }
Slot* slotAt(char* base, int slot) {
  return reinterpret_cast<Slot*>(base + kHeaderBytes + slot * kSlotBytes);
}

// The crash handler reads these, so they must not need lazy initialization.
__attribute__((tls_model("initial-exec"))) thread_local int t_slot = -1;
thread_local DecodeJournal::Scope* t_top = nullptr;
// The handler's own stack, so a recursive parser that overflows the thread's
// stack is still recorded. Only for a thread that had none.
constexpr size_t kAltStackBytes = 64 * 1024;
thread_local void* t_altStack = nullptr;

void ensureAltStack() {
  stack_t current {};
  if (t_altStack || ::sigaltstack(nullptr, &current) != 0 || !(current.ss_flags & SS_DISABLE)) return;
  void* memory = ::mmap(nullptr, kAltStackBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                        -1, 0);
  if (memory == MAP_FAILED) return;
  stack_t stack {};
  stack.ss_sp = memory;
  stack.ss_size = kAltStackBytes;
  if (::sigaltstack(&stack, nullptr) != 0) {
    ::munmap(memory, kAltStackBytes);
    return;
  }
  t_altStack = memory;
}

void releaseAltStack() {
  if (!t_altStack) return;
  stack_t disabled {};
  disabled.ss_flags = SS_DISABLE;
  ::sigaltstack(&disabled, nullptr);
  ::munmap(t_altStack, kAltStackBytes);
  t_altStack = nullptr;
}

void clearSlot(int slot) {
  char* base = g_base.load(std::memory_order_acquire);
  if (base) std::atomic_ref<quint32>(slotAt(base, slot)->length).store(0, std::memory_order_release);
}

// Gives the slot back when its thread ends, as pooled threads do when idle.
struct SlotOwner {
  bool armed = false;
  ~SlotOwner() {
    releaseAltStack();
    if (t_slot < 0) return;
    clearSlot(t_slot);
    g_used.fetch_and(~(quint64(1) << t_slot));
    t_slot = -1;
  }
};
thread_local SlotOwner t_owner;

int claimSlot() {
  if (t_slot >= 0) return t_slot;
  quint64 used = g_used.load();
  while (used != ~quint64(0)) {
    const int free = std::countr_one(used);
    if (g_used.compare_exchange_weak(used, used | (quint64(1) << free))) {
      t_slot = free;
      t_owner.armed = true;
      ensureAltStack();
      return free;
    }
  }
  return -1;
}

void writeSlot(int slot, const QByteArray& path) {
  char* base = g_base.load(std::memory_order_acquire);
  if (!base) return;
  Slot* target = slotAt(base, slot);
  std::atomic_ref<quint32> length(target->length);
  length.store(0, std::memory_order_relaxed);
  std::memcpy(target->path, path.constData(), size_t(path.size()));
  target->path[path.size()] = '\0';
  length.store(quint32(path.size()), std::memory_order_release);
}

// Async-signal-safe: loads, stores and stat(2). False when this thread is
// not decoding anything.
bool markCrash(int signal) {
  char* base = g_base.load(std::memory_order_acquire);
  const int slot = t_slot;
  if (!base || slot < 0) return false;
  Slot* crashed = slotAt(base, slot);
  const quint32 length = std::atomic_ref<quint32>(crashed->length).load(std::memory_order_acquire);
  if (length == 0 || length >= kPathBytes) return false;
  struct stat info {};
  if (::stat(crashed->path, &info) == 0) {
    crashed->size = info.st_size;
    crashed->modifiedNs = qint64(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec;
  } else {
    crashed->size = -1;
  }
  header(base)->signal = signal;
  std::atomic_ref<qint32>(header(base)->crashedSlot).store(slot, std::memory_order_release);
  return true;
}

constexpr int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
struct sigaction g_previous[NSIG] = {};

void onFatalSignal(int signal, siginfo_t* info, void* context) {
  const struct sigaction& previous = g_previous[signal];
  if (!markCrash(signal)) {
    // Not a decode: someone else's signal, which their handler may survive.
    // It is handed on and this handler stays armed for the decoders, unless
    // that handler asked to be run once (QtTest's does): then the default
    // comes next, as the kernel would have arranged, or a fault that runs
    // again would call it again forever.
    const bool once = previous.sa_flags & SA_RESETHAND;
    if (once && previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
      struct sigaction fallback {};
      fallback.sa_handler = SIG_DFL;
      sigemptyset(&fallback.sa_mask);
      ::sigaction(signal, &fallback, nullptr);
    }
    if (previous.sa_flags & SA_SIGINFO) {
      if (previous.sa_sigaction) previous.sa_sigaction(signal, info, context);
      return;
    }
    if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
      previous.sa_handler(signal);
      return;
    }
  }
  // The process ends here. As if this handler had never been there: a fault
  // runs again into the previous handler (a sanitizer's, or the core dump),
  // and a signal sent with kill or raise is sent again.
  ::sigaction(signal, &previous, nullptr);
  if (!info || info->si_code <= 0) ::raise(signal);
}

void installHandlers() {
  for (const int signal : kFatalSignals) {
    struct sigaction current {};
    // Already first in line: installing again would chain to itself.
    if (::sigaction(signal, nullptr, &current) == 0 && (current.sa_flags & SA_SIGINFO) &&
        current.sa_sigaction == onFatalSignal) {
      continue;
    }
    struct sigaction action {};
    action.sa_sigaction = onFatalSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    ::sigaction(signal, &action, &g_previous[signal]);
  }
}

struct Identity {
  qint64 size = -1;
  qint64 modifiedNs = 0;
  bool operator==(const Identity&) const = default;
};

// Blocked files are known by one name whichever way they are reached.
QString canonical(const QString& path) {
  const QFileInfo info(path);
  const QString resolved = info.canonicalFilePath();
  return resolved.isEmpty() ? info.absoluteFilePath() : resolved;
}

bool identityOf(const QString& path, Identity* identity) {
  struct stat info {};
  if (::stat(QFile::encodeName(path).constData(), &info) != 0) return false;
  identity->size = info.st_size;
  identity->modifiedNs = qint64(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec;
  return true;
}

// Files that crashed a decoder, kept beside the journal until they change.
QMutex g_mutex;
QHash<QString, Identity> g_blocked;
QString g_notice;
QString g_storePath;
std::atomic<bool> g_anyBlocked{false};
std::atomic<bool> g_failReservation{false};

void saveStore();

void loadStore() {
  g_blocked.clear();
  g_notice.clear();
  bool pruned = false;
  QFile file(g_storePath);
  if (file.open(QIODevice::ReadOnly)) {
    const QJsonObject root = QJsonDocument::fromJson(file.read(1024 * 1024)).object();
    for (const QJsonValue& value : root.value(QStringLiteral("files")).toArray()) {
      const QJsonObject entry = value.toObject();
      const QString path = entry.value(QStringLiteral("path")).toString();
      if (path.isEmpty()) continue;
      // Nanosecond times are past a double's precision, so they are text.
      bool sized = false;
      bool timed = false;
      const qint64 size = entry.value(QStringLiteral("size")).toString().toLongLong(&sized);
      const qint64 modified = entry.value(QStringLiteral("modified")).toString().toLongLong(&timed);
      // A file that is gone needs no blocking, and the list stays short. One
      // whose folder is gone too may be on a drive that is not plugged in,
      // and is kept for when it is.
      const QFileInfo info(path);
      if (!sized || !timed || (!info.exists() && info.absoluteDir().exists())) {
        pruned = true;
        continue;
      }
      g_blocked.insert(canonical(path), {size, modified});
    }
    const QString notice = root.value(QStringLiteral("notice")).toString();
    if (!notice.isEmpty() && g_blocked.contains(canonical(notice))) g_notice = canonical(notice);
  }
  if (pruned) saveStore();
  g_anyBlocked.store(!g_blocked.isEmpty(), std::memory_order_release);
}

void saveStore() {
  QJsonArray files;
  for (auto it = g_blocked.cbegin(); it != g_blocked.cend(); ++it) {
    files.append(QJsonObject{{QStringLiteral("path"), it.key()},
                             {QStringLiteral("size"), QString::number(it->size)},
                             {QStringLiteral("modified"), QString::number(it->modifiedNs)}});
  }
  QJsonObject root{{QStringLiteral("version"), 1}, {QStringLiteral("files"), files}};
  if (!g_notice.isEmpty()) root.insert(QStringLiteral("notice"), g_notice);
  if (g_blocked.isEmpty()) {
    QFile::remove(g_storePath);
    return;
  }
  QSaveFile file(g_storePath);
  if (file.open(QIODevice::WriteOnly)) {
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();
  }
}

// The file the previous process crashed on, from the journal it left.
QString crashedPath(const QByteArray& journal, Identity* identity) {
  if (journal.size() != kFileBytes) return {};
  Header previous {};
  std::memcpy(&previous, journal.constData(), sizeof(previous));
  if (std::memcmp(previous.magic, kMagic, sizeof(kMagic)) != 0 || previous.version != 1 ||
      previous.crashedSlot < 0 || previous.crashedSlot >= kSlots) {
    return {};
  }
  Slot slot {};
  std::memcpy(&slot, journal.constData() + kHeaderBytes + previous.crashedSlot * kSlotBytes,
              sizeof(slot));
  if (slot.length == 0 || slot.length >= kPathBytes) return {};
  const QString path = canonical(QFile::decodeName(QByteArray(slot.path, slot.length)));
  if (slot.size >= 0) {
    *identity = {slot.size, slot.modifiedNs};
  } else if (!identityOf(path, identity)) {
    return {};
  }
  return path;
}

// Room for the whole journal, taken before anything is written through the
// mapping: a store into a page the filesystem cannot supply is SIGBUS. On
// btrfs the file is also kept out of copy-on-write, which would otherwise need
// new space for every rewrite.
bool reserve(int fd) {
  if (::ftruncate(fd, 0) != 0) return false;
  int flags = 0;
  if (::ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0 && !(flags & FS_NOCOW_FL)) {
    flags |= FS_NOCOW_FL;
    (void)::ioctl(fd, FS_IOC_SETFLAGS, &flags);
  }
  if (g_failReservation.exchange(false)) return false;
  return ::posix_fallocate(fd, 0, kFileBytes) == 0;
}

} // namespace

namespace DecodeJournal {

bool open(const QString& directory, const QString& journalDirectory) {
  if (g_base.load()) return true;
  {
    // First, so files blocked before are skipped whatever happens to the
    // journal below.
    const QMutexLocker lock(&g_mutex);
    g_storePath = directory + QStringLiteral("/decoder-crashes.json");
    loadStore();
  }
  QDir().mkpath(directory);
  // The journal where asked, or beside the list when that folder fails.
  const auto openJournal = [](const QString& folder) {
    if (folder.isEmpty() || !QDir().mkpath(folder)) return -1;
    const QByteArray path = QFile::encodeName(folder + QStringLiteral("/decoding"));
    return ::open(path.constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  };
  int fd = openJournal(journalDirectory.isEmpty() ? directory : journalDirectory);
  if (fd < 0 && !journalDirectory.isEmpty()) fd = openJournal(directory);
  if (fd < 0) {
    qWarning("omaroll: the decode journal cannot be opened; a decoder crash will not be recorded");
    return false;
  }
  // A second Omaroll (one forwarding its files, a render) leaves the first
  // one's journal alone.
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return false;
  }
  QByteArray previous(kFileBytes, '\0');
  const ssize_t read = ::pread(fd, previous.data(), size_t(kFileBytes), 0);
  previous.resize(read > 0 ? qsizetype(read) : 0);
  {
    // Blocked files are kept apart from the journal, which may live on a
    // filesystem that is gone after a reboot.
    const QMutexLocker lock(&g_mutex);
    Identity identity;
    const QString crashed = crashedPath(previous, &identity);
    if (!crashed.isEmpty()) {
      qWarning().noquote() << "omaroll: the last run crashed while reading" << crashed
                           << "; it is skipped until it changes";
      g_blocked.insert(crashed, identity);
      g_notice = crashed;
      g_anyBlocked.store(true, std::memory_order_release);
      saveStore();
    }
  }
  // A fresh journal for this process. Without room for it, decoding goes on
  // unrecorded; files blocked before are still skipped.
  void* base = reserve(fd)
                   ? ::mmap(nullptr, size_t(kFileBytes), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
                   : MAP_FAILED;
  if (base == MAP_FAILED) {
    (void)::ftruncate(fd, 0);
    ::close(fd);
    qWarning("omaroll: no room for the decode journal; a decoder crash will not be recorded");
    return false;
  }
  Header* fresh = header(static_cast<char*>(base));
  std::memcpy(fresh->magic, kMagic, sizeof(kMagic));
  fresh->version = 1;
  fresh->crashedSlot = -1;
  g_fd = fd;
  installHandlers();
  g_base.store(static_cast<char*>(base), std::memory_order_release);
  return true;
}

void failNextReservation() { g_failReservation.store(true); }

void close() {
  char* base = g_base.exchange(nullptr);
  if (base) ::munmap(base, size_t(kFileBytes));
  if (g_fd >= 0) ::close(g_fd);
  g_fd = -1;
  const QMutexLocker lock(&g_mutex);
  g_blocked.clear();
  g_notice.clear();
  g_storePath.clear();
  g_anyBlocked.store(false);
}

bool active() { return g_base.load() != nullptr; }

bool isBlocked(const QString& path) {
  if (!g_anyBlocked.load(std::memory_order_acquire) || path.isEmpty()) return false;
  const QString key = canonical(path);
  const QMutexLocker lock(&g_mutex);
  const auto found = g_blocked.constFind(key);
  if (found == g_blocked.cend()) return false;
  Identity now;
  // A file that is gone has nothing to read either way.
  if (!identityOf(path, &now) || now == *found) return true;
  // Replaced or edited since: it gets another chance.
  g_blocked.remove(key);
  if (g_notice == key) g_notice.clear();
  g_anyBlocked.store(!g_blocked.isEmpty(), std::memory_order_release);
  saveStore();
  return false;
}

QStringList blocked() {
  const QMutexLocker lock(&g_mutex);
  return g_blocked.keys();
}

QString pendingNotice() {
  const QMutexLocker lock(&g_mutex);
  return g_notice;
}

void noticeShown() {
  const QMutexLocker lock(&g_mutex);
  if (g_notice.isEmpty()) return;
  g_notice.clear();
  saveStore();
}

void simulateCrash() { markCrash(0); }

void installCrashHandler() { installHandlers(); }

Scope::Scope(const QString& path) {
  if (!g_base.load(std::memory_order_relaxed) || path.isEmpty()) return;
  m_encoded = QFile::encodeName(path);
  if (m_encoded.size() >= qsizetype(kPathBytes)) return;
  m_slot = claimSlot();
  if (m_slot < 0) return;
  m_previous = t_top;
  t_top = this;
  writeSlot(m_slot, m_encoded);
}

Scope::~Scope() {
  if (m_slot < 0) return;
  t_top = m_previous;
  if (m_previous) {
    writeSlot(m_slot, m_previous->m_encoded);
  } else {
    clearSlot(m_slot);
  }
}

} // namespace DecodeJournal

DecodeCrashNotice::DecodeCrashNotice(QObject* parent)
    : QObject(parent), m_path(DecodeJournal::pendingNotice()) {}

QString DecodeCrashNotice::fileName() const { return QFileInfo(m_path).fileName(); }

void DecodeCrashNotice::shown() { DecodeJournal::noticeShown(); }

void DecodeCrashNotice::reload() {
  m_path = DecodeJournal::pendingNotice();
  emit changed();
}

void DecodeCrashNotice::dismiss() {
  if (m_path.isEmpty()) return;
  m_path.clear();
  emit changed();
}
