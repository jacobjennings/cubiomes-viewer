# Memory Debugging Guide

## Quick Memory Profiling Tools

### 1. Monitor Memory in Real-Time
```bash
# Watch memory usage continuously
watch -n 1 'ps aux | grep cubiomes-viewer | grep -v grep'

# Or use htop for detailed view
htop -p $(pgrep -f cubiomes-viewer)
```

### 2. Use Valgrind (Detailed Memory Leak Detection)
```bash
# Build with debug symbols
make -C build clean
make -C build CFLAGS="-g -O0" CXXFLAGS="-g -O0"

# Run with valgrind massif (heap profiler)
valgrind --tool=massif --massif-out-file=massif.out ./build/cubiomes-viewer --worker 23473 --threads 15

# Analyze results
ms_print massif.out > massif_analysis.txt
```

### 3. Use Valgrind Memcheck (Leak Detection)
```bash
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
  --track-origins=yes --log-file=valgrind.log \
  ./build/cubiomes-viewer --worker 23473 --threads 15
```

### 4. Use `perf` for Memory Allocation Tracking
```bash
# Record memory allocations
sudo perf record -e page-faults -g ./build/cubiomes-viewer --worker 23473 --threads 15

# View report
sudo perf report
```

### 5. Add Memory Tracking in Code
Add these debug hooks to track allocations:
- Track QVector/QByteArray sizes
- Track Qt signal queue depth
- Track batch buffer sizes
- Monitor slist sizes per thread

