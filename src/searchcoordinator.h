#ifndef SEARCHCOORDINATOR_H
#define SEARCHCOORDINATOR_H

#include "searchprotocol.h"
#include "searchthread.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QHash>
#include <QMutex>
#include <QTimer>
#include <QElapsedTimer>
#include <QStringList>
#include <deque>

class SearchCoordinatorClient : public QObject
{
    Q_OBJECT
public:
    SearchCoordinatorClient(QTcpSocket *socket, QObject *parent);
    ~SearchCoordinatorClient();

    QTcpSocket* socket() const { return m_socket; }
    bool isReady() const { return m_ready; }
    bool hasPendingTask() const { return m_pendingTask; }

    void sendTask(uint64_t sstart, uint64_t scnt, uint64_t idx);
    void sendStop();
    void sendConfig(const Session& session);
    
    QString getHostname() const;
    double getSeedsPerSecond() const; // Returns current seeds/sec rate

signals:
    void taskRequested();
    void result(uint64_t seed);
    void results(const QVector<uint64_t>& seeds);
    void progress(uint64_t prog, uint64_t seed);
    void disconnected();
    void workerError(const QString& msg);
    void ready();

private slots:
    void onReadyRead();
    void onDisconnected();
    void onError(QAbstractSocket::SocketError error);

private:
    void processMessage(const QByteArray& msg);
    bool readMessage(QByteArray& msg);
    
    QTcpSocket *m_socket;
    QByteArray m_readBuffer;
    bool m_ready;
    bool m_pendingTask;
    uint64_t m_currentProg;
    uint64_t m_currentSeed;
    
    // Progress tracking for seeds/sec calculation
    struct ProgressPoint {
        qint64 timestamp;  // milliseconds since start
        uint64_t totalSeeds; // cumulative total seeds processed
    };
    std::deque<ProgressPoint> m_progressHistory;
    QElapsedTimer m_progressTimer;
    uint64_t m_totalSeedsProcessed; // Cumulative seeds processed
};


class SearchCoordinator : public QObject
{
    Q_OBJECT
public:
    explicit SearchCoordinator(QObject *parent = nullptr);
    ~SearchCoordinator();

    bool connectToWorkers(const QStringList& workerHosts, quint16 port);
    void disconnectFromWorkers();
    bool isConnected() const;

    void setSession(const Session& session);
    void startSearch();
    void stopSearch();

    // Task distribution (now delegates to SearchMaster instead of managing own state)
    bool getNextTask(uint64_t *sstart, uint64_t *scnt, uint64_t *idx);
    
    // Set reference to SearchMaster for coordinated task distribution
    void setSearchMaster(class SearchMaster* master);

    // Results and progress
    QVector<uint64_t> getResults();
    bool getProgress(uint64_t *prog, uint64_t *end, uint64_t *seed);
    
    // Per-worker statistics
    struct WorkerStats {
        QString hostname;
        double seedsPerSecond;
        bool isActive;
    };
    QVector<WorkerStats> getWorkerStats() const;

signals:
    void searchResult(uint64_t seed, const QString& source);
    void searchFinish(bool done);
    void clientConnected();
    void clientDisconnected();

private slots:
    void onConnected();
    void onConnectionFailed();
    void onTaskRequested();
    void onWorkerResult(uint64_t seed);
    void onWorkerResults(const QVector<uint64_t>& seeds);
    void onWorkerProgress(uint64_t prog, uint64_t seed);
    void onClientDisconnected();

private:
    void distributeTasks();
    void distributeTasksLocked(); // Assumes m_mutex is locked
    void connectToWorker(const QString& hostname, quint16 port);
    bool getNextTaskLocked(uint64_t *sstart, uint64_t *scnt, uint64_t *idx); // Assumes m_mutex is locked
    
    QStringList m_workerHosts;
    quint16 m_workerPort;
    QList<SearchCoordinatorClient*> m_clients;
    mutable QMutex m_mutex;
    
    // Reference to SearchMaster for coordinated task distribution
    class SearchMaster* m_searchMaster;
    
    // Search state (copied from SearchMaster)
    Session m_session;
    bool m_searching;
    std::atomic_bool m_stop;
    
    int m_searchtype;
    int m_mc;
    int m_large;
    std::vector<uint64_t> m_slist;
    uint64_t m_idx;
    uint64_t m_scnt;
    uint64_t m_prog;
    uint64_t m_seed;
    uint64_t m_smin;
    uint64_t m_smax;
    bool m_isdone;
    int m_itemsize;
    
    // Special slicing for local 48-bit generation (QH/QM) to avoid duplication
    bool m_localGenSlicingActive = false;
    uint64_t m_flatIdx = 0; // next flat index to assign across (high, low) space
    
    QVector<uint64_t> m_results;
    QMutex m_resultsMutex;
    static const int MAX_RESULTS_BUFFER = 1000000; // Maximum results to buffer (prevent unbounded growth)
    
    QElapsedTimer m_itemtimer;
    uint64_t m_count;
};

#endif // SEARCHCOORDINATOR_H

