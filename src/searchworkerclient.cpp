#include "searchworkerclient.h"

#include "cubiomes/finders.h"
#include "cubiomes/util.h"
#include "config.h"
#include "seedtables.h"
#include "cubiomes/quadbase.h"
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <cstring>
#include <cstdint>

// Local 48-bit generator cursor implementation (must be defined before use)
struct LocalGen48Cursor
{
    int mode;
    Gen48Config gen48;
    int mc;
    uint64_t salt;
    int cst_type;
    // QH state
    QList<QFileInfo> files; // sorted by name
    int fileIdx = -1;
    QFile file;
    QTextStream stream;
    uint64_t curLow = 0;
    uint64_t curMid = 0;
    bool fileOpen = false;
    // QM state
    const uint64_t *qmPtr = nullptr;
    // Grid expansion
    int gx = 0, gz = 0;
    int gw = 1, gh = 1;
    // Global index
    uint64_t curIdx = 0;
    // Current base seed state
    uint64_t currentBase = 0;
    bool haveBase = false;
    // Debug: call counter for logging throttling
    uint64_t callCount = 0;

    void init()
    {
        gw = gen48.x2 - gen48.x1 + 1;
        gh = gen48.z2 - gen48.z1 + 1;
        if (gw < 1) gw = 1;
        if (gh < 1) gh = 1;
        gx = 0; gz = 0;
        curIdx = 0;
        haveBase = false;
        if (mode == GEN48_QH)
        {
            QDir dir(":/qh");
            files = dir.entryInfoList(QDir::Files, QDir::Name);
            fileIdx = -1;
            fileOpen = false;
        }
        else if (mode == GEN48_QM)
        {
            qmPtr = g_qm_90;
        }
    }

    bool openNextQHFile()
    {
        static int openCount = 0;
        openCount++;
        // Only log first 2 calls for initialization verification
        bool shouldLog = (openCount <= 2);
        
        while (true)
        {
            fileIdx++;
            if (fileIdx >= files.size())
            {
                // Only log when no more files (end state)
                if (shouldLog)
                {
                    qDebug() << "LocalGen48Cursor::openNextQHFile() - no more files available";
                }
                return false;
            }
            curLow = files[fileIdx].baseName().toULongLong(nullptr, 16);
            if (getQuadHutCst(curLow) > cst_type) // skip invalid
            {
                continue;
            }
            QString filePath = files[fileIdx].absoluteFilePath();
            if (file.isOpen())
                file.close();
            file.setFileName(filePath);
            if (!file.open(QIODevice::ReadOnly))
            {
                // Always log errors
                qWarning() << "LocalGen48Cursor::openNextQHFile() - failed to open file:" << filePath;
                continue;
            }
            stream.setDevice(&file);
            curMid = 0;
            fileOpen = true;
            if (shouldLog)
            {
                qDebug() << "LocalGen48Cursor::openNextQHFile() - opened file #" << openCount 
                         << "fileIdx:" << fileIdx << "curLow:" << curLow;
            }
            return true;
        }
    }

    bool nextBaseQH(uint64_t &base)
    {
        static int callCount = 0;
        callCount++;
        // Only log every 1 million calls, or first 2 calls for initialization verification
        if (callCount % 1000000 == 0 || callCount <= 2)
        {
            qDebug() << "LocalGen48Cursor::nextBaseQH() - call #" << callCount 
                     << "fileOpen:" << fileOpen << "fileIdx:" << fileIdx;
        }
        while (true)
        {
            if (!fileOpen)
            {
                if (!openNextQHFile())
                {
                    // Only log errors
                    qDebug() << "LocalGen48Cursor::nextBaseQH() - openNextQHFile() returned false";
                    return false;
                }
            }
            QString line;
            if (stream.atEnd())
            {
                file.close();
                fileOpen = false;
                continue;
            }
            line = stream.readLine();
            uint64_t diff = line.toULongLong(nullptr, 16);
            if (diff == 0)
            {
                file.close();
                fileOpen = false;
                continue;
            }
            curMid += diff;
            uint64_t s48 = (curMid << 20) + curLow;
            base = (s48 - salt) & MASK48;
            return true;
        }
    }

    bool nextBaseQM(uint64_t &base)
    {
        static int callCount = 0;
        callCount++;
        
        if (!qmPtr)
        {
            qWarning() << "LocalGen48Cursor::nextBaseQM() - qmPtr is null!";
            return false;
        }
        int count = 0;
        // Safety: check that we don't dereference invalid memory
        // In practice, g_qm_90 should be null-terminated, but be defensive
        int maxIterations = 1000000; // Safety limit
        while (count < maxIterations)
        {
            // Read the value before incrementing pointer to be safe
            uint64_t s = *qmPtr;
            
            if (s == 0) // Hit sentinel value
            {
                // Only log first time
                if (callCount <= 1)
                {
                    qDebug() << "LocalGen48Cursor::nextBaseQM() - reached sentinel at count:" << count;
                }
                break;
            }
            
            // Now safe to increment pointer
            qmPtr++;
            count++;
            
            int qual = qmonumentQual(s);
            
            if (qual >= gen48.qmarea)
            {
                base = (s - salt) & MASK48;
                return true;
            }
        }
        if (count >= maxIterations)
        {
            qWarning() << "LocalGen48Cursor::nextBaseQM() - hit safety limit, possible infinite loop or corrupted array";
            return false;
        }
        return false;
    }

    bool nextExpanded(uint64_t &seed)
    {
        // Critical validation - check if 'this' pointer is valid by accessing a simple member first
        while (true)
        {
            if (!haveBase)
            {
                bool ok;
                if (mode == GEN48_QH)
                    ok = nextBaseQH(currentBase);
                else
                    ok = nextBaseQM(currentBase);
                if (!ok)
                    return false;
                gx = 0; gz = 0;
                haveBase = true;
            }
            int dx = gen48.x1 + gx;
            int dz = gen48.z1 + gz;
            callCount++;
            
            // Validate grid coordinates
            if (gx < 0 || gx >= gw || gz < 0 || gz >= gh)
            {
                qWarning() << "LocalGen48Cursor::nextExpanded() - INVALID GRID COORDS! gx:" << gx 
                           << "gw:" << gw << "gz:" << gz << "gh:" << gh;
                return false;
            }
            
            seed = moveStructure(currentBase, dx, dz);
            
            // advance grid
            gx++;
            if (gx >= gw)
            {
                gx = 0;
                gz++;
                if (gz >= gh)
                {
                    gz = 0;
                    haveBase = false;
                }
            }
            return true;
        }
    }
};

#include <QDebug>
#include <QHostAddress>
#include <QDateTime>
#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>

SearchWorkerClient::SearchWorkerClient(quint16 port, int threadCount, QObject *parent)
    : QObject(parent)
    , m_port(port)
    , m_server(new QTcpServer(this))
    , m_socket(nullptr)
    , m_configReceived(false)
    , m_searchtype(0)
    , m_mc(0)
    , m_large(false)
    , m_localGen48(false)
    , m_localGenSlicing(false)
    , m_stop(false)
    , m_threadCount(threadCount)  // -1 means use value from config message
    , m_threadCountSet(threadCount > 0)  // True if explicitly set from command line
    , m_taskTimer(new QTimer(this))
    , m_heartbeatTimer(new QTimer(this))
    , m_rateLogTimer(new QTimer(this))
    , m_resultsFlushTimer(new QTimer(this))
    , m_logTotalSeeds(0)
    , m_logLastSeeds(0)
    , m_logFirstInterval(true)
    , m_progressThrottleAccumulator(0)
    , m_taskCounter(0)
    , m_taskRequestPending(false)
{
    m_resultsBatch.reserve(RESULTS_BATCH_SIZE);
    connect(m_server, &QTcpServer::newConnection, this, &SearchWorkerClient::onNewConnection);
    
    m_taskTimer->setSingleShot(true);
    connect(m_taskTimer, &QTimer::timeout, this, &SearchWorkerClient::requestTask);
    
    m_heartbeatTimer->setInterval(30000); // 30 seconds
    connect(m_heartbeatTimer, &QTimer::timeout, this, &SearchWorkerClient::sendHeartbeat);
    
    m_rateLogTimer->setInterval(10000); // 10 seconds
    connect(m_rateLogTimer, &QTimer::timeout, this, &SearchWorkerClient::logSearchRate);
    
    // Periodic flush of results batch
    m_resultsFlushTimer->setInterval(RESULTS_BATCH_MS);
    connect(m_resultsFlushTimer, &QTimer::timeout, this, [this]() {
        QMutexLocker locker(&m_resultsBatchMutex);
        if (!m_resultsBatch.isEmpty())
        {
            QVector<uint64_t> toSend = m_resultsBatch;
            m_resultsBatch.clear();
            locker.unlock(); // Unlock before sending
            
            if (!toSend.isEmpty())
            {
                sendResults(toSend);
            }
        }
    });
}

SearchWorkerClient::~SearchWorkerClient()
{
    stop();
    stopWorkerThreads();
}

bool SearchWorkerClient::start()
{
    if (m_server->isListening())
        return true;
    
    if (!m_server->listen(QHostAddress::Any, m_port))
    {
        qWarning() << "Failed to start worker server on port" << m_port << ":" << m_server->errorString();
        return false;
    }
    
    qDebug() << "Worker server listening on port" << m_port;
    return true;
}

void SearchWorkerClient::stop()
{
    m_stop = true;
    
    if (m_server->isListening())
    {
        m_server->close();
    }
    
    // Flush any remaining progress before stopping
    {
        QMutexLocker locker(&m_progressThrottleMutex);
        if (m_progressThrottleAccumulator > 0 && m_socket && 
            m_socket->state() == QAbstractSocket::ConnectedState)
        {
            QByteArray progress;
            writeUint32(progress, MSG_PROGRESS);
            writeUint64(progress, m_progressThrottleAccumulator);
            writeUint64(progress, 0); // seed unknown
            sendMessage(progress);
            m_progressThrottleAccumulator = 0;
        }
    }
    
    // Flush any remaining results before stopping
    {
        QMutexLocker locker(&m_resultsBatchMutex);
        if (!m_resultsBatch.isEmpty() && m_socket && 
            m_socket->state() == QAbstractSocket::ConnectedState)
        {
            QVector<uint64_t> toSend = m_resultsBatch;
            m_resultsBatch.clear();
            locker.unlock(); // Unlock before sending
            
            sendResults(toSend);
        }
    }
    
    if (m_socket)
    {
        m_socket->disconnectFromHost();
        if (m_socket->state() != QAbstractSocket::UnconnectedState)
            m_socket->waitForDisconnected(1000);
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_heartbeatTimer->stop();
    m_taskTimer->stop();
    m_rateLogTimer->stop();
    m_resultsFlushTimer->stop();
    
    stopWorkerThreads();
}

void SearchWorkerClient::onNewConnection()
{
    if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState)
    {
        // Already have a connection, reject new one
        QTcpSocket* newSocket = m_server->nextPendingConnection();
        if (newSocket)
        {
            qWarning() << "Rejecting new connection - already connected";
            newSocket->deleteLater();
        }
        return;
    }
    
    QTcpSocket* socket = m_server->nextPendingConnection();
    if (!socket)
        return;
    
    if (m_socket)
        m_socket->deleteLater();
    
    m_socket = socket;
    connect(m_socket, &QTcpSocket::readyRead, this, &SearchWorkerClient::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &SearchWorkerClient::onDisconnected);
    connect(m_socket, &QAbstractSocket::errorOccurred,
            this, &SearchWorkerClient::onError);
    
    m_readBuffer.clear();
    m_configReceived = false;
    
    qDebug() << "Coordinator connected from" << socket->peerAddress().toString();
    
    emit connected();
}

void SearchWorkerClient::onReadyRead()
{
    QByteArray msg;
    while (readMessage(msg))
    {
        processMessage(msg);
    }
}

bool SearchWorkerClient::readMessage(QByteArray& msg)
{
    // Read message length (4 bytes)
    while (m_readBuffer.size() < 4)
    {
        QByteArray data = m_socket->read(4096);
        if (data.isEmpty())
            return false;
        m_readBuffer.append(data);
    }
    
    uint32_t msgLen;
    memcpy(&msgLen, m_readBuffer.constData(), 4);
    msgLen = qFromBigEndian(msgLen);
    
    // DEBUG: Warn about large messages
    if (msgLen > 10 * 1024 * 1024) {  // > 10MB
        qWarning() << "WARNING: Receiving very large message:" << (msgLen / 1024 / 1024) << "MB";
    }
    
    // Read complete message
    while (m_readBuffer.size() < (int)(4 + msgLen))
    {
        QByteArray data = m_socket->read(4096);
        if (data.isEmpty())
            return false;
        m_readBuffer.append(data);
        
        // DEBUG: Track read buffer growth
        if (m_readBuffer.size() > 100 * 1024 * 1024) {  // > 100MB
            static qint64 lastWarning = 0;
            qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - lastWarning > 5000) {  // Warn every 5 seconds max
                qWarning() << "WARNING: Read buffer very large:" << (m_readBuffer.size() / 1024 / 1024) << "MB";
                lastWarning = now;
            }
        }
    }
    
    msg = m_readBuffer.mid(4, msgLen);
    m_readBuffer.remove(0, 4 + msgLen);
    
    return true;
}

void SearchWorkerClient::sendMessage(const QByteArray& msg)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return;
    
    // Backpressure: Avoid unbounded growth of Qt socket write buffer
    const qint64 WRITE_BUFFER_LIMIT = 32LL * 1024 * 1024; // 32 MB
    while (m_socket->bytesToWrite() > WRITE_BUFFER_LIMIT)
    {
        // Log occasionally
        static qint64 lastWarn = 0;
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastWarn > 2000)
        {
            qWarning() << "Backpressure: bytesToWrite=" << (m_socket->bytesToWrite() / 1024) << "KB, delaying send";
            lastWarn = now;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
        if (m_stop) break;
        if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
            return;
    }
    
    uint32_t len = qToBigEndian((uint32_t)msg.size());
    m_socket->write(reinterpret_cast<const char*>(&len), 4);
    m_socket->write(msg);
    m_socket->flush();
}

void SearchWorkerClient::processMessage(const QByteArray& msg)
{
    if (msg.size() < 4)
        return;
    
    int offset = 0;
    uint32_t type = readUint32(msg, offset);
    
    switch (type)
    {
    case MSG_HELLO:
    {
        uint32_t version = readUint32(msg, offset);
        if (version != SEARCH_PROTOCOL_VERSION)
        {
            emit clientError(QString("Protocol version mismatch: expected %1, got %2")
                      .arg(SEARCH_PROTOCOL_VERSION).arg(version));
            return;
        }
        
        // Send hello response
        QByteArray hello;
        writeUint32(hello, MSG_HELLO);
        writeUint32(hello, SEARCH_PROTOCOL_VERSION);
        sendMessage(hello);
        
        // Start heartbeat
        m_heartbeatTimer->start();
        break;
    }
    
    case MSG_CONFIG:
    {
        uint32_t version = readUint32(msg, offset);
        if (version != SEARCH_PROTOCOL_VERSION)
        {
            emit clientError(QString("Protocol version mismatch"));
            return;
        }
        
        m_mc = readInt32(msg, offset);
        m_large = readUint32(msg, offset) != 0;
        m_searchtype = readInt32(msg, offset);
        m_session.sc.searchtype = m_searchtype;
        m_session.wi.mc = m_mc;
        m_session.wi.large = m_large;
        
        m_session.sc.startseed = readUint64(msg, offset);
        m_session.sc.smin = readUint64(msg, offset);
        m_session.sc.smax = readUint64(msg, offset);
        
        m_session.gen48.mode = readUint32(msg, offset);
        m_session.gen48.salt = readUint64(msg, offset);
        m_session.gen48.listsalt = readUint64(msg, offset);
        m_session.gen48.qual = readInt32(msg, offset);
        m_session.gen48.qmarea = readInt32(msg, offset);
        m_session.gen48.x1 = readInt32(msg, offset);
        m_session.gen48.z1 = readInt32(msg, offset);
        m_session.gen48.x2 = readInt32(msg, offset);
        m_session.gen48.z2 = readInt32(msg, offset);
        
        // Conditions
        uint32_t condCount = readUint32(msg, offset);
        m_session.cv.clear();
        m_session.cv.reserve(condCount);
        uint32_t receivedCount = 0;
        for (uint32_t i = 0; i < condCount; i++)
        {
            if (offset + (int)sizeof(Condition) > msg.size())
            {
                qWarning() << "Message truncated: expected" << condCount << "conditions, received" << receivedCount;
                break;
            }
            Condition c;
            memcpy(&c, msg.constData() + offset, sizeof(Condition));
            // Zero out generated members to ensure clean state for apply()
            memset(c.generated_start, 0, sizeof(Condition) - offsetof(Condition, generated_start));
            // Ensure condition is upgraded to current version
            if (!c.versionUpgrade())
            {
                qWarning() << "Failed to upgrade condition to current version";
                continue;
            }
            m_session.cv.push_back(c);
            offset += sizeof(Condition);
            receivedCount++;
        }
        if (receivedCount != condCount)
        {
            qWarning() << "Received" << receivedCount << "conditions out of" << condCount << "expected";
        }
        
        // Seed list
        uint32_t listCount = readUint32(msg, offset);
        m_slist.clear();
        m_slist.reserve(listCount);
        for (uint32_t i = 0; i < listCount; i++)
        {
            m_slist.push_back(readUint64(msg, offset));
        }
        m_session.slist = m_slist;

        // If a seed list is provided, prefer it over local generation to ensure identical ordering
        if (listCount > 0)
        {
            m_localGen48 = false;
        }

        // Thread count from coordinator
        int coordThreadCount = -1;
        if (offset + 4 <= msg.size())
        {
            coordThreadCount = readInt32(msg, offset);
        }

        // Local generator flag (instruct worker to generate 48-bit candidate list locally)
        if (offset + 4 <= msg.size())
        {
            m_localGen48 = (readUint32(msg, offset) != 0);
        }
        // Special slicing semantics flag for local 48-bit generation
        if (offset + 4 <= msg.size())
        {
            m_localGenSlicing = (readUint32(msg, offset) != 0);
        }
        // Low-candidate count (provided by coordinator) to avoid per-thread pre-counting
        if (offset + 8 <= msg.size())
        {
            m_localGenCount = readUint64(msg, offset);
        }

        if (m_localGen48)
        {
            m_localGenCache = std::make_shared<QVector<uint64_t>>();
            if (m_session.gen48.mode == GEN48_QH)
            {
                // Pre-compute and cache the full list of QH candidates
                LocalGen48Cursor cursor;
                cursor.mode = m_session.gen48.mode;
                cursor.gen48 = m_session.gen48;
                cursor.mc = m_mc;
                switch (m_session.gen48.qual)
                {
                case IDEAL_SALTED:
                case IDEAL:   cursor.cst_type = CST_IDEAL; break;
                case CLASSIC: cursor.cst_type = CST_CLASSIC; break;
                case NORMAL:  cursor.cst_type = CST_NORMAL; break;
                case BARELY:  cursor.cst_type = CST_BARELY; break;
                default:      cursor.cst_type = CST_BARELY; break;
                }
                if (m_session.gen48.qual == IDEAL_SALTED)
                    cursor.salt = m_session.gen48.salt;
                else
                {
                    StructureConfig sconf;
                    getStructureConfig_override(Swamp_Hut, m_mc, &sconf);
                    cursor.salt = sconf.salt;
                }
                cursor.init();
                uint64_t seed;
                while(cursor.nextExpanded(seed))
                {
                    m_localGenCache->append(seed);
                }
                m_localGenCount = m_localGenCache->size();
                qDebug() << "Worker: Pre-computed" << m_localGenCount << "local QH candidates";
            }
        }
        
        // Honor command-line thread count if provided (takes precedence over coordinator)
        if (m_threadCount > 0)
        {
            qDebug() << "Using command-line thread count:" << m_threadCount << "(ignoring coordinator value:" << coordThreadCount << ")";
        }
        else if (coordThreadCount > 0)
        {
            m_threadCount = coordThreadCount;
            qDebug() << "Using thread count from coordinator:" << m_threadCount;
        }
        else
        {
            // Fallback to ideal thread count
            m_threadCount = QThread::idealThreadCount();
            if (m_threadCount <= 0)
                m_threadCount = 1;
            qDebug() << "Using ideal thread count:" << m_threadCount;
        }

        // Initialize range-based local generator cursor (avoid building full list)
        if (m_localGen48)
        {
            // This is now handled per-thread
        }
        
        // Initialize search environment
        QString err = m_condtree.set(m_session.cv, m_mc);
        if (!err.isEmpty())
        {
            emit clientError(QString("Failed to initialize search environment: %1").arg(err));
            return;
        }
        
        m_configReceived = true;
        m_stop = false;
        qDebug() << "Received search configuration, starting" << m_threadCount << "worker threads";
        
        // Reset logging state
        m_logTotalSeeds = 0;
        m_logLastSeeds = 0;
        m_logFirstInterval = true;
        m_logTimer.restart();
        
        // Clear and start results batch timer
        {
            QMutexLocker locker(&m_resultsBatchMutex);
            m_resultsBatch.clear();
        }
        m_resultsFlushTimer->start();
        
        // Start worker threads
        startWorkerThreads();
        
        // Start rate logging timer
        m_rateLogTimer->start();
        
        // Request initial tasks to fill the queue to keep all threads busy
        // Additional tasks will be requested automatically as queue gets low
        int initialTasks = qMin(m_threadCount * 2, 64); // Warm start: 2x threads, capped at 64
        for (int i = 0; i < initialTasks; i++)
        {
            QByteArray msg;
            writeUint32(msg, MSG_TASK_REQUEST);
            sendMessage(msg);
            {
                QMutexLocker locker(&m_taskRequestMutex);
                m_taskRequestPending = true;
                m_inflightRequests++;
            }
        }
        break;
    }
    
    case MSG_TASK_ASSIGN:
    {
        uint64_t sstart = readUint64(msg, offset);
        uint64_t scnt = readUint64(msg, offset);
        uint64_t idx = readUint64(msg, offset);
        
        // Mark that we've received a task, so we can request more if needed
        {
            QMutexLocker locker(&m_taskRequestMutex);
            m_taskRequestPending = false;
            if (m_inflightRequests > 0)
                m_inflightRequests--;
        }
        
        // Log only every Nth task
        uint64_t taskNum = m_taskCounter.fetch_add(1) + 1;
        if (taskNum % TASK_LOG_INTERVAL == 0)
        {
            qDebug() << "Worker: Task #" << taskNum << "- sstart:" << sstart << "scnt:" << scnt << "idx:" << idx;
        }
        
        if (m_configReceived)
        {
            queueTask(sstart, scnt, idx);
        }
        else
        {
            qWarning() << "Received task before configuration";
        }
        break;
    }
    
    case MSG_STOP:
        qDebug() << "Received stop command";
        m_stop = true;
        m_taskTimer->stop();
        m_rateLogTimer->stop();
        // Wake up all waiting threads
        {
            QMutexLocker locker(&m_taskMutex);
            m_taskCondition.wakeAll();
        }
        break;
    
    case MSG_PING:
    {
        QByteArray pong;
        writeUint32(pong, MSG_PONG);
        sendMessage(pong);
        break;
    }
    
    case MSG_PONG:
        // Heartbeat response from coordinator - no action needed
        break;
    
    default:
        qWarning() << "Unknown message type:" << type;
        break;
    }
}

void SearchWorkerClient::queueTask(uint64_t sstart, uint64_t scnt, uint64_t idx)
{
    TaskItem task;
    task.sstart = sstart;
    task.scnt = scnt;
    task.idx = idx;
    
    QMutexLocker locker(&m_taskMutex);
    m_taskQueue.push(task);
    // Logging is done in MSG_TASK_ASSIGN handler, not here
    m_taskCondition.wakeOne(); // Wake one waiting thread
}

bool SearchWorkerClient::getNextTask(TaskItem& task)
{
    QMutexLocker locker(&m_taskMutex);
    
    while (m_taskQueue.empty() && !m_stop)
    {
        // Don't log waiting - too verbose
        m_taskCondition.wait(&m_taskMutex);
    }
    
    if (m_stop && m_taskQueue.empty())
    {
        // Don't log stop - happens for every thread
        return false;
    }
    
    if (!m_taskQueue.empty())
    {
        task = m_taskQueue.front();
        m_taskQueue.pop();
        return true;
    }
    
    return false;
}

void SearchWorkerClient::startWorkerThreads()
{
    stopWorkerThreads();
    
    // Only set default thread count if it wasn't explicitly set from command line
    // Command-line settings take precedence over coordinator settings
    if (!m_threadCountSet && m_threadCount <= 0)
    {
        m_threadCount = QThread::idealThreadCount();
        if (m_threadCount <= 0)
            m_threadCount = 1;
    }
    // If m_threadCountSet is true, m_threadCount should already be set correctly
    // If m_threadCountSet is false but m_threadCount > 0, it was set from coordinator config
    
    for (int i = 0; i < m_threadCount; i++)
    {
        // Pass thread-local copies of configuration data to avoid accessing shared client data
        WorkerThread *worker = new WorkerThread(this, m_session, m_mc, m_large, 
                                                 m_searchtype, m_slist, &m_stop, this,
                                                 m_localGenCache);
        // Batch results to prevent Qt signal queue buildup
        // Use QueuedConnection to avoid blocking worker threads - batching prevents queue growth
        connect(worker, &WorkerThread::taskResult, this, [this](uint64_t seed) {
            QMutexLocker locker(&m_resultsBatchMutex);
            m_resultsBatch.append(seed);
            
            // Send if batch is full or timer expired
            bool shouldSend = false;
            if (m_resultsBatch.size() >= RESULTS_BATCH_SIZE)
            {
                shouldSend = true;
            }
            else if (!m_resultsBatchTimer.isValid())
            {
                m_resultsBatchTimer.start();
                shouldSend = false; // Don't send immediately on first item
            }
            else if (m_resultsBatchTimer.elapsed() >= RESULTS_BATCH_MS)
            {
                shouldSend = true;
            }
            
            if (shouldSend && !m_resultsBatch.isEmpty())
            {
                QVector<uint64_t> toSend = m_resultsBatch;
                m_resultsBatch.clear();
                m_resultsBatchTimer.restart();
                locker.unlock(); // Unlock before sending
                
                sendResults(toSend);
            }
        }, Qt::QueuedConnection);  // Non-blocking - batching prevents queue growth
        
        connect(worker, &WorkerThread::taskResults, this, [this](const QVector<uint64_t>& seeds) {
            // For batched results from worker, add to our batch buffer
            QMutexLocker locker(&m_resultsBatchMutex);
            m_resultsBatch.append(seeds);
            
            // Send if batch is getting large
            if (m_resultsBatch.size() >= RESULTS_BATCH_SIZE)
            {
                QVector<uint64_t> toSend = m_resultsBatch;
                m_resultsBatch.clear();
                m_resultsBatchTimer.restart();
                locker.unlock(); // Unlock before sending
                
                sendResults(toSend);
            }
        }, Qt::QueuedConnection);  // Non-blocking - batching prevents queue growth
        
        connect(worker, &WorkerThread::taskProgress, this, [this](uint64_t prog, uint64_t seed) {
            // Track total seeds for logging (atomic increment to avoid race conditions)
            m_logTotalSeeds.fetch_add(prog, std::memory_order_relaxed);
            
            // Throttle progress updates to prevent signal queue buildup
            // Use atomic operations to reduce mutex contention
            uint64_t accumulated;
            bool shouldSend = false;
            
            {
                QMutexLocker locker(&m_progressThrottleMutex);
                m_progressThrottleAccumulator += prog;
                
                qint64 elapsed = m_progressThrottleTimer.elapsed();
                if (elapsed >= PROGRESS_THROTTLE_MS || !m_progressThrottleTimer.isValid())
                {
                    // Send accumulated progress
                    if (!m_progressThrottleTimer.isValid())
                    {
                        m_progressThrottleTimer.start();
                    }
                    
                    accumulated = m_progressThrottleAccumulator;
                    m_progressThrottleAccumulator = 0;
                    m_progressThrottleTimer.restart();
                    shouldSend = true;
                }
            }
            
            // Send outside mutex to avoid blocking other threads
            if (shouldSend)
            {
                QByteArray progress;
                writeUint32(progress, MSG_PROGRESS);
                writeUint64(progress, accumulated);
                writeUint64(progress, seed);
                sendMessage(progress);
            }
        }, Qt::QueuedConnection);  // Non-blocking - throttling prevents queue growth
        
        connect(worker, &WorkerThread::taskFinished, this, [this]() {
            // Task completed, send done message
            // Note: Multiple threads may emit this, but that's okay - each completed task needs a done message
            // Logging is done in MSG_TASK_ASSIGN handler (every Nth task)
            QByteArray done;
            writeUint32(done, MSG_DONE);
            sendMessage(done);
            // Check if we need more tasks (batched to reduce network overhead)
            QMetaObject::invokeMethod(this, "checkTaskQueue", Qt::QueuedConnection);
        }, Qt::QueuedConnection);  // Non-blocking - task requests are queued
        
        m_workers.push_back(worker);
        worker->start();
    }
    
    qDebug() << "Started" << m_threadCount << "worker threads";
}

void SearchWorkerClient::stopWorkerThreads()
{
    // Stop all workers
    for (WorkerThread *worker : m_workers)
    {
        worker->stop();
        worker->wait(); // Wait indefinitely for thread to finish
        worker->deleteLater();
    }
    m_workers.clear();
    
    // Clear task queue
    QMutexLocker locker(&m_taskMutex);
    while (!m_taskQueue.empty())
        m_taskQueue.pop();
}

// WorkerThread implementation
WorkerThread::WorkerThread(SearchWorkerClient *client, const Session& session, int mc, bool large,
                           int searchtype, const std::vector<uint64_t>& slist,
                           std::atomic_bool *stop, QObject *parent,
                           LocalGen48Cache localGenCache)
    : QThread(parent)
    , m_client(client)
    , m_stop(false)
    , m_session(session)
    , m_mc(mc)
    , m_large(large)
    , m_searchtype(searchtype)
    , m_slist(slist)  // Copy the vector to avoid pointer invalidation
    , m_sharedStop(stop)
    , m_localGenCursor(nullptr)
    , m_localGenCache(localGenCache)
{
}

WorkerThread::~WorkerThread()
{
    stop();
    wait(1000);
}

void WorkerThread::stop()
{
    m_stop = true;
}

void WorkerThread::run()
{
    static std::atomic<int> threadInitCount(0);
    int threadNum = threadInitCount.fetch_add(1) + 1;
    
    // Only log first thread initialization for verification
    bool shouldLog = (threadNum == 1);
    
    // Initialize condition tree first, then pass to environment
    ConditionTree condtree;
    QString err = condtree.set(m_session.cv, m_mc);
    if (!err.isEmpty())
    {
        qWarning() << "WorkerThread: Failed to set condition tree:" << err;
        return;
    }

    if (m_client && m_client->m_localGen48)
    {
        if (m_localGenCache && !m_localGenCache->isEmpty())
        {
            if (shouldLog)
                qDebug() << "WorkerThread: First thread initialized with pre-computed local gen48 cache";
        }
        else
        {
            m_localGenCursor.reset(new LocalGen48Cursor());
            m_localGenCursor->mode = m_session.gen48.mode;
            m_localGenCursor->gen48 = m_session.gen48;
            m_localGenCursor->mc = m_mc;
            switch (m_session.gen48.qual)
            {
            case IDEAL_SALTED:
            case IDEAL:   m_localGenCursor->cst_type = CST_IDEAL; break;
            case CLASSIC: m_localGenCursor->cst_type = CST_CLASSIC; break;
            case NORMAL:  m_localGenCursor->cst_type = CST_NORMAL; break;
            case BARELY:  m_localGenCursor->cst_type = CST_BARELY; break;
            default:      m_localGenCursor->cst_type = CST_BARELY; break;
            }
            if (m_session.gen48.mode == GEN48_QH)
            {
                if (m_session.gen48.qual == IDEAL_SALTED)
                    m_localGenCursor->salt = m_session.gen48.salt;
                else
                {
                    StructureConfig sconf;
                    getStructureConfig_override(Swamp_Hut, m_mc, &sconf);
                    m_localGenCursor->salt = sconf.salt;
                }
            }
            else if (m_session.gen48.mode == GEN48_QM)
            {
                StructureConfig sconf;
                getStructureConfig_override(Monument, m_mc, &sconf);
                m_localGenCursor->salt = sconf.salt;
            }
            m_localGenCursor->init();
            if (shouldLog)
            {
                qDebug() << "WorkerThread: First thread initialized with local gen48 cursor";
            }
        }
    }
    
    err = m_env.init(m_mc, m_large, condtree);
    if (!err.isEmpty())
    {
        qWarning() << "WorkerThread: Failed to initialize environment:" << err;
        return;
    }
    
    m_env.stop = m_sharedStop;
    
    Pos origin = {0, 0};
    QVector<uint64_t> results;
    const std::vector<uint64_t>& slist = m_slist;
    
    // Process tasks until stopped
    while (!m_stop && !m_sharedStop->load())
    {
        TaskItem task;
        if (!m_client || !m_client->getNextTask(task))
            break; // No more tasks or client invalid
        
        results.clear();
        uint64_t sstart = task.sstart;
        uint64_t scnt = task.scnt;
        uint64_t idx = task.idx;
        uint64_t testedCount = 0; // actual number of seeds evaluated in this task
        
        switch (m_searchtype)
        {
        case SEARCH_LIST:
        {
            if (m_client && m_client->m_localGen48)
            {
                if (m_localGenCache)
                {
                    uint64_t ie = idx + scnt < (uint64_t)m_localGenCache->size() ? idx + scnt : (uint64_t)m_localGenCache->size();
                    for (uint64_t i = idx; i < ie; i++)
                    {
                        if (m_stop || m_sharedStop->load())
                            break;
                        uint64_t seed = (*m_localGenCache)[i];
                        m_env.setSeed(seed);
                        testedCount++;
                        if (testTreeAt(origin, &m_env, PASS_FULL_64, nullptr) == COND_OK)
                            results.append(seed);
                    }
                }
                else
                {
                    // Fallback to original method if cache is not available
                    QVector<uint64_t> slice;
                    if (generateLocal48Slice(idx, scnt, slice))
                    {
                        for (uint64_t seed : slice)
                        {
                            if (m_stop || m_sharedStop->load())
                                break;
                            m_env.setSeed(seed);
                            testedCount++;
                            if (testTreeAt(origin, &m_env, PASS_FULL_64, nullptr) == COND_OK)
                                results.append(seed);
                        }
                    }
                }
            }
            else
            {
                uint64_t ie = idx + scnt < slist.size() ? idx + scnt : slist.size();
                for (uint64_t i = idx; i < ie; i++)
                {
                    if (m_stop || m_sharedStop->load())
                        break;
                    uint64_t seed = slist[i];
                    m_env.setSeed(seed);
                    testedCount++;
                    if (testTreeAt(origin, &m_env, PASS_FULL_64, nullptr) == COND_OK)
                    {
                        results.append(seed);
                    }
                }
            }
            break;
        }
        
        case SEARCH_48ONLY:
        {
            if (m_client && m_client->m_localGen48)
            {
                if (m_localGenCache)
                {
                    uint64_t ie = idx + scnt < (uint64_t)m_localGenCache->size() ? idx + scnt : (uint64_t)m_localGenCache->size();
                    for (uint64_t i = idx; i < ie; i++)
                    {
                        if (m_stop || m_sharedStop->load())
                            break;
                        uint64_t seed = (*m_localGenCache)[i];
                        m_env.setSeed(seed);
                        testedCount++;
                        if (testTreeAt(origin, &m_env, PASS_FULL_48, nullptr) != COND_FAILED)
                            results.append(seed);
                    }
                }
                else
                {
                    // Fallback to original method if cache is not available
                    // Lazy init
                    if (!m_localGenCursor)
                    {
                        m_localGenCursor.reset(new LocalGen48Cursor());
                        m_localGenCursor->mode = m_session.gen48.mode;
                        m_localGenCursor->gen48 = m_session.gen48;
                        m_localGenCursor->mc = m_mc;
                        switch (m_session.gen48.qual)
                        {
                        case IDEAL_SALTED:
                        case IDEAL:   m_localGenCursor->cst_type = CST_IDEAL; break;
                        case CLASSIC: m_localGenCursor->cst_type = CST_CLASSIC; break;
                        case NORMAL:  m_localGenCursor->cst_type = CST_NORMAL; break;
                        case BARELY:  m_localGenCursor->cst_type = CST_BARELY; break;
                        default:      m_localGenCursor->cst_type = CST_BARELY; break;
                        }
                        if (m_session.gen48.mode == GEN48_QH)
                        {
                            if (m_session.gen48.qual == IDEAL_SALTED)
                                m_localGenCursor->salt = m_session.gen48.salt;
                            else
                            {
                                StructureConfig sconf;
                                getStructureConfig_override(Swamp_Hut, m_mc, &sconf);
                                m_localGenCursor->salt = sconf.salt;
                            }
                        }
                        else if (m_session.gen48.mode == GEN48_QM)
                        {
                            StructureConfig sconf;
                            getStructureConfig_override(Monument, m_mc, &sconf);
                            m_localGenCursor->salt = sconf.salt;
                        }
                        m_localGenCursor->init();
                    }
                    if (m_client->m_localGenSlicing)
                    {
                        // Flat slicing over low candidates only
                        uint64_t remaining = scnt;
                        uint64_t curIdx = idx;
                        while (remaining > 0)
                        {
                            uint64_t chunk = remaining;
                            QVector<uint64_t> slice;
                            if (!generateLocal48Slice(curIdx, chunk, slice) || slice.isEmpty())
                                break;
                            for (uint64_t low : slice)
                            {
                                if (m_stop || m_sharedStop->load())
                                    break;
                                m_env.setSeed(low);
                                testedCount++;
                                if (testTreeAt(origin, &m_env, PASS_FULL_48, nullptr) != COND_FAILED)
                                    results.append(low);
                            }
                            curIdx += slice.size();
                            if (static_cast<uint64_t>(slice.size()) >= remaining)
                                break;
                            remaining -= static_cast<uint64_t>(slice.size());
                        }
                    }
                    else
                    {
                        QVector<uint64_t> slice;
                        if (generateLocal48Slice(idx, scnt, slice))
                        {
                            // Debug: log first few localGen48 48-only candidates (thread 1 only)
                            static bool logged48 = false;
                            if (!logged48)
                            {
                                int n = std::min<int>(8, slice.size());
                                QStringList vals;
                                for (int i = 0; i < n; i++)
                                    vals << QString::asprintf("%012llx", (unsigned long long)slice[i]);
                                qDebug() << "Worker localGen48 (48-only) slice[0.." << n-1 << "]=" << vals.join(", ");
                                logged48 = true;
                            }
                            for (uint64_t seed : slice)
                            {
                                if (m_stop || m_sharedStop->load())
                                    break;
                                m_env.setSeed(seed);
                                testedCount++;
                                if (testTreeAt(origin, &m_env, PASS_FULL_48, nullptr) != COND_FAILED)
                                    results.append(seed);
                            }
                        }
                    }
                }
            }
            else if (!slist.empty())
            {
                uint64_t ie = idx + scnt < slist.size() ? idx + scnt : slist.size();
                for (uint64_t i = idx; i < ie; i++)
                {
                    if (m_stop || m_sharedStop->load())
                        break;
                    uint64_t seed = slist[i];
                    m_env.setSeed(seed);
                    testedCount++;
                    if (testTreeAt(origin, &m_env, PASS_FULL_48, nullptr) != COND_FAILED)
                    {
                        results.append(seed);
                    }
                }
            }
            else
            {
                uint64_t seed = sstart;
                for (uint64_t i = 0; i < scnt && seed <= MASK48; i++)
                {
                    if (m_stop || m_sharedStop->load())
                        break;
                    m_env.setSeed(seed);
                    testedCount++;
                    if (testTreeAt(origin, &m_env, PASS_FULL_48, nullptr) != COND_FAILED)
                    {
                        results.append(seed);
                    }
                    seed++;
                }
            }
            break;
        }
        
        case SEARCH_INC:
        {
            if (m_client && m_client->m_localGen48)
            {
                if (m_localGenCache)
                {
                    uint64_t high = (sstart >> 48) & 0xffff;
                    uint64_t lowidx = idx;
                    if (lowidx >= (uint64_t)m_localGenCache->size())
                    {
                        uint64_t wraps = lowidx / m_localGenCache->size();
                        lowidx = lowidx % m_localGenCache->size();
                        high += wraps;
                    }
                    for (uint64_t i = 0; i < scnt; i++)
                    {
                        if (m_stop || m_sharedStop->load() || high >= 0x10000)
                            break;
                        uint64_t seed = (high << 48) | (*m_localGenCache)[lowidx];
                        m_env.setSeed(seed);
                        testedCount++;
                        if (testTreeAt(origin, &m_env, PASS_FULL_64, nullptr) == COND_OK)
                            results.append(seed);
                        if (++lowidx >= (uint64_t)m_localGenCache->size())
                        {
                            lowidx = 0;
                            high++;
                        }
                    }
                }
                else
                {
                    // Fallback to original method
                    // Lazy initialization
                    if (!m_localGenCursor)
                    {
                        m_localGenCursor.reset(new LocalGen48Cursor());
                        m_localGenCursor->mode = m_session.gen48.mode;
                        m_localGenCursor->gen48 = m_session.gen48;
                        m_localGenCursor->mc = m_mc;
                        switch (m_session.gen48.qual)
                        {
                        case IDEAL_SALTED:
                        case IDEAL:   m_localGenCursor->cst_type = CST_IDEAL; break;
                        case CLASSIC: m_localGenCursor->cst_type = CST_CLASSIC; break;
                        case NORMAL:  m_localGenCursor->cst_type = CST_NORMAL; break;
                        case BARELY:  m_localGenCursor->cst_type = CST_BARELY; break;
                        default:      m_localGenCursor->cst_type = CST_BARELY; break;
                        }
                        if (m_session.gen48.mode == GEN48_QH)
                        {
                            if (m_session.gen48.qual == IDEAL_SALTED)
                                m_localGenCursor->salt = m_session.gen48.salt;
                            else
                            {
                                StructureConfig sconf;
                                getStructureConfig_override(Swamp_Hut, m_mc, &sconf);
                                m_localGenCursor->salt = sconf.salt;
                            }
                        }
                        else if (m_session.gen48.mode == GEN48_QM)
                        {
                            StructureConfig sconf;
                            getStructureConfig_override(Monument, m_mc, &sconf);
                            m_localGenCursor->salt = sconf.salt;
                        }
                        m_localGenCursor->init();
                    }
                    if (m_client->m_localGenSlicing)
                    {
                        if (m_localGenCount == 0)
                            m_localGenCount = m_client->m_localGenCount;
                        if (m_localGenCount == 0)
                            m_localGenCount = computeLocalGenCount();
                        if (m_localGenCount == 0)
                            break;
                        uint64_t startHigh = (m_session.sc.smin >> 48) & 0xffff;
                        uint64_t endHigh = (m_session.sc.smax >> 48) & 0xffff;
                        uint64_t totalHighs = (endHigh >= startHigh) ? (endHigh - startHigh + 1) : 0;
                        if (totalHighs == 0)
                            break;
                        uint64_t remaining = scnt;
                        uint64_t flat = idx;
                        while (remaining > 0)
                        {
                            uint64_t highIdx = flat / m_localGenCount;
                            if (highIdx >= totalHighs)
                                break;
                            uint64_t lowStart = flat % m_localGenCount;
                            uint64_t space = m_localGenCount - lowStart;
                            uint64_t chunk = remaining < space ? remaining : space;
                            QVector<uint64_t> lows;
                            if (!generateLocal48Slice(lowStart, chunk, lows) || lows.isEmpty())
                                break;
                            uint64_t high = startHigh + highIdx;
                            for (uint64_t low : lows)
                            {
                                if (m_stop || m_sharedStop->load())
                                    break;
                                uint64_t seed = (high << 48) | low;
                                // Bound check
                                if (seed < m_session.sc.smin || seed > m_session.sc.smax)
                                {
                                    // skip out-of-range
                                }
                                else
                                {
                                    m_env.setSeed(seed);
                                    testedCount++;
                                    if (testTreeAt(origin, &m_env, PASS_FULL_64, nullptr) == COND_OK)
                                        results.append(seed);
                                }
                            }
                            flat += lows.size();
                            if (static_cast<uint64_t>(lows.size()) >= remaining)
                                break;
                            remaining -= static_cast<uint64_t>(lows.size());
                        }
                    }
                    else
                    {
                        QVector<uint64_t> slice;
                        if (generateLocal48Slice(idx, scnt, slice))
                        {
                            // Debug: log first few localGen48 INC low candidates (thread 1 only)
                            static bool loggedInc = false;
                            if (!loggedInc)
                            {
                                int n = std::min<int>(8, slice.size());
                                QStringList vals;
                                for (int i = 0; i < n; i++)
                                    vals << QString::asprintf("%012llx", (unsigned long long)slice[i]);
                                qDebug() << "Worker localGen48 (INC) lows[0.." << n-1 << "]=" << vals.join(", ");
                                loggedInc = true;
                            }
                            uint64_t high = (sstart >> 48) & 0xffff;
                            uint64_t lowidx = 0;
                            for (uint64_t i = 0; i < scnt; i++)
                            {
                                if (m_stop || m_sharedStop->load())
                                    break;

                                if (lowidx >= (uint64_t)slice.size())
                                    break;
                                uint64_t seed = (high << 48) | slice[lowidx];
                                m_env.setSeed(seed);
                                testedCount++;
                                if (testTreeAt(origin, &m_env, PASS_FULL_64, nullptr) == COND_OK)
                                    results.append(seed);
                                if (++lowidx >= (uint64_t)slice.size())
                                {
                                    lowidx = 0;
                                    if (++high >= 0x10000)
                                        break;
                                }
                            }
                        }
                    }
                }
            }
            else if (!slist.empty())
            {
                uint64_t high = (sstart >> 48) & 0xffff;
                uint64_t lowidx = idx;
                
                // Ensure lowidx is within bounds before starting
                // If idx >= slist.size(), wrap around and adjust high bits
                if (lowidx >= slist.size())
                {
                    uint64_t wraps = lowidx / slist.size();
                    lowidx = lowidx % slist.size();
                    high += wraps;
                    if (high >= 0x10000)
                    {
                        // Already beyond valid range, skip this task
                        break;
                    }
                }
                
                for (uint64_t i = 0; i < scnt; i++)
                {
                    if (m_stop || m_sharedStop->load())
                        break;
                    
                    uint64_t seed = (high << 48) | slist[lowidx];
                    m_env.setSeed(seed);
                    testedCount++;
                    if (testTreeAt(origin, &m_env, PASS_FULL_64, nullptr) == COND_OK)
                    {
                        results.append(seed);
                    }
                    
                    if (++lowidx >= slist.size())
                    {
                        lowidx = 0;
                        if (++high >= 0x10000)
                            break;
                    }
                }
            }
            else
            {
                uint64_t seed = sstart;
                for (uint64_t i = 0; i < scnt && seed != ~(uint64_t)0; i++)
                {
                    if (m_stop || m_sharedStop->load())
                        break;
                    m_env.setSeed(seed);
                    testedCount++;
                    if (testTreeAt(origin, &m_env, PASS_FULL_64, nullptr) == COND_OK)
                    {
                        results.append(seed);
                    }
                    seed++;
                }
            }
            break;
        }
        
        case SEARCH_BLOCKS:
        {
            uint64_t high = (sstart >> 48) & 0xffff;
            uint64_t low;
            if (m_client && m_client->m_localGen48)
            {
                if (m_localGenCache)
                {
                    if (idx >= (uint64_t)m_localGenCache->size())
                        break;
                    low = (*m_localGenCache)[idx];
                }
                else
                {
                    // Fallback to original method
                    // Lazy initialization: if m_localGen48 is true but cursor not initialized
                    if (!m_localGenCursor)
                    {
                        // Don't log - initialization happens per thread
                        m_localGenCursor.reset(new LocalGen48Cursor());
                        m_localGenCursor->mode = m_session.gen48.mode;
                        m_localGenCursor->gen48 = m_session.gen48;
                        m_localGenCursor->mc = m_mc;
                        switch (m_session.gen48.qual)
                        {
                        case IDEAL_SALTED:
                        case IDEAL:   m_localGenCursor->cst_type = CST_IDEAL; break;
                        case CLASSIC: m_localGenCursor->cst_type = CST_CLASSIC; break;
                        case NORMAL:  m_localGenCursor->cst_type = CST_NORMAL; break;
                        case BARELY:  m_localGenCursor->cst_type = CST_BARELY; break;
                        default:      m_localGenCursor->cst_type = CST_BARELY; break;
                        }
                        if (m_session.gen48.mode == GEN48_QH)
                        {
                            if (m_session.gen48.qual == IDEAL_SALTED)
                                m_localGenCursor->salt = m_session.gen48.salt;
                            else
                            {
                                StructureConfig sconf;
                                getStructureConfig_override(Swamp_Hut, m_mc, &sconf);
                                m_localGenCursor->salt = sconf.salt;
                            }
                        }
                        else if (m_session.gen48.mode == GEN48_QM)
                        {
                            StructureConfig sconf;
                            getStructureConfig_override(Monument, m_mc, &sconf);
                            m_localGenCursor->salt = sconf.salt;
                        }
                        m_localGenCursor->init();
                    }
                    QVector<uint64_t> slice;
                    if (!generateLocal48Slice(idx, 1, slice) || slice.isEmpty())
                        break;
                    low = slice[0];
                }
            }
            else if (!slist.empty())
            {
                if (idx >= slist.size())
                    break;
                low = slist[idx];
            }
            else
            {
                low = sstart & MASK48;
            }
            
            m_env.setSeed(low);
            if (testTreeAt(origin, &m_env, PASS_FULL_48, nullptr) == COND_FAILED)
            {
                // No valid low bits, skip
                break;
            }
            
            for (uint64_t i = 0; i < scnt; i++)
            {
                if (m_stop || m_sharedStop->load())
                    break;
                uint64_t seed = (high << 48) | low;
                m_env.setSeed(seed);
                if (testTreeAt(origin, &m_env, PASS_FULL_64, nullptr) == COND_OK)
                {
                    results.append(seed);
                }
                
                if (++high >= 0x10000)
                    break;
            }
            break;
        }
        }
        
        // Send results
        if (!results.isEmpty())
        {
            static int matchCount = 0;
            matchCount += results.size();
            if (results.size() >= 1)  // Log when we find matches
            {
                qDebug() << "Worker: Found" << results.size() << "match(es) in task"
                         << "(total matches:" << matchCount << ")";
            }
            
            if (results.size() == 1)
            {
                emit taskResult(results[0]);
            }
            else
            {
                emit taskResults(results);
            }
        }
        
        // Send progress (aggregated per task to reduce signal frequency)
        // Note: Progress is already tracked per-task, so we send it once per task completion
        // Use testedCount (actual seeds tested) for accurate progress reporting
        uint64_t actualProgress = testedCount ? testedCount : scnt;
        emit taskProgress(actualProgress, sstart + actualProgress);
        
        // Notify task completion
        emit taskFinished();
    }
}

bool WorkerThread::generateLocal48Slice(uint64_t startIdx, uint64_t count, QVector<uint64_t>& out)
{
    if (!m_localGenCursor)
    {
        qCritical() << "WorkerThread::generateLocal48Slice() - m_localGenCursor is null!";
        return false;
    }

    // If out-of-order, reinit and fast-forward
    if (startIdx < m_localGenCursor->curIdx)
    {
        m_localGenCursor->init();
    }
    
    // Fast-forward to startIdx
    uint64_t toSkip = startIdx - m_localGenCursor->curIdx;
    uint64_t tmpSeed;
    while (toSkip > 0)
    {
        if (!m_localGenCursor->nextExpanded(tmpSeed))
        {
            qWarning() << "WorkerThread::generateLocal48Slice() - nextExpanded failed during skip";
            return false;
        }
        m_localGenCursor->curIdx++;
        toSkip--;
    }
    
    out.clear();
    out.reserve(count);
    for (uint64_t i = 0; i < count; i++)
    {
        if (!m_localGenCursor)
        {
            qCritical() << "WorkerThread::generateLocal48Slice() - m_localGenCursor became null at iteration" << i;
            break;
        }
        
        uint64_t s;
        if (!m_localGenCursor->nextExpanded(s))
            break;
        
        out.append(s);
        m_localGenCursor->curIdx++;
    }
    return !out.isEmpty();
}

uint64_t WorkerThread::computeLocalGenCount()
{
    if (!m_localGenCursor)
    {
        qCritical() << "WorkerThread::computeLocalGenCount() - m_localGenCursor is null!";
        return 0;
    }
    // Create a temporary cursor to avoid disturbing the working cursor
    std::unique_ptr<LocalGen48Cursor> tmp(new LocalGen48Cursor());
    tmp->mode = m_localGenCursor->mode;
    tmp->gen48 = m_localGenCursor->gen48;
    tmp->mc = m_localGenCursor->mc;
    tmp->salt = m_localGenCursor->salt;
    tmp->init();
    uint64_t cnt = 0;
    uint64_t s;
    while (tmp->nextExpanded(s))
    {
        cnt++;
        // Be cooperative if computation is long
        if ((cnt & 0xFFFF) == 0 && m_sharedStop && m_sharedStop->load())
            break;
    }
    return cnt;
}

void SearchWorkerClient::sendResult(uint64_t seed)
{
    QByteArray msg;
    writeUint32(msg, MSG_RESULT);
    writeUint64(msg, seed);
    sendMessage(msg);
}

void SearchWorkerClient::sendResults(const QVector<uint64_t>& seeds)
{
    QByteArray msg;
    writeUint32(msg, MSG_RESULTS);
    writeUint32(msg, seeds.size());
    for (uint64_t seed : seeds)
    {
        writeUint64(msg, seed);
    }
    sendMessage(msg);
}

// (duplicate LocalGen48Cursor definition removed)

void SearchWorkerClient::requestTask()
{
    if (!isConnected() || !m_configReceived)
    {
        // Only log errors, not normal flow
        return;
    }
    
    // Logging is done in MSG_TASK_ASSIGN handler (every Nth task)
    QByteArray msg;
    writeUint32(msg, MSG_TASK_REQUEST);
    sendMessage(msg);
    
    // Mark that we've requested a task
    QMutexLocker locker(&m_taskRequestMutex);
    m_taskRequestPending = true;
    m_inflightRequests++;
}

void SearchWorkerClient::checkTaskQueue()
{
    if (!isConnected() || !m_configReceived || m_stop)
        return;
    
    // Determine how many tasks we should have queued to keep threads busy
    int desiredQueued = qMax(m_threadCount, 4);
    
    // Current queue depth and in-flight requests
    int queueSize;
    {
        QMutexLocker taskLocker(&m_taskMutex);
        queueSize = static_cast<int>(m_taskQueue.size());
    }
    int inflight;
    {
        QMutexLocker requestLocker(&m_taskRequestMutex);
        inflight = m_inflightRequests;
    }
    
    int need = desiredQueued - (queueSize + inflight);
    if (need <= 0)
        return;
    
    // Request up to 'need' tasks to top up the queue
    for (int i = 0; i < need; i++)
    {
        requestTask();
    }
}

void SearchWorkerClient::sendHeartbeat()
{
    if (!isConnected())
        return;
    
    QByteArray msg;
    writeUint32(msg, MSG_PING);
    sendMessage(msg);
}

void SearchWorkerClient::logSearchRate()
{
    if (!m_configReceived || m_stop)
        return;
    
    // Calculate rate over the actual elapsed time (similar to coordinator's 20-second window approach)
    qint64 elapsedMs = m_logTimer.elapsed();
    if (elapsedMs <= 0)
        return;
    
    uint64_t currentTotal = m_logTotalSeeds.load(std::memory_order_relaxed);
    uint64_t seedsDelta = currentTotal - m_logLastSeeds;
    double elapsedSec = elapsedMs / 1000.0;
    
    // Always use actual elapsed time for accurate rate calculation
    // Match coordinator's approach of using actual time windows rather than fixed intervals
    if (m_logFirstInterval)
    {
        m_logFirstInterval = false;
        // Use actual elapsed time for first log
        if (elapsedSec < 1.0)
            return; // Wait at least 1 second before first log for accuracy
    }
    else
    {
        // For subsequent logs, use actual elapsed time since last log
        // Restart timer after reading to prepare for next interval
        m_logTimer.restart();
    }
    
    m_logLastSeeds = currentTotal;
    
    if (elapsedSec <= 0.0)
        return;
    
    double seedsPerSec = seedsDelta / elapsedSec;
    
    QString rateStr;
    if (seedsPerSec >= 1000000)
        rateStr = QString::asprintf("%.2f M seeds/sec", seedsPerSec / 1000000.0);
    else if (seedsPerSec >= 1000)
        rateStr = QString::asprintf("%.2f K seeds/sec", seedsPerSec / 1000.0);
    else
        rateStr = QString::asprintf("%.1f seeds/sec", seedsPerSec);
    
    qDebug() << QString("Worker search rate: %1 (%2 threads, %3 seeds in last %4 s)")
                .arg(rateStr)
                .arg(m_threadCount)
                .arg(seedsDelta)
                .arg(QString::number(elapsedSec, 'f', 1));
}

void SearchWorkerClient::onDisconnected()
{
    if (m_socket)
    {
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_heartbeatTimer->stop();
    m_taskTimer->stop();
    m_configReceived = false;
    m_readBuffer.clear();
    emit disconnected();
    qDebug() << "Disconnected from coordinator, waiting for reconnection...";
    // Server keeps listening, so coordinator can reconnect
}

void SearchWorkerClient::onError(QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError);
    emit clientError(m_socket->errorString());
}

