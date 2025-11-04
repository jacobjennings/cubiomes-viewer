# Memory Usage Analysis: Worker Client vs Standard GUI Search

## Primary Source of Memory Usage

The **primary source of unbounded memory growth** is **Qt's event queue** when using `Qt::QueuedConnection` for signals.

### How Memory Grows

1. **Signal Queue Accumulation**:
   - Each `taskResult(uint64_t)` signal creates a queued event in Qt's event loop
   - Each `taskResults(QVector<uint64_t>)` signal creates a queued event with the entire vector copied
   - Each `taskProgress(uint64_t, uint64_t)` signal creates a queued event
   - Each `taskFinished()` signal creates a queued event

2. **With 15 threads**, if threads find many matches:
   - 15 threads × many matches/second = hundreds to thousands of signals/second
   - If the main event loop can't process signals fast enough → queue grows unbounded
   - Each queued signal holds a copy of its parameters in memory

## Memory Estimation

### Per WorkerThread Memory:
```
SearchThreadEnv:        ~5-50 KB  (Generator ~few KB, SurfaceNoise ~few KB, Lua states variable)
ConditionTree:          ~1-10 KB  (depends on number of conditions)
Session copy:           Variable  (depends on cv.size() and slist.size())
  - cv vector:          ~100-500 bytes per condition
  - slist vector:       ~8 bytes × slist.size() (could be millions!)
Total per thread:       ~10 KB + 8×slist.size() bytes
```

### Batch Buffer:
```
Results batch buffer:   100 × 8 bytes = 800 bytes
Progress accumulator:   8 bytes
```

### Qt Event Queue (THE PROBLEM):
```
Per queued signal:      ~50-200 bytes overhead + parameter size
- taskResult:           ~50 bytes + 8 bytes (uint64_t) = ~58 bytes
- taskResults(N):       ~50 bytes + 8×N bytes
- taskProgress:         ~50 bytes + 16 bytes = ~66 bytes
- taskFinished:         ~50 bytes

If 15 threads emit 1000 signals/second that aren't processed:
  1000 signals/sec × 100 bytes/signal = 100 KB/sec
  Over 5 minutes: 100 KB/sec × 300 sec = 30 MB (unbounded growth)
```

### Worst Case Scenario:
With current batching (100 results per batch):
- If each thread finds 10 matches per task
- 15 threads × 10 matches = 150 matches, but batched into ~2 `taskResults` signals
- Still: 15 threads × ~4 signals per task (result + progress + finished) = 60 signals per task cycle
- If tasks complete every 100ms: 600 signals/second
- If event loop is slow: Queue grows at 60 KB/sec = 3.6 MB/min = **216 MB/hour** (still unbounded!)

## Comparison: Worker vs Standard GUI Search

### Standard GUI SearchWorker:
```cpp
// searchthread.cpp:654
Qt::BlockingQueuedConnection  // Blocks worker thread until signal processed
```
- **Memory efficient**: Signals processed immediately, no queue buildup
- **Blocks threads**: Worker threads wait for GUI thread to process
- **No copying**: Direct connection prevents parameter copying

### WorkerThread (Current):
```cpp
// searchworkerclient.cpp:454
Qt::QueuedConnection  // Queues signals without blocking
```
- **Memory inefficient**: Signals queue up, can grow unbounded
- **Non-blocking**: Worker threads continue immediately
- **Copies parameters**: Each signal creates a copy in the queue

## Is 32GB Expected?

**NO, 32GB is NOT expected for normal operation.**

Expected memory:
- 15 threads × ~1 MB per thread (with typical slist) = ~15 MB
- Batch buffers = ~1 KB
- **Normal event queue = < 100 KB**
- **Total expected: < 50 MB**

If memory grows to 32GB, it means:
1. Qt event queue has millions of queued signals
2. Event loop is processing signals slower than they're emitted
3. Possible deadlock or blocking in event loop processing

## Recommendations

### 1. Reduce Batch Sizes (IMMEDIATE):
```cpp
static const int RESULTS_BATCH_SIZE = 10;  // Reduce from 100 to 10
static const qint64 RESULTS_BATCH_MS = 25;  // Reduce from 50ms to 25ms
```
This sends results more frequently, preventing large batches from queuing.

### 2. Use BlockingQueuedConnection (LIKE STANDARD SEARCH):
Change to `Qt::BlockingQueuedConnection` to match standard search behavior:
- Prevents queue buildup
- Matches memory model of standard search
- Worker threads will wait, but memory stays bounded

### 3. Direct Thread Communication:
Instead of signals, use direct method calls with mutex protection:
- Eliminates Qt event queue entirely
- More memory efficient
- Requires careful thread synchronization

### 4. Limit Event Queue Size:
Qt doesn't provide this directly, but we could:
- Track pending signals count
- Block worker threads when queue too large
- Use semaphore or condition variable

## Current Batch Sizes Analysis

Current settings:
- `RESULTS_BATCH_SIZE = 100`
- `RESULTS_BATCH_MS = 50ms`

With 15 threads finding matches:
- Worst case: 15 threads × 100 matches/task = 1500 matches
- Batched: ~15 `taskResults` signals (100 each)
- Still allows queue buildup if event loop slow

**Recommendation**: Reduce to `RESULTS_BATCH_SIZE = 10` to send results 10× more frequently.

