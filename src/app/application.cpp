/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2015  Vladimir Golovnev <glassez@yandex.ru>
 * Copyright (C) 2006  Christophe Dumez
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "application.h"

#include <QtGlobal>

#include <algorithm>

#ifdef DISABLE_GUI
#include <cstdio>
#endif

#ifdef Q_OS_WIN
#include <memory>
#include <Windows.h>
#include <Shellapi.h>
#endif

#include <QAtomicInt>
#include <QDebug>
#include <QDir>
#include <QLibraryInfo>
#include <QProcess>

#ifndef DISABLE_GUI
#include <QMessageBox>
#include <QPixmapCache>
#ifdef Q_OS_WIN
#include <QSessionManager>
#include <QSharedMemory>
#endif // Q_OS_WIN
#ifdef Q_OS_MACOS
#include <QFileOpenEvent>
#endif // Q_OS_MACOS
#endif

#include "base/bittorrent/infohash.h"
#include "base/bittorrent/session.h"
#include "base/bittorrent/torrent.h"
#include "base/exceptions.h"
#include "base/iconprovider.h"
#include "base/logger.h"
#include "base/net/downloadmanager.h"
#include "base/net/geoipmanager.h"
#include "base/net/proxyconfigurationmanager.h"
#include "base/net/smtp.h"
#include "base/preferences.h"
#include "base/profile.h"
#include "base/rss/rss_autodownloader.h"
#include "base/rss/rss_session.h"
#include "base/search/searchpluginmanager.h"
#include "base/settingsstorage.h"
#include "base/torrentfileswatcher.h"
#include "base/utils/compare.h"
#include "base/utils/fs.h"
#include "base/utils/misc.h"
#include "base/version.h"
#include "applicationinstancemanager.h"
#include "filelogger.h"

#ifndef DISABLE_GUI
#include "gui/addnewtorrentdialog.h"
#include "gui/uithememanager.h"
#include "gui/utils.h"
#include "gui/mainwindow.h"
#include "gui/shutdownconfirmdialog.h"
#endif // DISABLE_GUI

#ifndef DISABLE_WEBUI
#include "webui/webui.h"
#endif

namespace
{
#define SETTINGS_KEY(name) "Application/" name
#define FILELOGGER_SETTINGS_KEY(name) (SETTINGS_KEY("FileLogger/") name)

    const QString LOG_FOLDER = QStringLiteral("logs");
    const QChar PARAMS_SEPARATOR = QLatin1Char('|');

    const QString DEFAULT_PORTABLE_MODE_PROFILE_DIR = QStringLiteral("profile");

    const int MIN_FILELOG_SIZE = 1024; // 1KiB
    const int MAX_FILELOG_SIZE = 1000 * 1024 * 1024; // 1000MiB
    const int DEFAULT_FILELOG_SIZE = 65 * 1024; // 65KiB

#if !defined(DISABLE_GUI)
    const int PIXMAP_CACHE_SIZE = 64 * 1024 * 1024;  // 64MiB
#endif
}

Application::Application(int &argc, char **argv)
    : BaseApplication(argc, argv)
    , m_running(false)
    , m_shutdownAct(ShutdownDialogAction::Exit)
    , m_commandLineArgs(parseCommandLine(this->arguments()))
    , m_storeFileLoggerEnabled(FILELOGGER_SETTINGS_KEY("Enabled"))
    , m_storeFileLoggerBackup(FILELOGGER_SETTINGS_KEY("Backup"))
    , m_storeFileLoggerDeleteOld(FILELOGGER_SETTINGS_KEY("DeleteOld"))
    , m_storeFileLoggerMaxSize(FILELOGGER_SETTINGS_KEY("MaxSizeBytes"))
    , m_storeFileLoggerAge(FILELOGGER_SETTINGS_KEY("Age"))
    , m_storeFileLoggerAgeType(FILELOGGER_SETTINGS_KEY("AgeType"))
    , m_storeFileLoggerPath(FILELOGGER_SETTINGS_KEY("Path"))
{
    qRegisterMetaType<Log::Msg>("Log::Msg");
    qRegisterMetaType<Log::Peer>("Log::Peer");

    setApplicationName("qBittorrent");
    setOrganizationDomain("qbittorrent.org");
#if !defined(DISABLE_GUI)
    setDesktopFileName("org.qbittorrent.qBittorrent");
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    setAttribute(Qt::AA_UseHighDpiPixmaps, true);  // opt-in to the high DPI pixmap support
#endif
    setQuitOnLastWindowClosed(false);
    QPixmapCache::setCacheLimit(PIXMAP_CACHE_SIZE);
#endif

    const bool portableModeEnabled = m_commandLineArgs.profileDir.isEmpty()
            && QDir(QCoreApplication::applicationDirPath()).exists(DEFAULT_PORTABLE_MODE_PROFILE_DIR);

    const QString profileDir = portableModeEnabled
        ? QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(DEFAULT_PORTABLE_MODE_PROFILE_DIR)
        : m_commandLineArgs.profileDir;
    Profile::initInstance(profileDir, m_commandLineArgs.configurationName,
                        (m_commandLineArgs.relativeFastresumePaths || portableModeEnabled));

    m_instanceManager = new ApplicationInstanceManager {Profile::instance()->location(SpecialFolder::Config), this};

    Logger::initInstance();
    SettingsStorage::initInstance();
    Preferences::initInstance();

    initializeTranslation();

    if (m_commandLineArgs.webUiPort > 0) // it will be -1 when user did not set any value
        Preferences::instance()->setWebUiPort(m_commandLineArgs.webUiPort);

    connect(this, &QCoreApplication::aboutToQuit, this, &Application::cleanup);
    connect(m_instanceManager, &ApplicationInstanceManager::messageReceived, this, &Application::processMessage);
#if defined(Q_OS_WIN) && !defined(DISABLE_GUI)
    connect(this, &QGuiApplication::commitDataRequest, this, &Application::shutdownCleanup, Qt::DirectConnection);
#endif

    if (isFileLoggerEnabled())
        m_fileLogger = new FileLogger(fileLoggerPath(), isFileLoggerBackup(), fileLoggerMaxSize(), isFileLoggerDeleteOld(), fileLoggerAge(), static_cast<FileLogger::FileLogAgeType>(fileLoggerAgeType()));

    Logger::instance()->addMessage(tr("qBittorrent %1 started", "qBittorrent v3.2.0alpha started").arg(QBT_VERSION));
    if (portableModeEnabled)
    {
        Logger::instance()->addMessage(tr("Running in portable mode. Auto detected profile folder at: %1").arg(profileDir));
        if (m_commandLineArgs.relativeFastresumePaths)
            Logger::instance()->addMessage(tr("Redundant command line flag detected: \"%1\". Portable mode implies relative fastresume.").arg("--relative-fastresume"), Log::WARNING); // to avoid translating the `--relative-fastresume` string
    }
    else
    {
        Logger::instance()->addMessage(tr("Using config directory: %1").arg(Profile::instance()->location(SpecialFolder::Config)));
    }
}

Application::~Application()
{
    // we still need to call cleanup()
    // in case the App failed to start
    cleanup();
}

#ifndef DISABLE_GUI
QPointer<MainWindow> Application::mainWindow()
{
    return m_window;
}
#endif

const QBtCommandLineParameters &Application::commandLineArgs() const
{
    return m_commandLineArgs;
}

bool Application::isFileLoggerEnabled() const
{
    return m_storeFileLoggerEnabled.get(true);
}

void Application::setFileLoggerEnabled(const bool value)
{
    if (value && !m_fileLogger)
        m_fileLogger = new FileLogger(fileLoggerPath(), isFileLoggerBackup(), fileLoggerMaxSize(), isFileLoggerDeleteOld(), fileLoggerAge(), static_cast<FileLogger::FileLogAgeType>(fileLoggerAgeType()));
    else if (!value)
        delete m_fileLogger;
    m_storeFileLoggerEnabled = value;
}

QString Application::fileLoggerPath() const
{
    return m_storeFileLoggerPath.get(QDir(specialFolderLocation(SpecialFolder::Data)).absoluteFilePath(LOG_FOLDER));
}

void Application::setFileLoggerPath(const QString &path)
{
    if (m_fileLogger)
        m_fileLogger->changePath(path);
    m_storeFileLoggerPath = path;
}

bool Application::isFileLoggerBackup() const
{
    return m_storeFileLoggerBackup.get(true);
}

void Application::setFileLoggerBackup(const bool value)
{
    if (m_fileLogger)
        m_fileLogger->setBackup(value);
    m_storeFileLoggerBackup = value;
}

bool Application::isFileLoggerDeleteOld() const
{
    return m_storeFileLoggerDeleteOld.get(true);
}

void Application::setFileLoggerDeleteOld(const bool value)
{
    if (value && m_fileLogger)
        m_fileLogger->deleteOld(fileLoggerAge(), static_cast<FileLogger::FileLogAgeType>(fileLoggerAgeType()));
    m_storeFileLoggerDeleteOld = value;
}

int Application::fileLoggerMaxSize() const
{
    const int val = m_storeFileLoggerMaxSize.get(DEFAULT_FILELOG_SIZE);
    return std::min(std::max(val, MIN_FILELOG_SIZE), MAX_FILELOG_SIZE);
}

void Application::setFileLoggerMaxSize(const int bytes)
{
    const int clampedValue = std::min(std::max(bytes, MIN_FILELOG_SIZE), MAX_FILELOG_SIZE);
    if (m_fileLogger)
        m_fileLogger->setMaxSize(clampedValue);
    m_storeFileLoggerMaxSize = clampedValue;
}

int Application::fileLoggerAge() const
{
    const int val = m_storeFileLoggerAge.get(1);
    return std::min(std::max(val, 1), 365);
}

void Application::setFileLoggerAge(const int value)
{
    m_storeFileLoggerAge = std::min(std::max(value, 1), 365);
}

int Application::fileLoggerAgeType() const
{
    const int val = m_storeFileLoggerAgeType.get(1);
    return ((val < 0) || (val > 2)) ? 1 : val;
}

void Application::setFileLoggerAgeType(const int value)
{
    m_storeFileLoggerAgeType = ((value < 0) || (value > 2)) ? 1 : value;
}

void Application::processMessage(const QString &message)
{
    const QStringList params = message.split(PARAMS_SEPARATOR, Qt::SkipEmptyParts);
    // If Application is not running (i.e., other
    // components are not ready) store params
    if (m_running)
        processParams(params);
    else
        m_paramsQueue.append(params);
}

void Application::runExternalProgram(const BitTorrent::Torrent *torrent) const
{
#if defined(Q_OS_WIN)
    const auto chopPathSep = [](const QString &str) -> QString
    {
        if (str.endsWith('\\'))
            return str.mid(0, (str.length() -1));
        return str;
    };
#endif

    QString program = Preferences::instance()->getAutoRunProgram().trimmed();

    for (int i = (program.length() - 2); i >= 0; --i)
    {
        if (program[i] != QLatin1Char('%'))
            continue;

        const ushort specifier = program[i + 1].unicode();
        switch (specifier)
        {
        case u'C':
            program.replace(i, 2, QString::number(torrent->filesCount()));
            break;
        case u'D':
#if defined(Q_OS_WIN)
            program.replace(i, 2, chopPathSep(Utils::Fs::toNativePath(torrent->savePath())));
#else
            program.replace(i, 2, Utils::Fs::toNativePath(torrent->savePath()));
#endif
            break;
        case u'F':
#if defined(Q_OS_WIN)
            program.replace(i, 2, chopPathSep(Utils::Fs::toNativePath(torrent->contentPath())));
#else
            program.replace(i, 2, Utils::Fs::toNativePath(torrent->contentPath()));
#endif
            break;
        case u'G':
            program.replace(i, 2, torrent->tags().join(QLatin1String(",")));
            break;
        case u'I':
            program.replace(i, 2, (torrent->infoHash().v1().isValid() ? torrent->infoHash().v1().toString() : QLatin1String("-")));
            break;
        case u'J':
            program.replace(i, 2, (torrent->infoHash().v2().isValid() ? torrent->infoHash().v2().toString() : QLatin1String("-")));
            break;
        case u'K':
            program.replace(i, 2, torrent->id().toString());
            break;
        case u'L':
            program.replace(i, 2, torrent->category());
            break;
        case u'N':
            program.replace(i, 2, torrent->name());
            break;
        case u'R':
#if defined(Q_OS_WIN)
            program.replace(i, 2, chopPathSep(Utils::Fs::toNativePath(torrent->rootPath())));
#else
            program.replace(i, 2, Utils::Fs::toNativePath(torrent->rootPath()));
#endif
            break;
        case u'T':
            program.replace(i, 2, torrent->currentTracker());
            break;
        case u'Z':
            program.replace(i, 2, QString::number(torrent->totalSize()));
            break;
        default:
            // do nothing
            break;
        }

        // decrement `i` to avoid unwanted replacement, example pattern: "%%N"
        --i;
    }

    LogMsg(tr("Torrent: %1, running external program, command: %2").arg(torrent->name(), program));

#if defined(Q_OS_WIN)
    auto programWchar = std::make_unique<wchar_t[]>(program.length() + 1);
    program.toWCharArray(programWchar.get());

    // Need to split arguments manually because QProcess::startDetached(QString)
    // will strip off empty parameters.
    // E.g. `python.exe "1" "" "3"` will become `python.exe "1" "3"`
    int argCount = 0;
    std::unique_ptr<LPWSTR[], decltype(&::LocalFree)> args {::CommandLineToArgvW(programWchar.get(), &argCount), ::LocalFree};

    QStringList argList;
    for (int i = 1; i < argCount; ++i)
        argList += QString::fromWCharArray(args[i]);

    QProcess proc;
    proc.setProgram(QString::fromWCharArray(args[0]));
    proc.setArguments(argList);
    proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args)
    {
        if (Preferences::instance()->isAutoRunConsoleEnabled())
        {
            args->flags |= CREATE_NEW_CONSOLE;
            args->flags &= ~(CREATE_NO_WINDOW | DETACHED_PROCESS);
        }
        else
        {
            args->flags |= CREATE_NO_WINDOW;
            args->flags &= ~(CREATE_NEW_CONSOLE | DETACHED_PROCESS);
        }
        args->inheritHandles = false;
        args->startupInfo->dwFlags &= ~STARTF_USESTDHANDLES;
        ::CloseHandle(args->startupInfo->hStdInput);
        ::CloseHandle(args->startupInfo->hStdOutput);
        ::CloseHandle(args->startupInfo->hStdError);
        args->startupInfo->hStdInput = nullptr;
        args->startupInfo->hStdOutput = nullptr;
        args->startupInfo->hStdError = nullptr;
    });
    proc.startDetached();
#else // Q_OS_WIN
    // Cannot give users shell environment by default, as doing so could
    // enable command injection via torrent name and other arguments
    // (especially when some automated download mechanism has been setup).
    // See: https://github.com/qbittorrent/qBittorrent/issues/10925
    QStringList args = QProcess::splitCommand(program);
    if (args.isEmpty())
        return;

    const QString command = args.takeFirst();
    QProcess::startDetached(command, args);
#endif
}

void Application::sendNotificationEmail(const BitTorrent::Torrent *torrent)
{
    // Prepare mail content
    const QString content = tr("Torrent name: %1").arg(torrent->name()) + '\n'
        + tr("Torrent size: %1").arg(Utils::Misc::friendlyUnit(torrent->wantedSize())) + '\n'
        + tr("Save path: %1").arg(torrent->savePath()) + "\n\n"
        + tr("The torrent was downloaded in %1.", "The torrent was downloaded in 1 hour and 20 seconds")
            .arg(Utils::Misc::userFriendlyDuration(torrent->activeTime())) + "\n\n\n"
        + tr("Thank you for using qBittorrent.") + '\n';

    // Send the notification email
    const Preferences *pref = Preferences::instance();
    auto *smtp = new Net::Smtp(this);
    smtp->sendMail(pref->getMailNotificationSender(),
                     pref->getMailNotificationEmail(),
                     tr("[qBittorrent] '%1' has finished downloading").arg(torrent->name()),
                     content);
}

void Application::torrentFinished(BitTorrent::Torrent *const torrent)
{
    Preferences *const pref = Preferences::instance();

    // AutoRun program
    if (pref->isAutoRunEnabled())
        runExternalProgram(torrent);

    // Mail notification
    if (pref->isMailNotificationEnabled())
    {
        Logger::instance()->addMessage(tr("Torrent: %1, sending mail notification").arg(torrent->name()));
        sendNotificationEmail(torrent);
    }
}

void Application::allTorrentsFinished()
{
    Preferences *const pref = Preferences::instance();
    bool isExit = pref->shutdownqBTWhenDownloadsComplete();
    bool isShutdown = pref->shutdownWhenDownloadsComplete();
    bool isSuspend = pref->suspendWhenDownloadsComplete();
    bool isHibernate = pref->hibernateWhenDownloadsComplete();

    bool haveAction = isExit || isShutdown || isSuspend || isHibernate;
    if (!haveAction) return;

    ShutdownDialogAction action = ShutdownDialogAction::Exit;
    if (isSuspend)
        action = ShutdownDialogAction::Suspend;
    else if (isHibernate)
        action = ShutdownDialogAction::Hibernate;
    else if (isShutdown)
        action = ShutdownDialogAction::Shutdown;

#ifndef DISABLE_GUI
    // ask confirm
    if ((action == ShutdownDialogAction::Exit) && (pref->dontConfirmAutoExit()))
    {
        // do nothing & skip confirm
    }
    else
    {
        if (!ShutdownConfirmDialog::askForConfirmation(m_window, action)) return;
    }
#endif // DISABLE_GUI

    // Actually shut down
    if (action != ShutdownDialogAction::Exit)
    {
        qDebug("Preparing for auto-shutdown because all downloads are complete!");
        // Disabling it for next time
        pref->setShutdownWhenDownloadsComplete(false);
        pref->setSuspendWhenDownloadsComplete(false);
        pref->setHibernateWhenDownloadsComplete(false);
        // Make sure preferences are synced before exiting
        m_shutdownAct = action;
    }

    qDebug("Exiting the application");
    exit();
}

bool Application::sendParams(const QStringList &params)
{
    return m_instanceManager->sendMessage(params.join(PARAMS_SEPARATOR));
}

// As program parameters, we can get paths or urls.
// This function parse the parameters and call
// the right addTorrent function, considering
// the parameter type.
void Application::processParams(const QStringList &params)
{
#ifndef DISABLE_GUI
    if (params.isEmpty())
    {
        m_window->activate(); // show UI
        return;
    }
#endif
    BitTorrent::AddTorrentParams torrentParams;
    std::optional<bool> skipTorrentDialog;

    for (QString param : params)
    {
        param = param.trimmed();

        // Process strings indicating options specified by the user.

        if (param.startsWith(QLatin1String("@savePath=")))
        {
            torrentParams.savePath = param.mid(10);
            continue;
        }

        if (param.startsWith(QLatin1String("@addPaused=")))
        {
            torrentParams.addPaused = (QStringView(param).mid(11).toInt() != 0);
            continue;
        }

        if (param == QLatin1String("@skipChecking"))
        {
            torrentParams.skipChecking = true;
            continue;
        }

        if (param.startsWith(QLatin1String("@category=")))
        {
            torrentParams.category = param.mid(10);
            continue;
        }

        if (param == QLatin1String("@sequential"))
        {
            torrentParams.sequential = true;
            continue;
        }

        if (param == QLatin1String("@firstLastPiecePriority"))
        {
            torrentParams.firstLastPiecePriority = true;
            continue;
        }

        if (param.startsWith(QLatin1String("@skipDialog=")))
        {
            skipTorrentDialog = (QStringView(param).mid(12).toInt() != 0);
            continue;
        }

#ifndef DISABLE_GUI
        // There are two circumstances in which we want to show the torrent
        // dialog. One is when the application settings specify that it should
        // be shown and skipTorrentDialog is undefined. The other is when
        // skipTorrentDialog is false, meaning that the application setting
        // should be overridden.
        const bool showDialogForThisTorrent = !skipTorrentDialog.value_or(!AddNewTorrentDialog::isEnabled());
        if (showDialogForThisTorrent)
            AddNewTorrentDialog::show(param, torrentParams, m_window);
        else
#endif
            BitTorrent::Session::instance()->addTorrent(param, torrentParams);
    }
}

int Application::exec(const QStringList &params)
{
    Net::ProxyConfigurationManager::initInstance();
    Net::DownloadManager::initInstance();
    IconProvider::initInstance();

    try
    {
        BitTorrent::Session::initInstance();
        connect(BitTorrent::Session::instance(), &BitTorrent::Session::torrentFinished, this, &Application::torrentFinished);
        connect(BitTorrent::Session::instance(), &BitTorrent::Session::allTorrentsFinished, this, &Application::allTorrentsFinished, Qt::QueuedConnection);

        Net::GeoIPManager::initInstance();
        TorrentFilesWatcher::initInstance();

#ifndef DISABLE_WEBUI
        m_webui = new WebUI;
#ifdef DISABLE_GUI
        if (m_webui->isErrored())
            return 1;
        connect(m_webui, &WebUI::fatalError, this, []() { QCoreApplication::exit(1); });
#endif // DISABLE_GUI
#endif // DISABLE_WEBUI

        new RSS::Session; // create RSS::Session singleton
        new RSS::AutoDownloader; // create RSS::AutoDownloader singleton
    }
    catch (const RuntimeError &err)
    {
#ifdef DISABLE_GUI
        fprintf(stderr, "%s", qPrintable(err.message()));
#else
        QMessageBox msgBox;
        msgBox.setIcon(QMessageBox::Critical);
        msgBox.setText(tr("Application failed to start."));
        msgBox.setInformativeText(err.message());
        msgBox.show(); // Need to be shown or to moveToCenter does not work
        msgBox.move(Utils::Gui::screenCenter(&msgBox));
        msgBox.exec();
#endif
        return 1;
    }

#ifdef DISABLE_GUI
#ifndef DISABLE_WEBUI
    const Preferences *pref = Preferences::instance();

    const auto scheme = QString::fromLatin1(pref->isWebUiHttpsEnabled() ? "https" : "http");
    const auto url = QString::fromLatin1("%1://localhost:%2\n").arg(scheme, QString::number(pref->getWebUiPort()));
    const QString mesg = QString::fromLatin1("\n******** %1 ********\n").arg(tr("Information"))
        + tr("To control qBittorrent, access the WebUI at: %1").arg(url);
    printf("%s\n", qUtf8Printable(mesg));

    if (pref->getWebUIPassword() == "ARQ77eY1NUZaQsuDHbIMCA==:0WMRkYTUWVT9wVvdDtHAjU9b3b7uB8NR1Gur2hmQCvCDpm39Q+PsJRJPaCU51dEiz+dTzh8qbPsL8WkFljQYFQ==")
    {
        const QString warning = tr("The Web UI administrator username is: %1").arg(pref->getWebUiUsername()) + '\n'
            + tr("The Web UI administrator password has not been changed from the default: %1").arg("adminadmin") + '\n'
            + tr("This is a security risk, please change your password in program preferences.") + '\n';
        printf("%s", qUtf8Printable(warning));
    }
#endif // DISABLE_WEBUI
#else
    UIThemeManager::initInstance();
    m_window = new MainWindow;
#endif // DISABLE_GUI

    m_running = true;

    // Now UI is ready to process signals from Session
    BitTorrent::Session::instance()->startUpTorrents();

    m_paramsQueue = params + m_paramsQueue;
    if (!m_paramsQueue.isEmpty())
    {
        processParams(m_paramsQueue);
        m_paramsQueue.clear();
    }
    return BaseApplication::exec();
}

bool Application::isRunning()
{
    return !m_instanceManager->isFirstInstance();
}

#ifndef DISABLE_GUI
#ifdef Q_OS_MACOS
bool Application::event(QEvent *ev)
{
    if (ev->type() == QEvent::FileOpen)
    {
        QString path = static_cast<QFileOpenEvent *>(ev)->file();
        if (path.isEmpty())
            // Get the url instead
            path = static_cast<QFileOpenEvent *>(ev)->url().toString();
        qDebug("Received a mac file open event: %s", qUtf8Printable(path));
        if (m_running)
            processParams(QStringList(path));
        else
            m_paramsQueue.append(path);
        return true;
    }
    else
    {
        return BaseApplication::event(ev);
    }
}
#endif // Q_OS_MACOS
#endif // DISABLE_GUI

void Application::initializeTranslation()
{
    Preferences *const pref = Preferences::instance();
    // Load translation
    const QString localeStr = pref->getLocale();

    if (m_qtTranslator.load(QLatin1String("qtbase_") + localeStr, QLibraryInfo::location(QLibraryInfo::TranslationsPath)) ||
        m_qtTranslator.load(QLatin1String("qt_") + localeStr, QLibraryInfo::location(QLibraryInfo::TranslationsPath)))
            qDebug("Qt %s locale recognized, using translation.", qUtf8Printable(localeStr));
    else
        qDebug("Qt %s locale unrecognized, using default (en).", qUtf8Printable(localeStr));

    installTranslator(&m_qtTranslator);

    if (m_translator.load(QLatin1String(":/lang/qbittorrent_") + localeStr))
        qDebug("%s locale recognized, using translation.", qUtf8Printable(localeStr));
    else
        qDebug("%s locale unrecognized, using default (en).", qUtf8Printable(localeStr));
    installTranslator(&m_translator);

#ifndef DISABLE_GUI
    if (localeStr.startsWith("ar") || localeStr.startsWith("he"))
    {
        qDebug("Right to Left mode");
        setLayoutDirection(Qt::RightToLeft);
    }
    else
    {
        setLayoutDirection(Qt::LeftToRight);
    }
#endif
}

#if (!defined(DISABLE_GUI) && defined(Q_OS_WIN))
void Application::shutdownCleanup(QSessionManager &manager)
{
    Q_UNUSED(manager);

    // This is only needed for a special case on Windows XP.
    // (but is called for every Windows version)
    // If a process takes too much time to exit during OS
    // shutdown, the OS presents a dialog to the user.
    // That dialog tells the user that qbt is blocking the
    // shutdown, it shows a progress bar and it offers
    // a "Terminate Now" button for the user. However,
    // after the progress bar has reached 100% another button
    // is offered to the user reading "Cancel". With this the
    // user can cancel the **OS** shutdown. If we don't do
    // the cleanup by handling the commitDataRequest() signal
    // and the user clicks "Cancel", it will result in qbt being
    // killed and the shutdown proceeding instead. Apparently
    // aboutToQuit() is emitted too late in the shutdown process.
    cleanup();

    // According to the qt docs we shouldn't call quit() inside a slot.
    // aboutToQuit() is never emitted if the user hits "Cancel" in
    // the above dialog.
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
}
#endif

void Application::cleanup()
{
    // cleanup() can be called multiple times during shutdown. We only need it once.
    static QAtomicInt alreadyDone;
    if (!alreadyDone.testAndSetAcquire(0, 1))
        return;

#ifndef DISABLE_GUI
    if (m_window)
    {
        // Hide the window and don't leave it on screen as
        // unresponsive. Also for Windows take the WinId
        // after it's hidden, because hide() may cause a
        // WinId change.
        m_window->hide();

#ifdef Q_OS_WIN
        ::ShutdownBlockReasonCreate(reinterpret_cast<HWND>(m_window->effectiveWinId())
            , tr("Saving torrent progress...").toStdWString().c_str());
#endif // Q_OS_WIN

        // Do manual cleanup in MainWindow to force widgets
        // to save their Preferences, stop all timers and
        // delete as many widgets as possible to leave only
        // a 'shell' MainWindow.
        // We need a valid window handle for Windows Vista+
        // otherwise the system shutdown will continue even
        // though we created a ShutdownBlockReason
        m_window->cleanup();
    }
#endif // DISABLE_GUI

#ifndef DISABLE_WEBUI
    delete m_webui;
#endif

    delete RSS::AutoDownloader::instance();
    delete RSS::Session::instance();

    TorrentFilesWatcher::freeInstance();
    BitTorrent::Session::freeInstance();
    Net::GeoIPManager::freeInstance();
    Net::DownloadManager::freeInstance();
    Net::ProxyConfigurationManager::freeInstance();
    Preferences::freeInstance();
    SettingsStorage::freeInstance();
    delete m_fileLogger;
    Logger::freeInstance();
    IconProvider::freeInstance();
    SearchPluginManager::freeInstance();
    Utils::Fs::removeDirRecursive(Utils::Fs::tempPath());

#ifndef DISABLE_GUI
    if (m_window)
    {
#ifdef Q_OS_WIN
        ::ShutdownBlockReasonDestroy(reinterpret_cast<HWND>(m_window->effectiveWinId()));
#endif // Q_OS_WIN
        delete m_window;
        UIThemeManager::freeInstance();
    }
#endif // DISABLE_GUI

    Profile::freeInstance();

    if (m_shutdownAct != ShutdownDialogAction::Exit)
    {
        qDebug() << "Sending computer shutdown/suspend/hibernate signal...";
        Utils::Misc::shutdownComputer(m_shutdownAct);
    }
}
