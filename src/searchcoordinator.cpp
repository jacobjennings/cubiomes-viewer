#include "searchcoordinator.h"
#include "searchthread.h"

#include "cubiomes/util.h"
#include "config.h"

#include <QDebug>
#include <QDataStream>
#include <QHostAddress>

SearchCoordinatorClient::SearchCoordinatorClient(QTcpSocket *socket, QObject *parent)
    : QObject(parent)
    , m_socket(socket)
    , m_ready(false)
    , m_pendingTask(false)
    , m_currentProg(0)
    , m_currentSeed(0)
    , m_totalSeedsProcessed(0)
{
    m_progressTimer.start();
    connect(m_socket, &QTcpSocket::readyRead, this, &SearchCoordinatorClient::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &SearchCoordinatorClient::onDisconnected);
    connect(m_socket, &QAbstractSocket::errorOccurred,
            this, &SearchCoordinatorClient::onError);
}

SearchCoordinatorClient::~SearchCoordinatorClient()
{
    if (m_socket)
        m_socket->deleteLater();
}

void SearchCoordinatorClient::sendTask(uint64_t sstart, uint64_t scnt, uint64_t idx)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return;

    QByteArray msg;
    writeUint32(msg, MSG_TASK_ASSIGN);
    writeUint64(msg, sstart);
    writeUint64(msg, scnt);
    writeUint64(msg, idx);
    
    // Send message length then message
    uint32_t len = qToBigEndian((uint32_t)msg.size());
    m_socket->write(reinterpret_cast<const char*>(&len), 4);
    m_socket->write(msg);
    m_socket->flush();
    
    m_pendingTask = true;
    // Don't log every task - too verbose
}

void SearchCoordinatorClient::sendStop()
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return;

    QByteArray msg;
    writeUint32(msg, MSG_STOP);
    
    uint32_t len = qToBigEndian((uint32_t)msg.size());
    m_socket->write(reinterpret_cast<const char*>(&len), 4);
    m_socket->write(msg);
    m_socket->flush();
}

void SearchCoordinatorClient::sendConfig(const Session& session)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return;

    QByteArray msg;
    writeUint32(msg, MSG_CONFIG);
    writeUint32(msg, SEARCH_PROTOCOL_VERSION);
    writeInt32(msg, session.wi.mc);
    writeUint32(msg, session.wi.large ? 1 : 0);
    writeInt32(msg, session.sc.searchtype);
    writeUint64(msg, session.sc.startseed);
    writeUint64(msg, session.sc.smin);
    writeUint64(msg, session.sc.smax);
    writeUint32(msg, session.gen48.mode);
    writeUint64(msg, session.gen48.salt);
    writeUint64(msg, session.gen48.listsalt);
    writeInt32(msg, session.gen48.qual);
    writeInt32(msg, session.gen48.qmarea);
    writeInt32(msg, session.gen48.x1);
    writeInt32(msg, session.gen48.z1);
    writeInt32(msg, session.gen48.x2);
    writeInt32(msg, session.gen48.z2);
    
    // Conditions
    writeUint32(msg, session.cv.size());
    for (const Condition& c : session.cv)
    {
        msg.append(reinterpret_cast<const char*>(&c), sizeof(Condition));
    }
    
    // Decide whether workers should generate seeds locally based on generator mode.
    // Always enable local generation for quad-hut / monument to avoid transmitting
    // very large seed lists over the wire, regardless of search type.
    const bool localGen48 = (session.gen48.mode == GEN48_QH || session.gen48.mode == GEN48_QM);

    // Seed list: for SEARCH_LIST we stream seeds in tasks; for QH/QM avoid sending massive list
    if (session.sc.searchtype == SEARCH_LIST || localGen48)
    {
        writeUint32(msg, 0); // no slist in config
    }
    else
    {
        writeUint32(msg, session.slist.size());
        for (uint64_t s : session.slist)
        {
            writeUint64(msg, s);
        }
    }

    // Thread count (for worker to know how many threads to use)
    writeInt32(msg, session.sc.threads);

    // Indicate to worker whether to generate 48-bit candidate list locally (e.g., Quad-hut/Monument)
    writeUint32(msg, localGen48 ? 1u : 0u);
    // Disable special slicing mode (use shared SearchMaster queue semantics)
    writeUint32(msg, 0u);
    // Provide low-candidate count to avoid per-thread pre-counting on workers
    uint64_t lowCount = localGen48 ? (uint64_t)session.slist.size() : 0ULL;
    writeUint64(msg, lowCount);
    
    uint32_t len = qToBigEndian((uint32_t)msg.size());
    m_socket->write(reinterpret_cast<const char*>(&len), 4);
    m_socket->write(msg);
    m_socket->flush();
}

void SearchCoordinatorClient::onReadyRead()
{
    QByteArray msg;
    while (readMessage(msg))
    {
        processMessage(msg);
    }
}

bool SearchCoordinatorClient::readMessage(QByteArray& msg)
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
    // Assume network byte order (big-endian) - Qt will handle conversion
    msgLen = qFromBigEndian(msgLen);
    
    // Read complete message
    while (m_readBuffer.size() < (int)(4 + msgLen))
    {
        QByteArray data = m_socket->read(4096);
        if (data.isEmpty())
            return false;
        m_readBuffer.append(data);
    }
    
    msg = m_readBuffer.mid(4, msgLen);
    m_readBuffer.remove(0, 4 + msgLen);
    
    return true;
}

void SearchCoordinatorClient::processMessage(const QByteArray& msg)
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
            emit workerError(QString("Protocol version mismatch: expected %1, got %2")
                      .arg(SEARCH_PROTOCOL_VERSION).arg(version));
            return;
        }
        if (!m_ready)
        {
            m_ready = true;
            emit ready();
        }
        break;
    }
    
    case MSG_TASK_REQUEST:
        m_pendingTask = false;
        emit taskRequested();
        break;
    
    case MSG_RESULT:
    {
        uint64_t seed = readUint64(msg, offset);
        emit result(seed);
        break;
    }
    
    case MSG_RESULTS:
    {
        uint32_t count = readUint32(msg, offset);
        QVector<uint64_t> seeds;
        seeds.reserve(count);
        for (uint32_t i = 0; i < count; i++)
        {
            seeds.append(readUint64(msg, offset));
        }
        emit results(seeds);
        break;
    }
    
    case MSG_PROGRESS:
    {
        uint64_t prog = readUint64(msg, offset);
        uint64_t seed = readUint64(msg, offset);
        m_currentProg = prog;
        m_currentSeed = seed;
        
        // Track progress for seeds/sec calculation
        // prog is the number of seeds processed in the current task, accumulate it
        m_totalSeedsProcessed += prog;
        
        qint64 now = m_progressTimer.elapsed();
        ProgressPoint point = { now, m_totalSeedsProcessed };
        m_progressHistory.push_back(point);
        
        // Keep only last 20 seconds of history
        qint64 cutoff = now - 20000;
        while (!m_progressHistory.empty() && m_progressHistory.front().timestamp < cutoff)
        {
            m_progressHistory.pop_front();
        }
        
        // Don't log progress updates - too verbose
        
        emit progress(prog, seed);
        break;
    }
    
    case MSG_DONE:
        // Don't log MSG_DONE - too verbose
        m_pendingTask = false;
        break;
    
    case MSG_ERROR:
    {
        QString err = readString(msg, offset);
        emit workerError(err);
        break;
    }
    
    case MSG_PING:
        // Worker is sending heartbeat ping, respond with pong
        {
            QByteArray pong;
            writeUint32(pong, MSG_PONG);
            uint32_t len = qToBigEndian((uint32_t)pong.size());
            m_socket->write(reinterpret_cast<const char*>(&len), 4);
            m_socket->write(pong);
            m_socket->flush();
        }
        break;
    
    case MSG_PONG:
        // Heartbeat response (if we ever send pings to workers)
        break;
    
    default:
        qWarning() << "Unknown message type:" << type;
        break;
    }
}

void SearchCoordinatorClient::onDisconnected()
{
    emit disconnected();
}

void SearchCoordinatorClient::onError(QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError);
    emit workerError(m_socket->errorString());
}

QString SearchCoordinatorClient::getHostname() const
{
    if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState)
    {
        return m_socket->peerAddress().toString();
    }
    return QString();
}

double SearchCoordinatorClient::getSeedsPerSecond() const
{
    if (m_progressHistory.empty())
        return 0.0;
    
    // If we have only one point, use time since timer started
    if (m_progressHistory.size() == 1)
    {
        const ProgressPoint& point = m_progressHistory.front();
        qint64 elapsed = point.timestamp;
        if (elapsed <= 0)
            return 0.0;
        double seconds = elapsed / 1000.0;
        if (seconds <= 0.0)
            return 0.0;
        return point.totalSeeds / seconds;
    }
    
    // With 2+ points, calculate rate over the time window
    const ProgressPoint& first = m_progressHistory.front();
    const ProgressPoint& last = m_progressHistory.back();
    
    qint64 timeDelta = last.timestamp - first.timestamp;
    if (timeDelta <= 0)
        return 0.0;
    
    uint64_t seedsDelta = (last.totalSeeds > first.totalSeeds) ? 
                          (last.totalSeeds - first.totalSeeds) : 0;
    
    // Convert from milliseconds to seconds
    double seconds = timeDelta / 1000.0;
    if (seconds <= 0.0)
        return 0.0;
    
    return seedsDelta / seconds;
}


SearchCoordinator::SearchCoordinator(QObject *parent)
    : QObject(parent)
    , m_workerPort(23473)
    , m_searchMaster(nullptr)
    , m_searching(false)
    , m_stop(false)
    , m_searchtype(0)
    , m_mc(0)
    , m_large(false)
    , m_idx(0)
    , m_scnt(0)
    , m_prog(0)
    , m_seed(0)
    , m_smin(0)
    , m_smax(~(uint64_t)0)
    , m_isdone(false)
    , m_itemsize(100000)  // Start with 100k seeds per chunk for distributed workers
    , m_count(0)
{
}

SearchCoordinator::~SearchCoordinator()
{
    disconnectFromWorkers();
}

bool SearchCoordinator::connectToWorkers(const QStringList& workerHosts, quint16 port)
{
    m_workerHosts = workerHosts;
    m_workerPort = port;
    
    // Connect to each worker
    for (const QString& host : workerHosts)
    {
        connectToWorker(host, port);
    }
    
    return true;
}

void SearchCoordinator::disconnectFromWorkers()
{
    QMutexLocker locker(&m_mutex);
    for (SearchCoordinatorClient* client : m_clients)
    {
        client->sendStop();
        client->disconnect();
    }
    qDeleteAll(m_clients);
    m_clients.clear();
}

bool SearchCoordinator::isConnected() const
{
    QMutexLocker locker(&m_mutex);
    return !m_clients.isEmpty();
}

void SearchCoordinator::connectToWorker(const QString& hostname, quint16 port)
{
    // Check if we're already connected to this host (checking both hostname and socket state)
    QMutexLocker locker(&m_mutex);
    for (SearchCoordinatorClient* c : m_clients)
    {
        if (c->socket() && c->socket()->state() == QAbstractSocket::ConnectedState)
        {
            QString peerAddr = c->socket()->peerAddress().toString();
            // Match by hostname (could be IP or hostname)
            if (peerAddr == hostname || peerAddr.contains(hostname) || hostname.contains(peerAddr))
            {
                qDebug() << "Already connected to" << hostname;
                return;
            }
        }
    }
    locker.unlock();
    
    QTcpSocket *socket = new QTcpSocket(this);
    SearchCoordinatorClient *client = new SearchCoordinatorClient(socket, this);
    
    connect(socket, &QTcpSocket::connected, this, &SearchCoordinator::onConnected);
    connect(socket, &QAbstractSocket::errorOccurred, this, &SearchCoordinator::onConnectionFailed);
    connect(client, &SearchCoordinatorClient::taskRequested, this, &SearchCoordinator::onTaskRequested);
    connect(client, &SearchCoordinatorClient::result, this, &SearchCoordinator::onWorkerResult);
    connect(client, &SearchCoordinatorClient::results, this, &SearchCoordinator::onWorkerResults);
    connect(client, &SearchCoordinatorClient::progress, this, &SearchCoordinator::onWorkerProgress);
    connect(client, &SearchCoordinatorClient::disconnected, this, &SearchCoordinator::onClientDisconnected);
    connect(client, &SearchCoordinatorClient::workerError, this, [this](const QString& msg) {
        qWarning() << "Worker error:" << msg;
    });
    connect(client, &SearchCoordinatorClient::ready, this, [this, client]() {
        QMutexLocker locker(&m_mutex);
        if (m_searching && client->isReady())
        {
            client->sendConfig(m_session);
        }
    }, Qt::QueuedConnection);
    
    {
        QMutexLocker locker(&m_mutex);
        m_clients.append(client);
    }
    
    qDebug() << "Connecting to worker at" << hostname << ":" << port;
    socket->connectToHost(hostname, port);
}

void SearchCoordinator::setSession(const Session& session)
{
    QMutexLocker locker(&m_mutex);
    m_session = session;
}

void SearchCoordinator::setSearchMaster(SearchMaster* master)
{
    QMutexLocker locker(&m_mutex);
    m_searchMaster = master;
}

void SearchCoordinator::startSearch()
{
    QMutexLocker locker(&m_mutex);
    
    if (m_searching)
        return;
    
    m_searching = true;
    m_stop = false;
    
    // Copy search state from session (similar to SearchMaster::preSearch)
    m_searchtype = m_session.sc.searchtype;
    m_mc = m_session.wi.mc;
    m_large = m_session.wi.large;
    m_slist = m_session.slist;
    m_smin = m_session.sc.smin;
    m_smax = m_session.sc.smax;
    m_seed = m_session.sc.startseed;
    m_itemsize = 100000;  // 100k seeds per chunk - appropriate for ~300k seeds/sec
    
    // Disable special slicing (use SearchMaster-shared queue to coordinate with local workers)
    m_localGenSlicingActive = false;
    
    // Initialize search state (simplified version of SearchMaster::preSearch)
    if (m_searchtype == SEARCH_LIST && !m_slist.empty())
    {
        m_scnt = m_slist.size();
        m_idx = 0;
        for (m_idx = 0; m_idx < m_scnt; m_idx++)
            if (m_slist[m_idx] == m_seed)
                break;
        if (m_idx == m_scnt)
            m_idx = 0;
        m_prog = m_idx;
    }
    else if (m_searchtype == SEARCH_48ONLY)
    {
        if (!m_slist.empty())
        {
            m_scnt = m_slist.size();
            m_idx = 0;
            for (m_idx = 0; m_idx < m_scnt; m_idx++)
                if (m_slist[m_idx] == (m_seed & MASK48))
                    break;
            if (m_idx == m_scnt)
                m_idx = 0;
            m_prog = m_idx;
        }
        else
        {
            m_prog = m_seed;
            m_scnt = MASK48;
        }
    }
    else if (m_searchtype == SEARCH_INC)
    {
        if (!m_slist.empty())
        {
            m_prog = 0;
            m_scnt = 0x10000 * m_slist.size();
        }
        else
        {
            if (m_seed < m_smin)
                m_seed = m_smin;
            m_prog = m_seed - m_smin;
            m_scnt = m_smax - m_smin;
        }
    }
    else if (m_searchtype == SEARCH_BLOCKS)
    {
        if (!m_slist.empty())
        {
            m_scnt = 0x10000 * m_slist.size();
            m_idx = 0;
            m_prog = 0;
        }
        else
        {
            m_scnt = ~(uint64_t)0;
            m_prog = 0;
        }
    }
    
    m_isdone = false;
    // Note: m_prog is already set above based on search type, don't reset it to 0
    // as that would lose the initial progress position
    m_count = 0;
    m_itemtimer.start();
    
    // Send config to all connected clients
    for (SearchCoordinatorClient* client : m_clients)
    {
        if (client->isReady())
        {
            client->sendConfig(m_session);
        }
    }
    
    // Start distributing tasks (mutex is already locked)
    distributeTasksLocked();
}

void SearchCoordinator::stopSearch()
{
    QMutexLocker locker(&m_mutex);
    m_stop = true;
    m_searching = false;
    
    for (SearchCoordinatorClient* client : m_clients)
    {
        client->sendStop();
    }
}

bool SearchCoordinator::getNextTask(uint64_t *sstart, uint64_t *scnt, uint64_t *idx)
{
    QMutexLocker locker(&m_mutex);
    return getNextTaskLocked(sstart, scnt, idx);
}

bool SearchCoordinator::getNextTaskLocked(uint64_t *sstart, uint64_t *scnt, uint64_t *idx)
{
    // Special slicing disabled

    // If we have a SearchMaster reference, delegate to it for coordinated task distribution
    // This prevents duplication of work between local and distributed workers
    if (m_searchMaster)
    {
        // Distributed workers need larger batches due to network latency
        // Target batch size: 100k seeds (good for ~300k seeds/sec with reasonable latency)
        const uint64_t TARGET_BATCH_SIZE = 100000;
        
        // We need to unlock our mutex before calling SearchMaster::requestItem to avoid deadlock
        m_mutex.unlock();
        
        // We need to lock SearchMaster's mutex to call requestItem
        // This is done inside requestItem via QMutexLocker
        bool hasTask = m_searchMaster->mutex.tryLock(100); // Try for 100ms
        if (!hasTask)
        {
            m_mutex.lock(); // Re-lock before returning
            return false; // Couldn't get lock, try again later
        }
        
        if (m_searchMaster->isdone || *m_searchMaster->env.stop)
        {
            m_searchMaster->mutex.unlock();
            m_mutex.lock(); // Re-lock before returning
            m_isdone = true;
            return false;
        }
        
        // Special case: For SEARCH_INC with a list, batching is complex due to idx wraparound
        // When idx wraps around the list, high bits increment, making batching non-trivial
        // For now, use a simpler approach: take a fixed batch size without complex batching
        bool canBatchSimply = true;
        if (m_searchMaster->searchtype == SEARCH_INC && !m_searchMaster->slist.empty())
        {
            // For SEARCH_INC with list, just take one larger chunk directly
            // This avoids the wraparound complexity
            canBatchSimply = false;
        }
        
        uint64_t batchStart = m_searchMaster->seed;
        uint64_t batchIdx = m_searchMaster->idx;
        uint64_t accumulated = 0;
        
        // Debug: Log what we're capturing
        static int captureNum = 0;
        if (++captureNum % 10 == 0)
        {
            qDebug() << "Coordinator: Capturing state #" << captureNum
                     << "seed:" << QString::asprintf("0x%llx", (unsigned long long)batchStart)
                     << "idx:" << batchIdx
                     << "listsize:" << m_searchMaster->slist.size();
        }
        
        if (!canBatchSimply)
        {
            // Simple case: just take TARGET_BATCH_SIZE directly
            accumulated = TARGET_BATCH_SIZE;
            m_searchMaster->prog += accumulated;
            
            if (m_searchMaster->searchtype == SEARCH_INC && !m_searchMaster->slist.empty())
            {
                uint64_t high = (m_searchMaster->seed >> 48) & 0xffff;
                m_searchMaster->idx += accumulated;
                high += m_searchMaster->idx / m_searchMaster->slist.size();
                m_searchMaster->idx %= m_searchMaster->slist.size();
                m_searchMaster->seed = (high << 48) | m_searchMaster->slist[m_searchMaster->idx];
                if (high > (m_searchMaster->smax >> 48))
                    m_searchMaster->isdone = true;
                if (m_searchMaster->seed > m_searchMaster->smax)
                    m_searchMaster->isdone = true;
            }
            
            // For SEARCH_INC with list: idx represents wrapped position in list (not cumulative)
            // Worker expects wrapped idx matching the high bits in sstart
            *sstart = batchStart;
            *scnt = accumulated;
            *idx = batchIdx;  // Already wrapped position in list
            
            // Debug: Log task distribution for SEARCH_INC
            static int taskNum = 0;
            if (++taskNum % 10 == 0)  // Log every 10th task
            {
                qDebug() << "Coordinator: Sending SEARCH_INC task #" << taskNum
                         << "sstart:" << QString::asprintf("0x%llx", (unsigned long long)batchStart)
                         << "idx:" << batchIdx
                         << "scnt:" << accumulated;
            }
            
            m_isdone = m_searchMaster->isdone;
            m_searchMaster->mutex.unlock();
            m_mutex.lock();
            return true;
        }
        
        // Pull tasks until we reach target batch size or run out of work
        while (accumulated < TARGET_BATCH_SIZE && !m_searchMaster->isdone && !*m_searchMaster->env.stop)
        {
            uint64_t itemsize = m_searchMaster->itemsize;
            
            // Check how much we can take without exceeding limits
            uint64_t canTake = itemsize;
            
            // Adjust for search type limits
            if (m_searchMaster->searchtype == SEARCH_LIST)
            {
                if (m_searchMaster->idx + canTake > m_searchMaster->scnt)
                    canTake = m_searchMaster->scnt - m_searchMaster->idx;
            }
            else if (m_searchMaster->searchtype == SEARCH_48ONLY && !m_searchMaster->slist.empty())
            {
                if (m_searchMaster->idx + canTake > m_searchMaster->scnt)
                    canTake = m_searchMaster->scnt - m_searchMaster->idx;
            }
            
            // Would this batch exceed our target? If so, take what we need and stop
            if (accumulated + canTake >= TARGET_BATCH_SIZE)
            {
                canTake = TARGET_BATCH_SIZE - accumulated;
                if (canTake == 0)
                    break; // Already at target
            }
            
            // Advance SearchMaster's state
            m_searchMaster->prog += canTake;
            accumulated += canTake;
            
            if (m_searchMaster->searchtype == SEARCH_LIST)
            {
                m_searchMaster->idx += canTake;
                if (m_searchMaster->idx >= m_searchMaster->scnt)
                    m_searchMaster->isdone = true;
            }
            else if (m_searchMaster->searchtype == SEARCH_48ONLY)
            {
                if (!m_searchMaster->slist.empty())
                {
                    m_searchMaster->idx += canTake;
                    if (m_searchMaster->idx >= m_searchMaster->scnt)
                        m_searchMaster->isdone = true;
                }
                else
                {
                    m_searchMaster->seed += canTake;
                    if (m_searchMaster->seed > MASK48)
                        m_searchMaster->isdone = true;
                }
            }
            else if (m_searchMaster->searchtype == SEARCH_INC)
            {
                if (!m_searchMaster->slist.empty())
                {
                    uint64_t high = (m_searchMaster->seed >> 48) & 0xffff;
                    m_searchMaster->idx += canTake;
                    high += m_searchMaster->idx / m_searchMaster->slist.size();
                    m_searchMaster->idx %= m_searchMaster->slist.size();
                    m_searchMaster->seed = (high << 48) | m_searchMaster->slist[m_searchMaster->idx];
                    if (high > (m_searchMaster->smax >> 48))
                        m_searchMaster->isdone = true;
                }
                else
                {
                    uint64_t s = m_searchMaster->seed + canTake;
                    if (s < m_searchMaster->seed)
                        m_searchMaster->isdone = true;
                    m_searchMaster->seed = s;
                }
                if (m_searchMaster->seed > m_searchMaster->smax)
                    m_searchMaster->isdone = true;
            }
            else if (m_searchMaster->searchtype == SEARCH_BLOCKS)
            {
                if (!m_searchMaster->slist.empty())
                {
                    uint64_t high = (m_searchMaster->seed >> 48) & 0xffff;
                    high += canTake;
                    if (high >= 0x10000)
                    {
                        high = 0;
                        m_searchMaster->idx++;
                    }
                    if (m_searchMaster->idx >= m_searchMaster->slist.size())
                        m_searchMaster->isdone = true;
                    else
                        m_searchMaster->seed = (high << 48) | m_searchMaster->slist[m_searchMaster->idx];
                }
                else
                {
                    uint64_t high = (m_searchMaster->seed >> 48) & 0xffff;
                    high += canTake;
                    if (high >= 0x10000)
                    {
                        // For BLOCKS search without list, we can't easily batch across 48-bit boundaries
                        // Just take what we accumulated so far
                        m_searchMaster->isdone = true; // Will be handled by master
                        break;
                    }
                    m_searchMaster->seed = (high << 48) | (m_searchMaster->seed & MASK48);
                }
            }
            
            // If we've reached target or are done, stop batching
            if (accumulated >= TARGET_BATCH_SIZE || m_searchMaster->isdone)
                break;
        }
        
        // Return the batched task
        *sstart = batchStart;
        *scnt = accumulated;
        *idx = batchIdx;
        
        m_isdone = m_searchMaster->isdone;
        m_searchMaster->mutex.unlock();
        m_mutex.lock(); // Re-lock before returning
        
        return accumulated > 0;
    }
    
    // Fallback: If no SearchMaster, use internal task distribution (old behavior)
    if (m_isdone || m_stop)
        return false;
    
    // Similar logic to SearchMaster::requestItem
    *sstart = m_seed;
    *scnt = m_itemsize;
    *idx = m_idx;
    
    m_prog += m_itemsize;
    
    // Update state based on search type
    if (m_searchtype == SEARCH_LIST)
    {
        if (m_idx + m_itemsize > m_scnt)
            *scnt = m_scnt - m_idx;
        m_idx += *scnt;
        if (m_idx >= m_scnt)
            m_isdone = true;
    }
    else if (m_searchtype == SEARCH_48ONLY)
    {
        if (!m_slist.empty())
        {
            if (m_idx + m_itemsize > m_scnt)
                *scnt = m_scnt - m_idx;
            m_idx += *scnt;
            if (m_idx >= m_scnt)
                m_isdone = true;
        }
        else
        {
            m_seed += m_itemsize;
            if (m_seed > MASK48)
                m_isdone = true;
        }
    }
    else if (m_searchtype == SEARCH_INC)
    {
        if (!m_slist.empty())
        {
            uint64_t high = (m_seed >> 48) & 0xffff;
            m_idx += m_itemsize;
            high += m_idx / m_slist.size();
            m_idx %= m_slist.size();
            m_seed = (high << 48) | m_slist[m_idx];
            if (high > (m_smax >> 48))
                m_isdone = true;
        }
        else
        {
            uint64_t s = m_seed + m_itemsize;
            if (s < m_seed)
                m_isdone = true;
            m_seed = s;
        }
        if (m_seed > m_smax)
            m_isdone = true;
    }
    else if (m_searchtype == SEARCH_BLOCKS)
    {
        if (!m_slist.empty())
        {
            uint64_t high = (m_seed >> 48) & 0xffff;
            high += m_itemsize;
            if (high >= 0x10000)
            {
                high = 0;
                m_idx++;
            }
            if (m_idx >= m_slist.size())
                m_isdone = true;
            else
                m_seed = (high << 48) | m_slist[m_idx];
        }
        else
        {
            uint64_t high = (m_seed >> 48) & 0xffff;
            high += m_itemsize;
            if (high >= 0x10000)
            {
                *scnt -= 0x10000 - high;
                high = 0;
                uint64_t low = (m_seed & MASK48) + 1;
                m_seed = (high << 48) | low;
            }
            else
            {
                m_seed = (high << 48) | (m_seed & MASK48);
            }
        }
    }
    
    return true;
}

QVector<uint64_t> SearchCoordinator::getResults()
{
    QMutexLocker locker(&m_resultsMutex);
    QVector<uint64_t> results = m_results;
    m_results.clear();
    return results;
}

QVector<SearchCoordinator::WorkerStats> SearchCoordinator::getWorkerStats() const
{
    QMutexLocker locker(&m_mutex);
    QVector<WorkerStats> stats;
    
    for (SearchCoordinatorClient* client : m_clients)
    {
        WorkerStats ws;
        ws.hostname = client->getHostname();
        ws.seedsPerSecond = client->getSeedsPerSecond();
        ws.isActive = client->isReady() && (client->hasPendingTask() || m_searching);
        stats.append(ws);
    }
    
    return stats;
}

bool SearchCoordinator::getProgress(uint64_t *prog, uint64_t *end, uint64_t *seed)
{
    QMutexLocker locker(&m_mutex);
    *prog = m_prog;
    *end = m_scnt;
    *seed = m_seed;
    
    // Could track per-client progress here if needed
    
    return true;
}

void SearchCoordinator::onConnected()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket)
        return;
    
    QMutexLocker locker(&m_mutex);
    
    // Find the client for this socket
    SearchCoordinatorClient* client = nullptr;
    for (SearchCoordinatorClient* c : m_clients)
    {
        if (c->socket() == socket)
        {
            client = c;
            break;
        }
    }
    
    if (!client)
        return;
    
    // Send hello
    QByteArray hello;
    writeUint32(hello, MSG_HELLO);
    writeUint32(hello, SEARCH_PROTOCOL_VERSION);
    
    uint32_t len = qToBigEndian((uint32_t)hello.size());
    socket->write(reinterpret_cast<const char*>(&len), 4);
    socket->write(hello);
    socket->flush();
    
    emit clientConnected();
    qDebug() << "Connected to worker at" << socket->peerAddress().toString();
    // Config will be sent when client becomes ready (via ready signal)
}

void SearchCoordinator::onConnectionFailed()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket)
        return;
    
    QMutexLocker locker(&m_mutex);
    
    // Find the client for this socket
    SearchCoordinatorClient* client = nullptr;
    for (SearchCoordinatorClient* c : m_clients)
    {
        if (c->socket() == socket)
        {
            client = c;
            break;
        }
    }
    
    if (client)
    {
        qWarning() << "Failed to connect to worker:" << socket->errorString();
        // Keep client in list for potential retry, but socket will be cleaned up
    }
}

void SearchCoordinator::onTaskRequested()
{
    // Ensure we're ready to distribute tasks
    distributeTasks();
    
    // If search hasn't started yet, this will be a no-op
    // but that's okay - tasks will be sent when search starts
}

void SearchCoordinator::distributeTasks()
{
    QMutexLocker locker(&m_mutex);
    distributeTasksLocked();
}

void SearchCoordinator::distributeTasksLocked()
{
    if (!m_searching || m_stop)
    {
        // Don't log - too verbose
        return;
    }
    
    // Find idle clients and send tasks
    for (SearchCoordinatorClient* client : m_clients)
    {
        if (!client->isReady())
        {
            // Don't log - too verbose
            continue;
        }
        if (client->hasPendingTask())
        {
            // Don't log - too verbose
            continue;
        }
        
        uint64_t sstart, scnt, idx;
        if (getNextTaskLocked(&sstart, &scnt, &idx))
        {
            client->sendTask(sstart, scnt, idx);
        }
        else
        {
            // Don't log - no more tasks is normal end state
            break;
        }
    }
}

// Note: List-based tasks are not sent to workers; workers operate on ranges only.

void SearchCoordinator::onWorkerResult(uint64_t seed)
{
    SearchCoordinatorClient* client = qobject_cast<SearchCoordinatorClient*>(sender());
    QString hostname = client ? client->getHostname() : QString("Unknown");
    
    {
        QMutexLocker locker(&m_resultsMutex);
        // Prevent unbounded growth - if buffer is too large, drop oldest results
        if (m_results.size() >= MAX_RESULTS_BUFFER)
        {
            // Remove oldest 10% to make room
            int removeCount = MAX_RESULTS_BUFFER / 10;
            m_results.remove(0, removeCount);
            qWarning() << "SearchCoordinator: Results buffer full, dropped" << removeCount << "oldest results";
        }
        m_results.append(seed);
    }
    emit searchResult(seed, hostname);
    
    // Continue distributing tasks
    distributeTasks();
}

void SearchCoordinator::onWorkerResults(const QVector<uint64_t>& seeds)
{
    SearchCoordinatorClient* client = qobject_cast<SearchCoordinatorClient*>(sender());
    QString hostname = client ? client->getHostname() : QString("Unknown");
    
    {
        QMutexLocker locker(&m_resultsMutex);
        // Prevent unbounded growth - if buffer would be too large, drop oldest results
        int newSize = m_results.size() + seeds.size();
        if (newSize > MAX_RESULTS_BUFFER)
        {
            // Remove enough oldest results to fit new ones
            int removeCount = newSize - MAX_RESULTS_BUFFER + (MAX_RESULTS_BUFFER / 10);
            if (removeCount > 0)
            {
                m_results.remove(0, removeCount);
                qWarning() << "SearchCoordinator: Results buffer would overflow, dropped" << removeCount << "oldest results";
            }
        }
        m_results.append(seeds);
    }
    for (uint64_t seed : seeds)
    {
        emit searchResult(seed, hostname);
    }
    
    // Continue distributing tasks
    distributeTasks();
}

void SearchCoordinator::onWorkerProgress(uint64_t prog, uint64_t seed)
{
    QMutexLocker locker(&m_mutex);
    m_prog += prog;
    m_seed = seed;
    // Note: m_prog tracks cumulative progress from all workers
    // The GUI will aggregate this with local worker progress
}

void SearchCoordinator::onClientDisconnected()
{
    SearchCoordinatorClient* client = qobject_cast<SearchCoordinatorClient*>(sender());
    if (!client)
        return;
    
    QTcpSocket *socket = client->socket();
    QString hostname;
    if (socket)
    {
        hostname = socket->peerAddress().toString();
    }
    quint16 port = m_workerPort;
    
    // Try to find hostname from our list if peer address doesn't work
    if (hostname.isEmpty() || hostname == "::ffff:127.0.0.1" || hostname == "127.0.0.1")
    {
        QMutexLocker locker(&m_mutex);
        // Try to match by finding a worker host that might match
        // This is a fallback - ideally we'd track this better
        if (!m_workerHosts.isEmpty())
        {
            // Use first available hostname as fallback
            for (const QString& h : m_workerHosts)
            {
                bool found = false;
                for (SearchCoordinatorClient* c : m_clients)
                {
                    if (c != client && c->socket() && 
                        (c->socket()->peerAddress().toString().contains(h) || 
                         h.contains(c->socket()->peerAddress().toString())))
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    hostname = h;
                    break;
                }
            }
        }
    }
    
    {
        QMutexLocker locker(&m_mutex);
        m_clients.removeAll(client);
    }
    client->deleteLater();
    
    emit clientDisconnected();
    qDebug() << "Worker disconnected from" << (hostname.isEmpty() ? "unknown" : hostname);
    
    // Try to reconnect if we have a hostname and workers are configured
    if (!hostname.isEmpty() && !m_workerHosts.isEmpty())
    {
        qDebug() << "Attempting to reconnect to worker at" << hostname << ":" << port;
        QTimer::singleShot(2000, this, [this, hostname, port]() {
            connectToWorker(hostname, port);
        });
    }
    
    // Check if search is done
    QMutexLocker locker(&m_mutex);
    bool allIdle = true;
    for (SearchCoordinatorClient* c : m_clients)
    {
        if (c->hasPendingTask())
        {
            allIdle = false;
            break;
        }
    }
    
    if (allIdle && m_isdone)
    {
        m_searching = false;
        emit searchFinish(true);
    }
}

