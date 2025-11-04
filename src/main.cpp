#include "aboutdialog.h"
#include "headless.h"
#include "mainwindow.h"
#include "searchworkerclient.h"

#include "cubiomes/util.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QStandardPaths>
#include <cstdlib>
#include <cstring>

extern "C"
int getStructureConfig_override(int stype, int mc, StructureConfig *sconf)
{
    if unlikely(mc == INT_MAX) // to check if override is enabled in cubiomes
        mc = 0;
    int ok = getStructureConfig(stype, mc, sconf);
    if (ok && g_extgen.saltOverride)
    {
        uint64_t salt = g_extgen.salts[stype];
        if (salt <= MASK48)
            sconf->salt = salt;
    }
    return ok;
}

int main(int argc, char *argv[])
{
    // FIRST: Check for worker mode BEFORE anything else, using only C functions
    bool workerMode = false;
    quint16 workerPort = 23473;
    int workerThreads = -1;  // -1 means use value from config message
    
    // Quick check for --worker-mode (exact match first)
    if (argc >= 2 && strcmp(argv[1], "--worker-mode") == 0)
    {
        workerMode = true;
        if (argc >= 3)
        {
            unsigned long port = strtoul(argv[2], nullptr, 10);
            if (port > 0 && port <= 65535)
                workerPort = (quint16)port;
        }
    }
    
    // Full argument scan for --worker-mode=port format
    if (!workerMode)
    {
        for (int i = 1; i < argc; i++)
        {
            if (strncmp(argv[i], "--worker-mode=", 14) == 0)
            {
                workerMode = true;
                const char *portStr = argv[i] + 14;
                unsigned long port = strtoul(portStr, nullptr, 10);
                if (port > 0 && port <= 65535)
                {
                    workerPort = (quint16)port;
                }
                break;  // Found it, no need to continue
            }
            else if (strncmp(argv[i], "--worker-mode", 13) == 0 && i+1 < argc)
            {
                workerMode = true;
                const char *portStr = argv[++i];
                unsigned long port = strtoul(portStr, nullptr, 10);
                if (port > 0 && port <= 65535)
                {
                    workerPort = (quint16)port;
                }
                break;  // Found it, no need to continue
            }
        }
    }

    // Set Qt platform to offscreen for headless operation BEFORE any Qt initialization
    // This must be done before creating any Qt application object
    // CRITICAL: Do this immediately after detecting worker mode, before ANY Qt code
    if (workerMode)
    {
        // Method 1: Unset DISPLAY to prevent Qt from trying to connect to X server
        unsetenv("DISPLAY");
        
        // Method 2: Set Qt platform environment variable using putenv with static string
        // Using static string to ensure it persists (putenv doesn't copy)
        static char qpa_platform_env[] = "QT_QPA_PLATFORM=offscreen";
        putenv(qpa_platform_env);
        
        // Also use setenv as backup (more portable, makes a copy)
        setenv("QT_QPA_PLATFORM", "offscreen", 1);
        
        // Method 3: Inject -platform offscreen into argv BEFORE Qt sees it
        // Qt processes -platform argument before environment variable in some versions
        // We need to check if -platform is already specified
        bool has_platform_arg = false;
        for (int i = 1; i < argc; i++)
        {
            if (strcmp(argv[i], "-platform") == 0)
            {
                has_platform_arg = true;
                break;
            }
        }
        
        if (!has_platform_arg)
        {
            // Allocate new argv array with space for two additional arguments
            char **new_argv = new char*[argc + 3];
            new_argv[0] = argv[0];
            // Insert -platform offscreen right after program name (Qt processes in order)
            static char platform_arg[] = "-platform";
            static char offscreen_arg[] = "offscreen";
            new_argv[1] = platform_arg;
            new_argv[2] = offscreen_arg;
            // Copy remaining arguments
            for (int i = 1; i < argc; i++)
            {
                new_argv[i + 2] = argv[i];
            }
            new_argv[argc + 2] = nullptr;  // Null-terminate the array
            argc += 2;
            argv = new_argv;
        }
    }

    initBiomeColors(g_biomeColors);
    initBiomeTypeColors(g_tempsColors);

    // Delay setApplicationName for worker mode to avoid any potential Qt initialization
    // before the platform is properly set
    if (!workerMode)
    {
        QCoreApplication::setApplicationName(APP_STRING);
    }

    bool version = false;
    bool nogui = false;
    bool clear = false;
    bool reset = false;
    bool usage = false;
    QString sessionpath;
    QString resultspath;
    QString workerHost;

    for (int i = 1; i < argc; i++)
    {
        // Skip Qt platform arguments that we may have injected
        if (strcmp(argv[i], "-platform") == 0 || strcmp(argv[i], "offscreen") == 0)
            continue;
            
        if (strcmp(argv[i], "--version") == 0)
            version = true;
        else if (strcmp(argv[i], "--nogui") == 0)
            nogui = true;
        else if (strcmp(argv[i], "--reset") == 0)
            clear = true;
        else if (strcmp(argv[i], "--reset-all") == 0)
            reset = true;
        else if (strncmp(argv[i], "--session=", 10) == 0)
            sessionpath = argv[i] + 10;
        else if (strncmp(argv[i], "--session", 9) == 0 && i+1 < argc)
            sessionpath = argv[++i];
        else if (strncmp(argv[i], "--out=", 6) == 0)
            resultspath = argv[i] + 6;
        else if (strncmp(argv[i], "--out", 5) == 0 && i+1 < argc)
            resultspath = argv[++i];
        else if (strncmp(argv[i], "--worker-mode=", 14) == 0)
        {
            workerMode = true;
            QString portStr = argv[i] + 14;
            bool ok;
            quint16 port = portStr.toUShort(&ok);
            if (ok && port > 0)
            {
                workerPort = port;
            }
        }
        else if (strncmp(argv[i], "--worker-mode", 13) == 0 && i+1 < argc)
        {
            workerMode = true;
            // Only consume next arg as port if it's numeric (not another option)
            const char *next = argv[i+1];
            bool numeric = (next && next[0] && next[0] != '-');
            if (numeric)
            {
                // Validate all chars are digits
                for (const char *p = next; *p; ++p)
                {
                    if (*p < '0' || *p > '9') { numeric = false; break; }
                }
            }
            if (numeric)
            {
                QString portStr = argv[++i];
                bool ok;
                quint16 port = portStr.toUShort(&ok);
                if (ok && port > 0)
                {
                    workerPort = port;
                }
            }
            // else: leave i unchanged so other options (e.g., --worker-threads) are parsed
        }
        else if (strncmp(argv[i], "--worker-threads=", 17) == 0)
        {
            QString threadsStr = argv[i] + 17;
            bool ok;
            int threads = threadsStr.toInt(&ok);
            if (ok && threads > 0)
            {
                workerThreads = threads;
            }
        }
        else if (strncmp(argv[i], "--worker-threads", 16) == 0 && i+1 < argc)
        {
            QString threadsStr = argv[++i];
            bool ok;
            int threads = threadsStr.toInt(&ok);
            if (ok && threads > 0)
            {
                workerThreads = threads;
            }
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            usage = true;
    }

    if (usage)
    {
        const char *msg =
                "Usage: cubiomes-viewer [options]\n"
                "Options:\n"
                "      --help                 Display this help and exit.\n"
                "      --version              Output version information and exit.\n"
                "      --nogui                Run in headless search mode.\n"
                "      --reset                Discard results and reset starting seed.\n"
                "      --reset-all            Clear settings and remove all session data.\n"
                "      --session=file         Open this session file.\n"
                "      --out=file             Write matching seeds to this file while searching.\n"
                "      --worker-mode=port     Run as worker server, listening for coordinator connections.\n"
                "                             Default port is 23473.\n"
                "      --worker-threads=N      Set number of worker threads (overrides GUI setting).\n"
                "                             If not specified, uses value from coordinator or CPU count.\n"
                "\n";
        printf("%s", msg);
        exit(0);
    }
    if (version)
    {
        printf("%s %s\n", APP_STRING, getVersStr().toLocal8Bit().data());
        exit(0);
    }

    if (reset)
    {
        QSettings settings(APP_STRING, APP_STRING);
        settings.clear();

        QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        QDir dir(path);
        if (dir.exists() && path.contains(APP_STRING))
        {
            dir.removeRecursively();
        }
    }

    if (sessionpath.isEmpty())
    {
        QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        QDir dir(path);
        if (!dir.exists())
            dir.mkpath(".");
        sessionpath = path + "/session.save";
    }

    if (workerMode)
    {
        // Force platform one more time right before creating QCoreApplication
        // Sometimes Qt reads environment variables at construction time
        // Use both putenv and setenv to ensure it's set
        static char qpa_platform_env[] = "QT_QPA_PLATFORM=offscreen";
        putenv(qpa_platform_env);
        setenv("QT_QPA_PLATFORM", "offscreen", 1);
        unsetenv("DISPLAY");
        
        // Set application name after platform is configured
        QCoreApplication::setApplicationName(APP_STRING);
        
        // Create QCoreApplication - platform should already be set via environment/argv
        // The -platform argument in argv should take precedence
        QCoreApplication app(argc, argv);
        
        SearchWorkerClient worker(workerPort, workerThreads);
        
        if (!worker.start())
        {
            fprintf(stderr, "Error: Failed to start worker server on port %d\n", workerPort);
            return 1;
        }
        
        fprintf(stdout, "Worker server listening on port %d", workerPort);
        if (workerThreads > 0)
        {
            fprintf(stdout, ", using %d threads", workerThreads);
        }
        fprintf(stdout, ", waiting for coordinator connections...\n");
        
        // Don't quit on disconnect - keep listening for reconnections
        QObject::connect(&worker, &SearchWorkerClient::disconnected, []() {
            fprintf(stdout, "Coordinator disconnected, waiting for reconnection...\n");
        });
        QObject::connect(&worker, &SearchWorkerClient::clientError, [&app](const QString& msg) {
            fprintf(stderr, "Worker error: %s\n", msg.toLocal8Bit().data());
        });
        
        return app.exec();
    }
    
    if (nogui)
    {
        QCoreApplication app(argc, argv);
        Headless headless(sessionpath, resultspath, clear, &app);

        QObject::connect(&headless, SIGNAL(finished()), &app, SLOT(quit()));
        QTimer::singleShot(0, &headless, SLOT(run()));

        return app.exec();
    }
    else
    {
        QGuiApplication::setDesktopFileName("com.github.cubitect.cubiomes-viewer");
        QApplication::setAttribute(Qt::AA_UseStyleSheetPropagationInWidgetStyles, false);

        QApplication app(argc, argv);

        MainWindow mw(sessionpath, resultspath);
        mw.show();
        return app.exec();
    }
}
