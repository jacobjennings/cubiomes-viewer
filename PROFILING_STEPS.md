# Memory Profiling Steps

## Quick Debugging Commands

### 1. Monitor Memory Growth in Real-Time
```bash
# Watch RSS (Resident Set Size) memory every second
watch -n 1 'ps -o pid,rss,vsz,comm -p $(pgrep -f cubiomes-viewer)'

# Or use htop
htop -p $(pgrep -f cubiomes-viewer)
```

### 2. Run with Debug Output
The code now includes debug warnings that will show:
- Large slist sizes per thread
- Large read buffer sizes
- Large socket write buffer sizes
- Large batch sizes

Run the worker and watch for these warnings:
```bash
./build/cubiomes-viewer --worker-mode 23473 --threads 15 2>&1 | tee worker_debug.log
```

Look for warnings starting with "WARNING:"

### 3. Use Valgrind Massif (Heap Profiler)
```bash
# Build with debug symbols
cd build
make clean
cd ..
make -C build CFLAGS="-g -O0" CXXFLAGS="-g -O0"

# Run with massif
valgrind --tool=massif \
  --massif-out-file=massif_worker.out \
  --time-unit=ms \
  ./build/cubiomes-viewer --worker-mode 23473 --threads 15

# Generate report
ms_print massif_worker.out > massif_report.txt
cat massif_report.txt | head -100  # First 100 lines
```

### 4. Use Valgrind Memcheck (Leak Detection)
```bash
valgrind --tool=memcheck \
  --leak-check=full \
  --show-leak-kinds=all \
  --track-origins=yes \
  --log-file=valgrind.log \
  ./build/cubiomes-viewer --worker-mode 23473 --threads 15

# Check for leaks
grep "definitely lost" valgrind.log
grep "indirectly lost" valgrind.log
```

### 5. Use `pmap` to See Memory Mapping
```bash
# Get PID
PID=$(pgrep -f cubiomes-viewer)

# Show memory map (repeat while memory grows)
pmap -x $PID | tail -20

# Show detailed memory breakdown
cat /proc/$PID/smaps | grep -E "^Size|^Rss" | awk '{sum+=$2} END {print sum/1024 " MB"}'
```

### 6. Monitor Qt Event Queue
Add this temporary code to track event queue depth (requires Qt source or custom build):
```cpp
// In searchworkerclient.cpp, add periodic check
QTimer::singleShot(5000, this, [this]() {
    qDebug() << "DEBUG: Pending events:" << QCoreApplication::instance()->property("pendingEvents").toInt();
});
```

### 7. Check for Deadlocks
```bash
# Use gdb to attach and check thread states
gdb -p $(pgrep -f cubiomes-viewer)
(gdb) info threads
(gdb) thread apply all bt
(gdb) detach
```

## What to Look For

### Red Flags:
1. **Slist size warnings** - If slist has millions of entries, each thread copies it
   - Solution: Don't copy slist, use shared pointer or reference
   
2. **Read buffer warnings** - Network messages accumulating
   - Solution: Process messages faster or limit message size
   
3. **Socket buffer warnings** - Network send buffer filling up
   - Solution: Coordinator not reading fast enough, or network issue
   
4. **Batch size growing** - Results accumulating faster than sent
   - Solution: Reduce batch size or increase send frequency

### Expected Memory Breakdown:
- 15 threads × ~1-10 MB each (depends on slist size) = 15-150 MB
- Batch buffers = < 1 MB
- Network buffers = < 10 MB
- Qt event queue = < 10 MB (should be near-zero with BlockingQueuedConnection)

**Total expected: < 200 MB for normal operation**

## Next Steps Based on Findings

### If slist is huge:
- Change WorkerThread to use reference/pointer instead of copy
- Or use shared_ptr<std::vector<uint64_t>>

### If read buffer grows:
- Check if processMessage is slow
- Check for blocking operations in message processing

### If socket buffer grows:
- Coordinator may not be reading
- Network may be slow
- Add flow control

### If batch grows unbounded:
- BlockingQueuedConnection may not be working
- Check for deadlock preventing signal processing
- Consider using direct mutex-protected calls instead of signals

