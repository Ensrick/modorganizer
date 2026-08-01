#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>

namespace
{

constexpr int ExUsage     = 64;
constexpr int ExDataErr   = 65;
constexpr int ExNoInput   = 66;
constexpr int ExCantCreat = 73;
constexpr int ExIoErr     = 74;
constexpr int ExTempFail  = 75;
constexpr int ExConfig    = 78;

class Failure : public std::runtime_error
{
public:
  Failure(int code, const QString& message)
      : std::runtime_error(message.toUtf8().constData()), m_code(code)
  {}

  int code() const { return m_code; }

private:
  int m_code;
};

QByteArray jsonBytes(const QJsonObject& object)
{
  return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

void emitJson(const QJsonObject& object, bool error = false)
{
  const auto bytes = jsonBytes(object);
  if (error) {
    std::cerr.write(bytes.constData(), bytes.size());
  } else {
    std::cout.write(bytes.constData(), bytes.size());
  }
}

QString absoluteClean(const QString& path)
{
  return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString canonicalForCompare(const QString& path)
{
  QString result = QDir::fromNativeSeparators(absoluteClean(path));
  if (!result.endsWith('/')) {
    result += '/';
  }
  return result.toLower();
}

bool isWithin(const QString& path, const QString& root)
{
  const QString candidate = QDir::fromNativeSeparators(absoluteClean(path)).toLower();
  const QString parent    = canonicalForCompare(root);
  return candidate == parent.chopped(1) || candidate.startsWith(parent);
}

class InstanceContext;
bool isManagedTransactionPath(const QString& path, const InstanceContext& context);

void requireSafeName(const QString& name, const QString& kind)
{
  if (name.trimmed().isEmpty() || name == "." || name == ".." || name.contains('/') ||
      name.contains('\\') || name.contains(':') || name.contains(QChar::Null)) {
    throw Failure(ExDataErr, QString("invalid %1 name: %2").arg(kind, name));
  }
}

QByteArray readFile(const QString& path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    throw Failure(ExIoErr, QString("cannot read %1: %2").arg(path, file.errorString()));
  }
  return file.readAll();
}

void writeAtomic(const QString& path, const QByteArray& bytes)
{
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    throw Failure(ExCantCreat,
                  QString("cannot create parent directory for %1").arg(path));
  }

  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    throw Failure(ExIoErr,
                  QString("cannot write %1: %2").arg(path, file.errorString()));
  }
  if (file.write(bytes) != bytes.size() || !file.commit()) {
    throw Failure(ExIoErr,
                  QString("cannot commit %1: %2").arg(path, file.errorString()));
  }
}

bool copyTree(const QString& source, const QString& target)
{
  const QFileInfo sourceInfo(source);
  if (!sourceInfo.exists() || !sourceInfo.isDir() || sourceInfo.isSymLink()) {
    return false;
  }
  if (isWithin(target, source)) {
    return false;
  }
  if (!QDir().mkpath(target)) {
    return false;
  }

  QDirIterator it(source, QDir::AllEntries | QDir::NoDotAndDotDot,
                  QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString item   = it.next();
    const QFileInfo info = it.fileInfo();
    if (info.isSymLink()) {
      return false;
    }
    const QString relative    = QDir(source).relativeFilePath(item);
    const QString destination = QDir(target).filePath(relative);
    if (info.isDir()) {
      if (!QDir().mkpath(destination)) {
        return false;
      }
    } else if (info.isFile()) {
      if (!QDir().mkpath(QFileInfo(destination).absolutePath()) ||
          !QFile::copy(item, destination)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

bool copyTreeOverlay(const QString& source, const QString& target)
{
  const QFileInfo sourceInfo(source);
  if (!sourceInfo.exists() || !sourceInfo.isDir() || sourceInfo.isSymLink() ||
      !QDir().mkpath(target)) {
    return false;
  }
  QDirIterator it(source, QDir::AllEntries | QDir::NoDotAndDotDot,
                  QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString item   = it.next();
    const QFileInfo info = it.fileInfo();
    if (info.isSymLink()) {
      return false;
    }
    const QString destination =
        QDir(target).filePath(QDir(source).relativeFilePath(item));
    if (info.isDir()) {
      if (!QDir().mkpath(destination)) {
        return false;
      }
    } else if (info.isFile()) {
      if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        return false;
      }
      if (QFileInfo::exists(destination) && !QFile::remove(destination)) {
        return false;
      }
      if (!QFile::copy(item, destination)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

bool isSafeRelative(QString path)
{
  path = QDir::fromNativeSeparators(path.trimmed());
  if (path.isEmpty() || path.startsWith('/') ||
      (path.size() > 1 && path.at(1) == ':')) {
    return false;
  }
  const auto components = path.split('/', Qt::SkipEmptyParts);
  return std::none_of(components.cbegin(), components.cend(),
                      [](const QString& component) {
                        return component == "..";
                      });
}

void validateExtractedTree(const QString& root);

void applyInstallPlan(const QString& extractedRoot, const QString& planPath,
                      const QString& resolvedRoot)
{
  const auto document = QJsonDocument::fromJson(readFile(planPath));
  if (!document.isObject()) {
    throw Failure(ExDataErr, "install plan must be a JSON object");
  }
  const auto object = document.object();
  if (object["schemaVersion"].toInt() != 1) {
    throw Failure(ExDataErr, "unsupported install plan schemaVersion");
  }
  const auto mappings = object["mappings"].toArray();
  if (mappings.isEmpty()) {
    throw Failure(ExDataErr, "install plan contains no mappings");
  }
  if (!QDir().mkpath(resolvedRoot)) {
    throw Failure(ExCantCreat, "cannot create resolved install stage");
  }

  for (const auto& value : mappings) {
    const auto mapping           = value.toObject();
    const QString sourceRelative = mapping["source"].toString();
    QString destinationRelative  = mapping["destination"].toString(".");
    if (!isSafeRelative(sourceRelative) || !isSafeRelative(destinationRelative)) {
      throw Failure(ExDataErr, "install plan contains an unsafe relative path");
    }
    const QString source = absoluteClean(QDir(extractedRoot).filePath(sourceRelative));
    const QString destination =
        absoluteClean(QDir(resolvedRoot).filePath(destinationRelative));
    if (!isWithin(source, extractedRoot) || !isWithin(destination, resolvedRoot) ||
        !QFileInfo::exists(source)) {
      throw Failure(ExDataErr,
                    QString("invalid install mapping source: %1").arg(sourceRelative));
    }
    const QFileInfo sourceInfo(source);
    if (sourceInfo.isSymLink()) {
      throw Failure(ExDataErr, "install plan source is a link");
    }
    if (sourceInfo.isDir()) {
      if (!copyTreeOverlay(source, destination)) {
        throw Failure(ExIoErr,
                      QString("cannot apply install mapping: %1").arg(sourceRelative));
      }
    } else if (sourceInfo.isFile()) {
      QString output = destination;
      if (destinationRelative.endsWith('/') || QFileInfo(destination).isDir()) {
        output = QDir(destination).filePath(sourceInfo.fileName());
      }
      if (!QDir().mkpath(QFileInfo(output).absolutePath())) {
        throw Failure(ExCantCreat, QString("cannot create %1").arg(output));
      }
      if (QFileInfo::exists(output) && !QFile::remove(output)) {
        throw Failure(ExIoErr, QString("cannot replace %1").arg(output));
      }
      if (!QFile::copy(source, output)) {
        throw Failure(ExIoErr, QString("cannot copy %1").arg(sourceRelative));
      }
    } else {
      throw Failure(ExDataErr, "install mapping source is not a file or directory");
    }
  }
  validateExtractedTree(resolvedRoot);
}

struct ModEntry
{
  QString name;
  bool enabled = false;
  QChar marker = '-';
};

QList<ModEntry> readModList(const QString& path)
{
  QList<ModEntry> result;
  if (!QFileInfo::exists(path)) {
    return result;
  }

  const auto lines = QString::fromUtf8(readFile(path)).split('\n');
  QSet<QString> seen;
  for (QString line : lines) {
    line = line.trimmed();
    if (line.isEmpty() || line.startsWith('#')) {
      continue;
    }
    QChar marker = line.at(0);
    if (marker != '+' && marker != '-' && marker != '*') {
      marker = '-';
    } else {
      line.remove(0, 1);
    }
    const QString name = line.trimmed();
    const QString key  = name.toLower();
    if (name.isEmpty() || seen.contains(key)) {
      continue;
    }
    seen.insert(key);
    result.push_back({name, marker != '-', marker});
  }
  return result;
}

QByteArray serializeModList(const QList<ModEntry>& entries)
{
  QByteArray bytes("# This file was automatically generated by MO2Headless.\r\n");
  for (const auto& entry : entries) {
    bytes += (entry.marker == '*' ? '*' : (entry.enabled ? '+' : '-'));
    bytes += entry.name.toUtf8();
    bytes += "\r\n";
  }
  return bytes;
}

struct PluginEntry
{
  QString name;
  bool enabled = false;
};

QList<PluginEntry> readPluginList(const QString& path)
{
  QList<PluginEntry> result;
  if (!QFileInfo::exists(path)) {
    return result;
  }
  QSet<QString> seen;
  const auto lines = QString::fromUtf8(readFile(path)).split('\n');
  for (QString line : lines) {
    line = line.trimmed();
    if (line.isEmpty() || line.startsWith('#')) {
      continue;
    }
    const bool enabled = line.startsWith('*');
    if (enabled) {
      line.remove(0, 1);
    }
    const QString name = line.trimmed();
    const QString key  = name.toLower();
    if (name.isEmpty() || seen.contains(key)) {
      continue;
    }
    seen.insert(key);
    result.push_back({name, enabled});
  }
  return result;
}

QByteArray serializePluginList(const QList<PluginEntry>& entries)
{
  QByteArray bytes("# This file was automatically generated by MO2Headless.\r\n");
  for (const auto& entry : entries) {
    if (entry.enabled) {
      bytes += '*';
    }
    bytes += entry.name.toUtf8();
    bytes += "\r\n";
  }
  return bytes;
}

class InstanceContext
{
public:
  InstanceContext(QString root, QString requestedProfile, bool requireIni = true)
      : root(absoluteClean(std::move(root))),
        iniPath(QDir(this->root).filePath("ModOrganizer.ini"))
  {
    if (requireIni && !QFileInfo::exists(iniPath)) {
      throw Failure(ExConfig, QString("portable instance has no %1").arg(iniPath));
    }

    if (QFileInfo::exists(iniPath)) {
      QSettings settings(iniPath, QSettings::IniFormat);
      base = resolve(settings.value("Settings/base_directory", this->root).toString(),
                     this->root);
      mods = resolve(
          settings.value("Settings/mod_directory", "%BASE_DIR%/mods").toString(), base);
      profiles =
          resolve(settings.value("Settings/profiles_directory", "%BASE_DIR%/profiles")
                      .toString(),
                  base);
      downloads =
          resolve(settings.value("Settings/download_directory", "%BASE_DIR%/downloads")
                      .toString(),
                  base);
      overwrite =
          resolve(settings.value("Settings/overwrite_directory", "%BASE_DIR%/overwrite")
                      .toString(),
                  base);
      gamePath = QString::fromUtf8(settings.value("General/gamePath").toByteArray());
      if (gamePath.isEmpty()) {
        gamePath = settings.value("General/gamePath").toString();
      }
      profile = requestedProfile;
      if (profile.isEmpty()) {
        profile = QString::fromUtf8(
            settings.value("General/selected_profile", "Default").toByteArray());
      }
      if (profile.isEmpty()) {
        profile = "Default";
      }
    } else {
      base      = this->root;
      mods      = QDir(base).filePath("mods");
      profiles  = QDir(base).filePath("profiles");
      downloads = QDir(base).filePath("downloads");
      overwrite = QDir(base).filePath("overwrite");
      profile   = requestedProfile.isEmpty() ? "Default" : requestedProfile;
    }
  }

  QString profilePath() const
  {
    requireSafeName(profile, "profile");
    return QDir(profiles).filePath(profile);
  }

  QString modListPath() const { return QDir(profilePath()).filePath("modlist.txt"); }
  QString pluginsPath() const { return QDir(profilePath()).filePath("plugins.txt"); }
  QString loadOrderPath() const
  {
    return QDir(profilePath()).filePath("loadorder.txt");
  }

  void requireProfile() const
  {
    if (!QFileInfo(profilePath()).isDir()) {
      throw Failure(ExNoInput, QString("profile does not exist: %1").arg(profile));
    }
  }

  static QString resolve(QString path, const QString& base)
  {
    path.replace("%BASE_DIR%", base, Qt::CaseInsensitive);
    if (QDir::isRelativePath(path)) {
      path = QDir(base).filePath(path);
    }
    return absoluteClean(path);
  }

  QString root;
  QString iniPath;
  QString base;
  QString mods;
  QString profiles;
  QString downloads;
  QString overwrite;
  QString gamePath;
  QString profile;
};

bool isManagedTransactionPath(const QString& path, const InstanceContext& context)
{
  if (isWithin(path, context.root) || isWithin(path, context.base) ||
      isWithin(path, context.mods) || isWithin(path, context.profiles) ||
      isWithin(path, context.downloads) || isWithin(path, context.overwrite)) {
    return true;
  }

  const auto isManagedSibling = [&path](const QString& managedDirectory) {
    const QString parent = QFileInfo(managedDirectory).absolutePath();
    const QString relative =
        QDir::fromNativeSeparators(QDir(parent).relativeFilePath(absoluteClean(path)));
    return relative != ".." && !relative.startsWith("../") &&
           (relative.startsWith(".mo2-headless-stage-") ||
            relative.startsWith(".mo2-headless-trash/"));
  };
  return isManagedSibling(context.mods) || isManagedSibling(context.profiles);
}

class Mutation
{
public:
  Mutation(const InstanceContext& context, QString operation, bool dryRun)
      : m_context(context), m_operation(std::move(operation)), m_dryRun(dryRun)
  {
    const QString stamp =
        QDateTime::currentDateTimeUtc().toString("yyyyMMddTHHmmsszzzZ");
    m_id      = stamp + "-" + QUuid::createUuid().toString(QUuid::Id128).left(12);
    m_journal = QDir(m_context.root).filePath("headless-journal/" + m_id);
    if (!m_dryRun && !QDir().mkpath(QDir(m_journal).filePath("files"))) {
      throw Failure(ExCantCreat, QString("cannot create journal %1").arg(m_journal));
    }
  }

  ~Mutation()
  {
    if (!m_committed && !m_dryRun) {
      try {
        rollbackInMemory();
        writeManifest(false, true);
      } catch (...) {
      }
    }
  }

  QString id() const { return m_id; }
  QString journal() const { return m_journal; }
  bool dryRun() const { return m_dryRun; }

  void write(const QString& path, const QByteArray& bytes)
  {
    backup(path);
    if (!m_dryRun) {
      writeAtomic(path, bytes);
    }
  }

  void move(const QString& from, const QString& to)
  {
    if (!m_dryRun && !QFileInfo::exists(from)) {
      throw Failure(ExNoInput, QString("move source does not exist: %1").arg(from));
    }
    if (!m_dryRun && QFileInfo::exists(to)) {
      throw Failure(ExCantCreat,
                    QString("move destination already exists: %1").arg(to));
    }
    m_moves.push_back({{"from", absoluteClean(from)}, {"to", absoluteClean(to)}});
    if (!m_dryRun) {
      if (!QDir().mkpath(QFileInfo(to).absolutePath()) || !QDir().rename(from, to)) {
        throw Failure(ExIoErr, QString("cannot move %1 to %2").arg(from, to));
      }
    }
  }

  void commit()
  {
    if (!m_dryRun) {
      writeManifest(true, false);
    }
    m_committed = true;
  }

private:
  void backup(const QString& path)
  {
    const QString clean = absoluteClean(path);
    for (const auto& entry : m_files) {
      if (entry["path"].toString().compare(clean, Qt::CaseInsensitive) == 0) {
        return;
      }
    }

    const bool existed = QFileInfo::exists(clean);
    QJsonObject entry{{"path", clean}, {"existed", existed}};
    if (existed && !m_dryRun) {
      const QString hash = QString::fromLatin1(
          QCryptographicHash::hash(clean.toUtf8(), QCryptographicHash::Sha256).toHex());
      const QString backupPath = QDir(m_journal).filePath("files/" + hash + ".bak");
      if (!QFile::copy(clean, backupPath)) {
        throw Failure(ExIoErr, QString("cannot back up %1").arg(clean));
      }
      entry["backup"] = backupPath;
    }
    m_files.push_back(entry);
  }

  void rollbackInMemory()
  {
    for (auto it = m_moves.crbegin(); it != m_moves.crend(); ++it) {
      const QString from = (*it)["from"].toString();
      const QString to   = (*it)["to"].toString();
      if (QFileInfo::exists(to) && !QFileInfo::exists(from)) {
        QDir().mkpath(QFileInfo(from).absolutePath());
        QDir().rename(to, from);
      }
    }
    for (const auto& entry : m_files) {
      const QString path = entry["path"].toString();
      if (entry["existed"].toBool()) {
        writeAtomic(path, readFile(entry["backup"].toString()));
      } else if (QFileInfo::exists(path)) {
        QFile::remove(path);
      }
    }
  }

  void writeManifest(bool committed, bool rolledBack)
  {
    QJsonArray files;
    for (const auto& file : m_files) {
      files.push_back(file);
    }
    QJsonArray moves;
    for (const auto& move : m_moves) {
      moves.push_back(move);
    }
    const QJsonObject manifest{
        {"schemaVersion", 1},       {"id", m_id},
        {"operation", m_operation}, {"instanceRoot", m_context.root},
        {"committed", committed},   {"rolledBack", rolledBack},
        {"files", files},           {"moves", moves}};
    writeAtomic(QDir(m_journal).filePath("transaction.json"), jsonBytes(manifest));
  }

  const InstanceContext& m_context;
  QString m_operation;
  bool m_dryRun    = false;
  bool m_committed = false;
  QString m_id;
  QString m_journal;
  QList<QJsonObject> m_files;
  QList<QJsonObject> m_moves;
};

QString findCaseInsensitiveDirectory(const QString& parent, const QString& name)
{
  const QDir dir(parent);
  for (const auto& entry : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    if (entry.fileName().compare(name, Qt::CaseInsensitive) == 0) {
      return entry.absoluteFilePath();
    }
  }
  return {};
}

QString findModDirectory(const InstanceContext& context, const QString& name)
{
  return findCaseInsensitiveDirectory(context.mods, name);
}

void ensureProfileFiles(const QString& profilePath)
{
  if (!QDir().mkpath(profilePath)) {
    throw Failure(ExCantCreat, QString("cannot create profile %1").arg(profilePath));
  }
  const QList<QPair<QString, QByteArray>> files = {
      {"modlist.txt", "# This file was automatically generated by MO2Headless.\r\n"},
      {"plugins.txt", "# This file was automatically generated by MO2Headless.\r\n"},
      {"loadorder.txt", "# This file was automatically generated by MO2Headless.\r\n"},
      {"archives.txt", QByteArray()},
  };
  for (const auto& [name, content] : files) {
    const QString path = QDir(profilePath).filePath(name);
    if (!QFileInfo::exists(path)) {
      writeAtomic(path, content);
    }
  }
  const QString settingsPath = QDir(profilePath).filePath("settings.ini");
  if (!QFileInfo::exists(settingsPath)) {
    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.setValue("LocalSaves", false);
    settings.setValue("LocalSettings", false);
    settings.sync();
  }
}

QSet<QString> discoverPlugins(const InstanceContext& context,
                              const QList<ModEntry>& modEntries)
{
  QSet<QString> result;
  auto scan = [&result](const QString& directory) {
    const QDir dir(directory);
    const auto files = dir.entryInfoList({"*.esm", "*.esp", "*.esl"}, QDir::Files,
                                         QDir::Name | QDir::IgnoreCase);
    for (const auto& file : files) {
      result.insert(file.fileName().toLower());
    }
  };

  if (!context.gamePath.isEmpty()) {
    scan(QDir(context.gamePath).filePath("Data"));
  }
  for (auto it = modEntries.crbegin(); it != modEntries.crend(); ++it) {
    if (!it->enabled) {
      continue;
    }
    const QString directory = findModDirectory(context, it->name);
    if (!directory.isEmpty()) {
      scan(directory);
    }
  }
  return result;
}

void validateExtractedTree(const QString& root)
{
  QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot,
                  QDirIterator::Subdirectories);
  bool any = false;
  while (it.hasNext()) {
    it.next();
    any                  = true;
    const QFileInfo info = it.fileInfo();
    if (info.isSymLink() || !isWithin(info.absoluteFilePath(), root)) {
      throw Failure(ExDataErr, QString("archive contains a link or escaping path: %1")
                                   .arg(info.absoluteFilePath()));
    }
  }
  if (!any) {
    throw Failure(ExDataErr, "archive or source directory is empty");
  }
}

bool containsFomod(const QString& root)
{
  QDirIterator it(root, {"ModuleConfig.xml"}, QDir::Files,
                  QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QFileInfo info(it.next());
    if (info.dir().dirName().compare("fomod", Qt::CaseInsensitive) == 0) {
      return true;
    }
  }
  return false;
}

QString normalizedContentRoot(const QString& stage)
{
  QDir dir(stage);
  const auto files = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
  const auto dirs  = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
  if (files.isEmpty() && dirs.size() == 1) {
    return dirs.front().absoluteFilePath();
  }
  return stage;
}

void validateArchiveListing(const QByteArray& listing)
{
  for (QString line : QString::fromUtf8(listing).split('\n')) {
    line = QDir::fromNativeSeparators(line.trimmed());
    if (line.isEmpty()) {
      continue;
    }
    if (line.startsWith('/') || line.startsWith("../") || line.contains("/../") ||
        line == ".." || (line.size() > 1 && line.at(1) == ':')) {
      throw Failure(ExDataErr, QString("unsafe archive entry: %1").arg(line));
    }
  }
}

QByteArray runProcess(const QString& program, const QStringList& arguments,
                      int timeoutMs, QByteArray* stderrBytes = nullptr)
{
  QProcess process;
  process.setProgram(program);
  process.setArguments(arguments);
  process.start();
  if (!process.waitForStarted(30000)) {
    throw Failure(ExNoInput,
                  QString("cannot start %1: %2").arg(program, process.errorString()));
  }
  if (!process.waitForFinished(timeoutMs)) {
    process.kill();
    process.waitForFinished(10000);
    throw Failure(ExTempFail, QString("process timed out: %1").arg(program));
  }
  const QByteArray output = process.readAllStandardOutput();
  const QByteArray errors = process.readAllStandardError();
  if (stderrBytes) {
    *stderrBytes = errors;
  }
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    throw Failure(ExIoErr, QString("%1 exited with %2: %3")
                               .arg(program)
                               .arg(process.exitCode())
                               .arg(QString::fromUtf8(errors).trimmed()));
  }
  return output;
}

void initInstance(const InstanceContext& context, const QString& gamePath,
                  const QString& gameName, bool dryRun)
{
  if (QFileInfo::exists(context.iniPath)) {
    throw Failure(ExCantCreat,
                  QString("instance already exists: %1").arg(context.root));
  }
  if (!QFileInfo(gamePath).isDir()) {
    throw Failure(ExNoInput, QString("game path does not exist: %1").arg(gamePath));
  }
  if (dryRun) {
    return;
  }
  for (const auto& directory :
       {context.root, context.mods, context.profiles, context.downloads,
        context.overwrite, QDir(context.root).filePath("webcache")}) {
    if (!QDir().mkpath(directory)) {
      throw Failure(ExCantCreat, QString("cannot create %1").arg(directory));
    }
  }
  ensureProfileFiles(QDir(context.profiles).filePath("Default"));

  QSettings settings(context.iniPath, QSettings::IniFormat);
  settings.setValue("General/gameName", gameName);
  settings.setValue("General/gamePath", QDir::toNativeSeparators(gamePath).toUtf8());
  settings.setValue("General/selected_profile", QByteArray("Default"));
  settings.setValue("Settings/base_directory", QDir::toNativeSeparators(context.root));
  settings.setValue("Settings/download_directory", "%BASE_DIR%/downloads");
  settings.setValue("Settings/mod_directory", "%BASE_DIR%/mods");
  settings.setValue("Settings/profiles_directory", "%BASE_DIR%/profiles");
  settings.setValue("Settings/overwrite_directory", "%BASE_DIR%/overwrite");
  settings.setValue("Settings/use_custom_browser", false);
  settings.sync();
  if (settings.status() != QSettings::NoError) {
    throw Failure(ExIoErr, QString("cannot write %1").arg(context.iniPath));
  }
}

QJsonObject contextStatus(const InstanceContext& context)
{
  const QDir modsDir(context.mods);
  const QDir profilesDir(context.profiles);
  return {{"ok", true},
          {"operation", "status"},
          {"instanceRoot", context.root},
          {"base", context.base},
          {"mods", context.mods},
          {"profiles", context.profiles},
          {"selectedProfile", context.profile},
          {"gamePath", context.gamePath},
          {"modCount", modsDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size()},
          {"profileCount",
           profilesDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size()}};
}

void acquireLock(QLockFile& lock, int timeoutMs)
{
  lock.setStaleLockTime(0);
  if (!lock.tryLock(timeoutMs)) {
    qint64 pid = 0;
    QString host;
    QString app;
    lock.getLockInfo(&pid, &host, &app);
    throw Failure(
        ExTempFail,
        QString("instance is locked by pid %1 (%2 on %3)").arg(pid).arg(app, host));
  }
}

int findModEntry(const QList<ModEntry>& entries, const QString& name)
{
  for (int i = 0; i < entries.size(); ++i) {
    if (entries[i].name.compare(name, Qt::CaseInsensitive) == 0) {
      return i;
    }
  }
  return -1;
}

int findPluginEntry(const QList<PluginEntry>& entries, const QString& name)
{
  for (int i = 0; i < entries.size(); ++i) {
    if (entries[i].name.compare(name, Qt::CaseInsensitive) == 0) {
      return i;
    }
  }
  return -1;
}

QJsonArray modEntriesJson(const QList<ModEntry>& entries)
{
  QJsonArray array;
  for (int i = 0; i < entries.size(); ++i) {
    array.push_back(QJsonObject{{"name", entries[i].name},
                                {"enabled", entries[i].enabled},
                                {"priority", entries.size() - i - 1}});
  }
  return array;
}

QJsonArray pluginEntriesJson(const QList<PluginEntry>& entries)
{
  QJsonArray array;
  for (int i = 0; i < entries.size(); ++i) {
    array.push_back(QJsonObject{
        {"name", entries[i].name}, {"enabled", entries[i].enabled}, {"priority", i}});
  }
  return array;
}

void restoreTransaction(const InstanceContext& context, const QString& id, bool dryRun)
{
  requireSafeName(id, "transaction");
  const QString journal      = QDir(context.root).filePath("headless-journal/" + id);
  const QString manifestPath = QDir(journal).filePath("transaction.json");
  if (!QFileInfo::exists(manifestPath)) {
    throw Failure(ExNoInput, QString("transaction does not exist: %1").arg(id));
  }
  const auto document = QJsonDocument::fromJson(readFile(manifestPath));
  if (!document.isObject()) {
    throw Failure(ExDataErr, "transaction manifest is invalid");
  }
  QJsonObject manifest = document.object();
  if (manifest["instanceRoot"].toString().compare(context.root, Qt::CaseInsensitive) !=
      0) {
    throw Failure(ExDataErr, "transaction belongs to a different instance");
  }
  if (manifest["rolledBack"].toBool()) {
    throw Failure(ExDataErr, "transaction is already rolled back");
  }

  const auto moves = manifest["moves"].toArray();
  for (auto it = moves.crbegin(); it != moves.crend(); ++it) {
    const auto move    = it->toObject();
    const QString from = absoluteClean(move["from"].toString());
    const QString to   = absoluteClean(move["to"].toString());
    if (!isManagedTransactionPath(from, context) ||
        !isManagedTransactionPath(to, context)) {
      throw Failure(ExDataErr, "transaction move escapes the instance base");
    }
    if (!dryRun && QFileInfo::exists(to) && !QFileInfo::exists(from)) {
      QDir().mkpath(QFileInfo(from).absolutePath());
      if (!QDir().rename(to, from)) {
        throw Failure(ExIoErr, QString("cannot reverse move %1").arg(to));
      }
    }
  }

  for (const auto& value : manifest["files"].toArray()) {
    const auto file    = value.toObject();
    const QString path = absoluteClean(file["path"].toString());
    if (!isManagedTransactionPath(path, context) &&
        path.compare(context.iniPath, Qt::CaseInsensitive) != 0) {
      throw Failure(ExDataErr, "transaction file escapes the instance base");
    }
    if (dryRun) {
      continue;
    }
    if (file["existed"].toBool()) {
      const QString backup = absoluteClean(file["backup"].toString());
      if (!isWithin(backup, journal)) {
        throw Failure(ExDataErr, "transaction backup escapes its journal");
      }
      writeAtomic(path, readFile(backup));
    } else if (QFileInfo::exists(path) && !QFile::remove(path)) {
      throw Failure(ExIoErr, QString("cannot remove newly created file %1").arg(path));
    }
  }

  if (!dryRun) {
    manifest["rolledBack"] = true;
    writeAtomic(manifestPath, jsonBytes(manifest));
  }
}

}  // namespace

int main(int argc, char* argv[])
{
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("MO2Headless");
  QCoreApplication::setApplicationVersion("0.1.0");

  QCommandLineParser parser;
  parser.setApplicationDescription(
      "Transactional, GUI-free Mod Organizer 2 instance controller");
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addOptions({
      {{"r", "root"},
       "Portable instance root (defaults to executable directory).",
       "path"},
      {{"p", "profile"}, "Profile name.", "name"},
      {"dry-run", "Validate and report without changing state."},
      {"lock-timeout", "Milliseconds to wait for the instance lock.", "ms", "30000"},
      {"game-name", "MO2 game plugin name used by init.", "name",
       "Skyrim Special Edition"},
      {"clone", "Profile to clone.", "profile"},
      {"select", "Select a newly-created profile."},
      {"enable", "Enable a staged/installed mod in the selected profile."},
      {"replace", "Recoverably replace an existing mod directory."},
      {"yes", "Confirm a recoverable trash operation."},
      {"priority", "Priority for a staged/installed mod.", "number"},
      {"arguments", "Arguments for run, passed as one command-line string.", "text"},
      {"cwd", "Working directory for run.", "path"},
      {"overwrite", "Named output mod for a VFS run.", "mod"},
      {"install-plan", "Deterministic JSON file mappings for a FOMOD archive.", "path"},
      {"timeout", "Timeout in seconds for archive extraction or VFS run (0=none).",
       "seconds", "300"},
  });
  parser.addPositionalArgument("command", "Operation to perform.");
  parser.addPositionalArgument("operands", "Operation-specific operands.",
                               "[operands...]");
  parser.process(app);

  const QStringList positional = parser.positionalArguments();
  if (positional.isEmpty()) {
    parser.showHelp(ExUsage);
  }

  const QString command      = positional[0].toLower();
  const QStringList operands = positional.mid(1);
  const QString root         = parser.value("root").isEmpty()
                                   ? QCoreApplication::applicationDirPath()
                                   : parser.value("root");
  const bool dryRun          = parser.isSet("dry-run");

  try {
    const bool init = command == "init";
    InstanceContext context(root, parser.value("profile"), !init);

    if (command == "status") {
      emitJson(contextStatus(context));
      return 0;
    }

    const bool mutating = command != "profile-list" && command != "mod-list" &&
                          command != "plugin-list" && command != "snapshot" &&
                          command != "audit" && command != "status";
    QLockFile lock(QDir(context.root).filePath(".mo2-headless.lock"));
    if (mutating) {
      acquireLock(lock, parser.value("lock-timeout").toInt());
    }

    if (command == "init") {
      if (operands.size() != 1) {
        throw Failure(ExUsage, "init requires GAME_PATH");
      }
      initInstance(context, absoluteClean(operands[0]), parser.value("game-name"),
                   dryRun);
      emitJson({{"ok", true},
                {"operation", "init"},
                {"dryRun", dryRun},
                {"instanceRoot", context.root},
                {"gamePath", absoluteClean(operands[0])}});
      return 0;
    }

    if (command == "profile-list") {
      QJsonArray profiles;
      const QDir dir(context.profiles);
      for (const auto& info : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot,
                                                QDir::Name | QDir::IgnoreCase)) {
        profiles.push_back(QJsonObject{
            {"name", info.fileName()},
            {"selected",
             info.fileName().compare(context.profile, Qt::CaseInsensitive) == 0}});
      }
      emitJson({{"ok", true}, {"operation", "profile-list"}, {"profiles", profiles}});
      return 0;
    }

    if (command == "profile-select") {
      if (operands.size() != 1) {
        throw Failure(ExUsage, "profile-select requires NAME");
      }
      const QString name = operands[0];
      requireSafeName(name, "profile");
      if (!QFileInfo(QDir(context.profiles).filePath(name)).isDir()) {
        throw Failure(ExNoInput, QString("profile does not exist: %1").arg(name));
      }
      Mutation tx(context, command, dryRun);
      tx.write(context.iniPath, readFile(context.iniPath));
      if (!dryRun) {
        QSettings settings(context.iniPath, QSettings::IniFormat);
        settings.setValue("General/selected_profile", name.toUtf8());
        settings.sync();
        if (settings.status() != QSettings::NoError) {
          throw Failure(ExIoErr, "cannot update selected profile");
        }
      }
      tx.commit();
      emitJson({{"ok", true},
                {"operation", command},
                {"profile", name},
                {"transaction", tx.id()},
                {"dryRun", dryRun}});
      return 0;
    }

    if (command == "profile-create") {
      if (operands.size() != 1) {
        throw Failure(ExUsage, "profile-create requires NAME");
      }
      const QString name = operands[0];
      requireSafeName(name, "profile");
      const QString target = QDir(context.profiles).filePath(name);
      if (QFileInfo::exists(target)) {
        throw Failure(ExCantCreat, QString("profile already exists: %1").arg(name));
      }
      Mutation tx(context, command, dryRun);
      const QString stage =
          QDir(QFileInfo(context.profiles).absolutePath())
              .filePath(".mo2-headless-stage-" + tx.id() + "-profile");
      if (!dryRun) {
        const QString clone = parser.value("clone");
        if (!clone.isEmpty()) {
          requireSafeName(clone, "profile");
          const QString source = QDir(context.profiles).filePath(clone);
          if (!copyTree(source, stage)) {
            throw Failure(ExIoErr, QString("cannot clone profile %1").arg(clone));
          }
        } else {
          ensureProfileFiles(stage);
        }
      }
      tx.move(stage, target);
      if (parser.isSet("select")) {
        tx.write(context.iniPath, readFile(context.iniPath));
        if (!dryRun) {
          QSettings settings(context.iniPath, QSettings::IniFormat);
          settings.setValue("General/selected_profile", name.toUtf8());
          settings.sync();
        }
      }
      tx.commit();
      emitJson({{"ok", true},
                {"operation", command},
                {"profile", name},
                {"transaction", tx.id()},
                {"dryRun", dryRun}});
      return 0;
    }

    if (command == "profile-trash") {
      if (operands.size() != 1 || !parser.isSet("yes")) {
        throw Failure(ExUsage, "profile-trash requires NAME and --yes");
      }
      const QString name = operands[0];
      requireSafeName(name, "profile");
      if (name.compare(context.profile, Qt::CaseInsensitive) == 0) {
        throw Failure(ExDataErr, "cannot trash the selected profile");
      }
      const QString existing = QDir(context.profiles).filePath(name);
      if (!QFileInfo(existing).isDir()) {
        throw Failure(ExNoInput, QString("profile does not exist: %1").arg(name));
      }
      Mutation tx(context, command, dryRun);
      const QString trash =
          QDir(QFileInfo(context.profiles).absolutePath())
              .filePath(".mo2-headless-trash/" + tx.id() + "-profile-" + name);
      tx.move(existing, trash);
      tx.commit();
      emitJson({{"ok", true},
                {"operation", command},
                {"profile", name},
                {"trash", trash},
                {"transaction", tx.id()},
                {"dryRun", dryRun}});
      return 0;
    }

    context.requireProfile();

    if (command == "mod-list") {
      emitJson({{"ok", true},
                {"operation", command},
                {"profile", context.profile},
                {"mods", modEntriesJson(readModList(context.modListPath()))}});
      return 0;
    }

    if (command == "mod-enable" || command == "mod-disable" ||
        command == "mod-priority") {
      const int expected = command == "mod-priority" ? 2 : 1;
      if (operands.size() != expected) {
        throw Failure(ExUsage, command + " received the wrong number of operands");
      }
      const QString name = operands[0];
      requireSafeName(name, "mod");
      if (findModDirectory(context, name).isEmpty()) {
        throw Failure(ExNoInput, QString("mod directory does not exist: %1").arg(name));
      }
      auto entries = readModList(context.modListPath());
      int index    = findModEntry(entries, name);
      if (index < 0) {
        entries.prepend({name, false, '-'});
        index = 0;
      }
      if (command == "mod-priority") {
        bool okay    = false;
        int priority = operands[1].toInt(&okay);
        if (!okay || priority < 0) {
          throw Failure(ExDataErr, "mod priority must be a non-negative integer");
        }
        const auto entry    = entries.takeAt(index);
        priority            = std::min(priority, entries.size());
        const int fileIndex = entries.size() - priority;
        entries.insert(fileIndex, entry);
      } else {
        entries[index].enabled = command == "mod-enable";
        entries[index].marker  = entries[index].enabled ? '+' : '-';
      }
      Mutation tx(context, command, dryRun);
      tx.write(context.modListPath(), serializeModList(entries));
      tx.commit();
      emitJson({{"ok", true},
                {"operation", command},
                {"mod", name},
                {"transaction", tx.id()},
                {"dryRun", dryRun}});
      return 0;
    }

    if (command == "mod-stage" || command == "mod-install") {
      if (operands.size() != 2) {
        throw Failure(ExUsage, command + " requires SOURCE_OR_ARCHIVE NAME");
      }
      const QString source = absoluteClean(operands[0]);
      const QString name   = operands[1];
      requireSafeName(name, "mod");
      if (!QFileInfo::exists(source)) {
        throw Failure(ExNoInput, QString("source does not exist: %1").arg(source));
      }
      Mutation tx(context, command, dryRun);
      const QString stageParent = QDir(QFileInfo(context.mods).absolutePath())
                                      .filePath(".mo2-headless-stage-" + tx.id());
      const QString stage       = QDir(stageParent).filePath("content");
      const QString target      = QDir(context.mods).filePath(name);
      const QString existing    = findModDirectory(context, name);
      if (!existing.isEmpty() && !parser.isSet("replace")) {
        throw Failure(ExCantCreat, QString("mod already exists: %1").arg(name));
      }

      if (!dryRun) {
        if (!QDir().mkpath(stage)) {
          throw Failure(ExCantCreat, QString("cannot create stage %1").arg(stage));
        }
        if (command == "mod-stage") {
          if (!QFileInfo(source).isDir() || !copyTree(source, stage)) {
            throw Failure(ExIoErr, "cannot copy source directory or it contains links");
          }
        } else {
          const int seconds        = parser.value("timeout").toInt();
          const int timeout        = seconds <= 0 ? -1 : seconds * 1000;
          const QByteArray listing = runProcess("tar.exe", {"-tf", source}, timeout);
          validateArchiveListing(listing);
          runProcess("tar.exe", {"-xf", source, "-C", stage}, timeout);
        }
        validateExtractedTree(stage);
        const bool fomod = containsFomod(stage);
        if (fomod && !parser.isSet("install-plan")) {
          throw Failure(ExDataErr,
                        "archive contains a FOMOD; --install-plan is required");
        }
      }

      QString installRoot = stage;
      if (parser.isSet("install-plan")) {
        const QString plan = absoluteClean(parser.value("install-plan"));
        if (!QFileInfo::exists(plan)) {
          throw Failure(ExNoInput,
                        QString("install plan does not exist: %1").arg(plan));
        }
        installRoot = QDir(stageParent).filePath("resolved");
        if (!dryRun) {
          applyInstallPlan(stage, plan, installRoot);
        }
      }
      QString contentRoot = dryRun ? installRoot : normalizedContentRoot(installRoot);
      if (!existing.isEmpty()) {
        const QString trash = QDir(QFileInfo(context.mods).absolutePath())
                                  .filePath(".mo2-headless-trash/" + tx.id() + "-" +
                                            QFileInfo(existing).fileName());
        tx.move(existing, trash);
      }
      tx.move(contentRoot, target);

      if (!dryRun) {
        QSettings meta(QDir(target).filePath("meta.ini"), QSettings::IniFormat);
        meta.setValue("General/installationFile", source);
        meta.setValue("General/installedBy", "MO2Headless");
        meta.setValue("General/installedAt",
                      QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        meta.sync();
      }

      auto entries = readModList(context.modListPath());
      int index    = findModEntry(entries, name);
      if (index >= 0) {
        entries.removeAt(index);
      }
      ModEntry entry{name, parser.isSet("enable"), parser.isSet("enable") ? '+' : '-'};
      int priority = entries.size();
      if (parser.isSet("priority")) {
        bool okay = false;
        priority  = parser.value("priority").toInt(&okay);
        if (!okay || priority < 0) {
          throw Failure(ExDataErr, "priority must be a non-negative integer");
        }
        priority = std::min(priority, entries.size());
      }
      entries.insert(entries.size() - priority, entry);
      tx.write(context.modListPath(), serializeModList(entries));
      tx.commit();
      emitJson({{"ok", true},
                {"operation", command},
                {"mod", name},
                {"enabled", entry.enabled},
                {"priority", priority},
                {"transaction", tx.id()},
                {"dryRun", dryRun}});
      return 0;
    }

    if (command == "mod-trash") {
      if (operands.size() != 1 || !parser.isSet("yes")) {
        throw Failure(ExUsage, "mod-trash requires NAME and --yes");
      }
      const QString name = operands[0];
      requireSafeName(name, "mod");
      const QString existing = findModDirectory(context, name);
      if (existing.isEmpty()) {
        throw Failure(ExNoInput, QString("mod does not exist: %1").arg(name));
      }
      Mutation tx(context, command, dryRun);
      const QString trash = QDir(QFileInfo(context.mods).absolutePath())
                                .filePath(".mo2-headless-trash/" + tx.id() + "-" +
                                          QFileInfo(existing).fileName());
      tx.move(existing, trash);
      const QDir profiles(context.profiles);
      for (const auto& profileInfo :
           profiles.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString listPath =
            QDir(profileInfo.absoluteFilePath()).filePath("modlist.txt");
        auto entries    = readModList(listPath);
        const int index = findModEntry(entries, name);
        if (index >= 0) {
          entries.removeAt(index);
          tx.write(listPath, serializeModList(entries));
        }
      }
      tx.commit();
      emitJson({{"ok", true},
                {"operation", command},
                {"mod", name},
                {"trash", trash},
                {"transaction", tx.id()},
                {"dryRun", dryRun}});
      return 0;
    }

    if (command == "plugin-list") {
      const auto entries = readPluginList(context.pluginsPath());
      const auto discovered =
          discoverPlugins(context, readModList(context.modListPath()));
      QJsonArray plugins = pluginEntriesJson(entries);
      emitJson({{"ok", true},
                {"operation", command},
                {"profile", context.profile},
                {"plugins", plugins},
                {"discoveredCount", discovered.size()}});
      return 0;
    }

    if (command == "snapshot") {
      const auto mods    = readModList(context.modListPath());
      const auto plugins = readPluginList(context.pluginsPath());
      emitJson({{"ok", true},
                {"operation", command},
                {"schemaVersion", 1},
                {"instanceRoot", context.root},
                {"profile", context.profile},
                {"mods", modEntriesJson(mods)},
                {"plugins", pluginEntriesJson(plugins)}});
      return 0;
    }

    if (command == "apply") {
      if (operands.size() != 1) {
        throw Failure(ExUsage, "apply requires MANIFEST_JSON");
      }
      const QString manifestPath = absoluteClean(operands[0]);
      const auto document        = QJsonDocument::fromJson(readFile(manifestPath));
      if (!document.isObject()) {
        throw Failure(ExDataErr, "state manifest must be a JSON object");
      }
      const auto object = document.object();
      if (object["schemaVersion"].toInt() != 1) {
        throw Failure(ExDataErr, "unsupported state manifest schemaVersion");
      }
      if (object.contains("profile") &&
          object["profile"].toString().compare(context.profile, Qt::CaseInsensitive) !=
              0) {
        throw Failure(ExDataErr, "manifest profile does not match selected profile");
      }

      QList<QPair<int, ModEntry>> plannedMods;
      QSet<QString> modNames;
      for (const auto& value : object["mods"].toArray()) {
        const auto mod     = value.toObject();
        const QString name = mod["name"].toString();
        requireSafeName(name, "mod");
        if (modNames.contains(name.toLower())) {
          throw Failure(ExDataErr, QString("duplicate mod in manifest: %1").arg(name));
        }
        if (findModDirectory(context, name).isEmpty()) {
          throw Failure(ExNoInput,
                        QString("manifest mod is not installed: %1").arg(name));
        }
        modNames.insert(name.toLower());
        plannedMods.push_back(
            {mod["priority"].toInt(-1), ModEntry{name, mod["enabled"].toBool(),
                                                 mod["enabled"].toBool() ? '+' : '-'}});
      }
      std::sort(plannedMods.begin(), plannedMods.end(),
                [](const auto& a, const auto& b) {
                  return a.first > b.first;
                });
      QList<ModEntry> modEntries;
      int previousPriority = std::numeric_limits<int>::max();
      for (const auto& [priority, entry] : plannedMods) {
        if (priority < 0 || priority >= previousPriority) {
          throw Failure(ExDataErr, "mod priorities must be unique non-negative values");
        }
        previousPriority = priority;
        modEntries.push_back(entry);
      }

      QList<QPair<int, PluginEntry>> plannedPlugins;
      QSet<QString> pluginNames;
      const auto discovered = discoverPlugins(context, modEntries);
      for (const auto& value : object["plugins"].toArray()) {
        const auto plugin  = value.toObject();
        const QString name = plugin["name"].toString();
        requireSafeName(name, "plugin");
        if (pluginNames.contains(name.toLower())) {
          throw Failure(ExDataErr,
                        QString("duplicate plugin in manifest: %1").arg(name));
        }
        if (!discovered.contains(name.toLower())) {
          throw Failure(
              ExNoInput,
              QString("manifest plugin is not in the effective tree: %1").arg(name));
        }
        pluginNames.insert(name.toLower());
        plannedPlugins.push_back({plugin["priority"].toInt(-1),
                                  PluginEntry{name, plugin["enabled"].toBool()}});
      }
      std::sort(plannedPlugins.begin(), plannedPlugins.end(),
                [](const auto& a, const auto& b) {
                  return a.first < b.first;
                });
      QList<PluginEntry> pluginEntries;
      previousPriority = -1;
      for (const auto& [priority, entry] : plannedPlugins) {
        if (priority < 0 || priority <= previousPriority) {
          throw Failure(ExDataErr,
                        "plugin priorities must be unique non-negative values");
        }
        previousPriority = priority;
        pluginEntries.push_back(entry);
      }

      Mutation tx(context, command, dryRun);
      tx.write(context.modListPath(), serializeModList(modEntries));
      tx.write(context.pluginsPath(), serializePluginList(pluginEntries));
      QByteArray loadOrder;
      for (const auto& entry : pluginEntries) {
        loadOrder += entry.name.toUtf8() + "\r\n";
      }
      tx.write(context.loadOrderPath(), loadOrder);
      tx.commit();
      emitJson({{"ok", true},
                {"operation", command},
                {"profile", context.profile},
                {"modCount", modEntries.size()},
                {"pluginCount", pluginEntries.size()},
                {"transaction", tx.id()},
                {"dryRun", dryRun}});
      return 0;
    }

    if (command == "plugin-enable" || command == "plugin-disable" ||
        command == "plugin-priority") {
      const int expected = command == "plugin-priority" ? 2 : 1;
      if (operands.size() != expected) {
        throw Failure(ExUsage, command + " received the wrong number of operands");
      }
      const QString name = operands[0];
      requireSafeName(name, "plugin");
      const auto mods       = readModList(context.modListPath());
      const auto discovered = discoverPlugins(context, mods);
      if (!discovered.contains(name.toLower())) {
        throw Failure(
            ExNoInput,
            QString("plugin is not present in the effective data tree: %1").arg(name));
      }
      auto entries = readPluginList(context.pluginsPath());
      int index    = findPluginEntry(entries, name);
      if (index < 0) {
        entries.push_back({name, false});
        index = entries.size() - 1;
      }
      if (command == "plugin-priority") {
        bool okay    = false;
        int priority = operands[1].toInt(&okay);
        if (!okay || priority < 0) {
          throw Failure(ExDataErr, "plugin priority must be a non-negative integer");
        }
        const auto entry = entries.takeAt(index);
        priority         = std::min(priority, entries.size());
        entries.insert(priority, entry);
      } else {
        entries[index].enabled = command == "plugin-enable";
      }
      Mutation tx(context, command, dryRun);
      tx.write(context.pluginsPath(), serializePluginList(entries));
      QByteArray loadOrder;
      for (const auto& entry : entries) {
        loadOrder += entry.name.toUtf8() + "\r\n";
      }
      tx.write(context.loadOrderPath(), loadOrder);
      tx.commit();
      emitJson({{"ok", true},
                {"operation", command},
                {"plugin", name},
                {"transaction", tx.id()},
                {"dryRun", dryRun}});
      return 0;
    }

    if (command == "audit") {
      QStringList errors;
      const auto mods = readModList(context.modListPath());
      QSet<QString> listedMods;
      for (const auto& mod : mods) {
        listedMods.insert(mod.name.toLower());
        if (findModDirectory(context, mod.name).isEmpty()) {
          errors.push_back("modlist references missing directory: " + mod.name);
        }
      }
      const QDir modsDirectory(context.mods);
      for (const auto& info : modsDirectory.entryInfoList(
               QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase)) {
        if (!listedMods.contains(info.fileName().toLower())) {
          errors.push_back("installed mod is absent from modlist: " + info.fileName());
        }
      }
      const auto discovered = discoverPlugins(context, mods);
      const auto plugins    = readPluginList(context.pluginsPath());
      for (const auto& plugin : plugins) {
        if (!discovered.contains(plugin.name.toLower())) {
          errors.push_back("plugins.txt references missing plugin: " + plugin.name);
        }
      }
      QJsonArray jsonErrors;
      for (const auto& error : errors) {
        jsonErrors.push_back(error);
      }
      emitJson({{"ok", errors.isEmpty()},
                {"operation", command},
                {"profile", context.profile},
                {"errors", jsonErrors}},
               !errors.isEmpty());
      return errors.isEmpty() ? 0 : ExDataErr;
    }

    if (command == "rollback") {
      if (operands.size() != 1) {
        throw Failure(ExUsage, "rollback requires TRANSACTION_ID");
      }
      restoreTransaction(context, operands[0], dryRun);
      emitJson({{"ok", true},
                {"operation", command},
                {"transaction", operands[0]},
                {"dryRun", dryRun}});
      return 0;
    }

    if (command == "run") {
      if (operands.size() != 1) {
        throw Failure(ExUsage, "run requires PROGRAM");
      }
      const QString mo = QDir(context.root).filePath("ModOrganizer.exe");
      if (!QFileInfo::exists(mo)) {
        throw Failure(ExNoInput,
                      QString("ModOrganizer.exe not found in %1").arg(context.root));
      }
      QStringList arguments{"-p", context.profile, "headless-run"};
      if (parser.isSet("arguments")) {
        arguments << "--arguments" << parser.value("arguments");
      }
      if (parser.isSet("cwd")) {
        arguments << "--cwd" << parser.value("cwd");
      }
      if (parser.isSet("overwrite")) {
        arguments << "--overwrite" << parser.value("overwrite");
      }
      arguments << operands[0];

      QProcess process;
      process.setProgram(mo);
      process.setArguments(arguments);
      process.setWorkingDirectory(context.root);
      process.start();
      if (!process.waitForStarted(30000)) {
        throw Failure(
            ExNoInput,
            QString("cannot start ModOrganizer.exe: %1").arg(process.errorString()));
      }
      const int seconds = parser.value("timeout").toInt();
      const int timeout = seconds <= 0 ? -1 : seconds * 1000;
      if (!process.waitForFinished(timeout)) {
        process.kill();
        process.waitForFinished(10000);
        throw Failure(ExTempFail, "VFS process timed out");
      }
      const QByteArray childOut = process.readAllStandardOutput();
      const QByteArray childErr = process.readAllStandardError();
      const int exitCode        = process.exitCode();
      emitJson({{"ok", process.exitStatus() == QProcess::NormalExit && exitCode == 0},
                {"operation", command},
                {"program", operands[0]},
                {"exitCode", exitCode},
                {"stdout", QString::fromUtf8(childOut)},
                {"stderr", QString::fromUtf8(childErr)}},
               exitCode != 0);
      return exitCode;
    }

    throw Failure(ExUsage, QString("unknown command: %1").arg(command));
  } catch (const Failure& e) {
    emitJson({{"ok", false},
              {"operation", command},
              {"error", QString::fromUtf8(e.what())},
              {"exitCode", e.code()}},
             true);
    return e.code();
  } catch (const std::exception& e) {
    emitJson({{"ok", false},
              {"operation", command},
              {"error", QString::fromUtf8(e.what())},
              {"exitCode", 70}},
             true);
    return 70;
  }
}
