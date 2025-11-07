#ifndef SEARCHWORKERCLIENT_H
#define SEARCHWORKERCLIENT_H

#include "searchprotocol.h"
#include "searchthread.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QObject>
#include <QTimer>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <queue>
#include <vector>
#include <atomic>
#include <memory>
#include <QAbstractSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QElapsedTimer>
#include <QQueue>
#include <QMutex>
#include <QWaitCondition>
#include <atomic>
#include <memory>
#include <vector>

// Forward declaration
class SearchWorkerClient;
struct LocalGen48Cursor;

// Shared cache for pre-computed 48-bit candidates
// This allows all threads to access the list without re-computing it
typedef std::shared_ptr<QVector<uint64_t>> LocalGen48Cache;

// Task structure for worker threads
struct TaskItem {
    uint64_t sstart;
    uint64_t scnt;
    uint64_t idx;
};

// Worker thread class similar to SearchWorker
class WorkerThread : public QThread
{
    Q_OBJECT
public:
    WorkerThread(SearchWorkerClient *client, const Session& session, int mc, bool large,
                           int searchtype, const std::vector<uint64_t>& slist,
                           std::atomic_bool *stop, QObject *parent,
                           LocalGen48Cache localGenCache);
    ~WorkerThread();
    
    void stop();
    
signals:
    void taskResult(uint64_t seed);
    void taskResults(const QVector<uint64_t>& seeds);
    void taskProgress(uint64_t prog, uint64_t seed);
    void taskFinished();

protected:
    void run() override;

private:
    bool generateLocal48Slice(uint64_t startIdx, uint64_t count, QVector<uint64_t>& out);
    uint64_t computeLocalGenCount();

    SearchWorkerClient *m_client;
    std::atomic_bool m_stop;
    SearchThreadEnv m_env;
    
    // Thread-local copies of search configuration
    Session m_session;
    int m_mc;
    bool m_large;
    int m_searchtype;
    std::vector<uint64_t> m_slist;  // Copy of seed list to avoid pointer invalidation
    std::atomic_bool *m_sharedStop;
    
    // For local 48-bit candidate generation (used when coordinator does not provide a list)
    std::unique_ptr<LocalGen48Cursor> m_localGenCursor;
    LocalGen48Cache m_localGenCache;
    uint64_t m_localGenCount = 0;
};

class SearchWorkerClient : public QObject
{
    Q_OBJECT
public:
    explicit SearchWorkerClient(quint16 port, int threadCount = -1, QObject *parent = nullptr);
    ~SearchWorkerClient();

    bool start();
    void stop();
    bool isListening() const { return m_server && m_server->isListening(); }
    bool isConnected() const { return m_socket && m_socket->state() == QAbstractSocket::ConnectedState; }
    quint16 serverPort() const { return m_server ? m_server->serverPort() : 0; }

signals:
    void connected();
    void disconnected();
    void clientError(const QString& msg);
    void finished();

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();
    void onError(QAbstractSocket::SocketError error);
    void requestTask();
    void sendHeartbeat();
    void logSearchRate();
    void checkTaskQueue(); // Check if we need more tasks

private:
    void processMessage(const QByteArray& msg);
    bool readMessage(QByteArray& msg);
    void sendMessage(const QByteArray& msg);
    
    void queueTask(uint64_t sstart, uint64_t scnt, uint64_t idx);
    void sendResult(uint64_t seed);
    void sendResults(const QVector<uint64_t>& seeds);
    
    // Thread management
    void startWorkerThreads();
    void stopWorkerThreads();
    bool getNextTask(TaskItem& task);
    
    friend class WorkerThread;
    
    quint16 m_port;
    QTcpServer *m_server;
    QTcpSocket *m_socket;
    QByteArray m_readBuffer;
    bool m_configReceived;
    
    // Search state
    Session m_session;
    ConditionTree m_condtree;
    int m_searchtype;
    int m_mc;
    int m_large;
    std::vector<uint64_t> m_slist;
    bool m_localGen48; // instruct worker to generate 48-bit candidates locally
    bool m_localGenSlicing; // enable flat index slicing semantics for local 48-bit gen
    uint64_t m_localGenCount = 0; // number of low candidates per high (provided by coordinator)
    LocalGen48Cache m_localGenCache;
    std::atomic_bool m_stop;
    
    // Threading
    std::vector<WorkerThread*> m_workers;
    QMutex m_taskMutex;
    QWaitCondition m_taskCondition;
    std::queue<TaskItem> m_taskQueue;
    int m_threadCount;
    bool m_threadCountSet; // True if thread count was explicitly set from command line
    
    QTimer *m_taskTimer;
    QTimer *m_heartbeatTimer;
    QTimer *m_rateLogTimer;
    QTimer *m_resultsFlushTimer;
    
    // Progress tracking for logging (use atomic to avoid race conditions)
    QElapsedTimer m_logTimer;
    std::atomic<uint64_t> m_logTotalSeeds; // Atomic to prevent race conditions from multiple threads
    uint64_t m_logLastSeeds; // Seeds count at last log
    bool m_logFirstInterval;
    
    // Progress throttling to prevent signal queue buildup
    QElapsedTimer m_progressThrottleTimer;
    QMutex m_progressThrottleMutex;
    uint64_t m_progressThrottleAccumulator; // Accumulated progress since last send
    
    // Task counter for logging every Nth task
    std::atomic<uint64_t> m_taskCounter;
    static const int TASK_LOG_INTERVAL = 100; // Log every 100 tasks
    static const qint64 PROGRESS_THROTTLE_MS = 200; // Send progress at most every 200ms (reduced frequency)
    
    // Results batching to prevent signal queue buildup
    QMutex m_resultsBatchMutex;
    QVector<uint64_t> m_resultsBatch; // Accumulated results waiting to be sent
    QElapsedTimer m_resultsBatchTimer;
    static const qint64 RESULTS_BATCH_MS = 50; // Send results at least every 50ms (increased to reduce overhead)
    static const int RESULTS_BATCH_SIZE = 50; // Or when batch reaches this size (increased to reduce network overhead)
    
    // Task queue management
    QMutex m_taskRequestMutex;
    bool m_taskRequestPending; // Track if we've already requested tasks
    int m_inflightRequests; // Number of outstanding MSG_TASK_REQUEST not yet assigned
    static const int TASK_QUEUE_LOW_THRESHOLD = 2; // Request more tasks when queue has <= this many tasks
};

#endif // SEARCHWORKERCLIENT_H

