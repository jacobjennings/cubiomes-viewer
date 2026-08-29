#ifndef SEARCHTHREAD_H
#define SEARCHTHREAD_H

#include "search.h"
#include "config.h"

#include <QThread>
#include <QMutex>
#include <QVector>
#include <QElapsedTimer>
#include <QTimer>
#include <QMessageBox>

#include <deque>

class SearchCoordinator;
class SearchWorkerClient;

struct Session
{
    void writeHeader(QTextStream& stream);
    bool save(QWidget *widget, QTextStream& stream);
    bool load(QWidget *widget, QTextStream& stream, bool quiet);

    WorldInfo wi;
    SearchConfig sc;
    Gen48Config gen48;
    std::vector<Condition> cv;
    std::vector<uint64_t> slist;
    int centerOnBiomesConditionSave = 0;
    QString biomeStatistics;
};

struct SearchWorker;

struct SearchMaster : QObject
{
    Q_OBJECT
public:
    SearchMaster(QWidget *parent);
    virtual ~SearchMaster();

    bool set(QWidget *widget, const Session& s);
    
    void setWorkers(const QStringList& workerHosts, quint16 port = 23473);
    void clearWorkers();

    void preSearch();

    void startSearch();
    void stopSearch();

    // Get search progress:
    //  status  : progress status summary
    //  prog    : scheduled progress in search space
    //  end     : size of search space
    //  seed    : current seed to be processed
    // Get the search speed (provided it is called at regular intervals):
    //  min,max : lower and upper search speed quartiles
    //  avg     : search speed average
    bool getProgress(QString *status, uint64_t *prog, uint64_t *end, uint64_t *seed, qreal *min, qreal *avg, qreal *max);

    // Get aggregated profiling data from local workers
    void getProfilingData(std::vector<Condition>& conditions);
    void captureConditionStats();

    bool requestItem(SearchWorker *item);

public slots:
    void onWorkerResult(uint64_t seed);
    void onWorkerResult(uint64_t seed, const QString& source);
    void onWorkerFinished();

signals:
    void searchResult(uint64_t seed, const QString& source);
    void searchFinish(bool done);

public:
    struct TProg { uint64_t ns, prog; };

public:
    std::vector<SearchWorker*>  workers;

    QMutex                      mutex;
    std::atomic_bool            stop;

    std::deque<TProg>           proghist;
    QElapsedTimer               progtimer;
    QElapsedTimer               itemtimer;
    uint64_t                    count;

    SearchThreadEnv             env;
    std::map<int, std::pair<uint64_t, uint64_t>> finished_condition_stats;

    int                         searchtype;
    int                         mc;
    int                         large;
    ConditionTree               condtree;
    int                         itemsize;   // number of seeds per search item
    int                         threadcnt;  // numbr of worker threads
    Gen48Config                 gen48;      // 48-bit generator settings
    std::vector<uint64_t>       slist;      // candidate list
    uint64_t                    idx;        // index within candidate list
    uint64_t                    scnt;       // search space size
    uint64_t                    prog;       // search space progress tracker
    uint64_t                    seed;       // current seed (next to be processed)
    uint64_t                    smin;
    uint64_t                    smax;
    bool                        isdone;
    
    // Distributed search support
    SearchCoordinator          *coordinator;
    QStringList                workerHosts;
    quint16                    workerPort;
    SearchWorkerClient        *localWorkerClient;
};


struct SearchWorker : QThread
{
    Q_OBJECT
public:
    SearchWorker(SearchMaster *master);
    ~SearchWorker();

    bool getNextItem();
    virtual void run() override;

signals:
    void result(uint64_t seed);

public:
    SearchMaster      * master;

    const uint64_t    * slist;      // candidate list
    uint64_t            len;        // number of candidates

    /// current work item
    uint64_t            prog;       // search space progress
    uint64_t            idx;        // current index in candidate buffer
    uint64_t            sstart;     // starting seed
    int                 scnt;       // number of seeds to process in this item
    uint64_t            seed;       // (out) current seed while processing
    // the end seed is the highest unsigned seed value in the search space
    // (or the last entry in the seed list)

    SearchThreadEnv     env;        // public for profiling data access
};





#endif // SEARCHTHREAD_H
