FlexiCAS: A ***Flexi***ble ***C***ache ***A***rchitectural ***S***imulator
-------------------------------

GPL licensed.

Copyright (c) 2023-2023 Wei Song <[wsong83@gmail.com](mailto:wsong83@gmail.com)> at the Institute of
Information Engineering, Chinese Academy of Sciences.

#### Authors:
* [Wei Song](mailto:wsong83@gmail.com) (SKLOIS, Institute of Information Engineering, Chinese Academy of Sciences)
* [Jinchi Han](mailto:hanjinchi@iie.ac.cn) (SKLOIS, Institute of Information Engineering, Chinese Academy of Sciences)

## Features

* A pure C++ (std c++17) implementation of a modular cache architecture.
* Modular support for different index function, replacement policy, and slice mapping function.
* Modular support for complex cache array structure, such as separated metadata and data arrays.
* Modular support for different coherence protocols (MI/MSI/MESI), inclusing broadcast and directory.
* Modular support for inclusive/exclusive cache hierarchy.
* On-demand hooking up with user defined performance monitors.
* On-demand estimation of cache access delay (behavoral, not cycle accurate).
* **L2 Cache Reuse Count Monitoring**: Track cache line reuse patterns to understand temporal locality after L1 filtering.

### Note

The project is under active developing. Please raise **issues** or **pull requests** for any sugguestion or fix.

This is an overhual of a previous in-house implemented simulator, namely [**cache-model**](https://github.com/comparch-security/cache-model).

## Usage

Right now, see the regression test cases in `regression/*.cpp` and try to run `make regression`.

## L2 Cache Reuse Count Monitoring

FlexiCAS now includes comprehensive reuse count tracking for L2 cache analysis. This feature helps understand temporal locality in the filtered reference stream that reaches L2 (after L1 filtering).

### What is Reuse Count?

**Reuse count** is the number of L2 accesses to a cache line **after** its initial fill. When an L2 cache line is installed, its reuse count starts at 0. Each subsequent hit to that line increments the count. Upon eviction, the reuse count is recorded in a global distribution histogram.

This metric is crucial because:
- L2 sees only L1 misses (filtered reference stream)
- L1 filtering typically reduces locality seen by L2
- Understanding L2 reuse patterns helps optimize cache hierarchy

### Key Files

- **`util/reuse_count_monitor.hpp`**: Main implementation of the ReuseCountMonitor class
- **`util/REUSE_COUNT_README.md`**: Detailed documentation and interpretation guide
- **`util/reuse_count_example.cpp`**: Example code demonstrating usage
- **`spike-cache_exploration.cc`**: Integration with cache simulation

### Output Files

When running simulations, the following files are generated:

1. **`l2_reuse_distribution.csv`**: Complete reuse count histogram
   - Columns: ReuseCount, Frequency, Percentage, CumulativePercentage
   
2. **`l2_reuse_eviction_history.csv`**: Detailed per-eviction records (last 10k evictions)
   - Columns: Address, Set, Way, ReuseCount, Timestamp
   
3. **`l2_reuse_summary.txt`**: Text summary with key statistics

### Quick Start

The reuse monitor is automatically enabled in `spike-cache_exploration.cc`. After running your simulation:

```bash
# View the reuse distribution
$ cat l2_reuse_distribution.csv

# Analyze patterns
$ head -20 l2_reuse_distribution.csv
ReuseCount,Frequency,Percentage,CumulativePercentage
0,15234,45.67,45.67      # 45% of lines evicted without reuse
1,8921,26.73,72.40       # 27% used once after fill
2,4523,13.56,85.96       # 14% used twice
...

# Plot with gnuplot, R, or Python
$ python3 plot_reuse_dist.py l2_reuse_distribution.csv
```

### Interpreting Results

**High zero-reuse rate (>50%)**:
- Poor L2 temporal locality
- Streaming behavior or working set > L2 capacity
- Consider cache bypassing or larger L2

**High average reuse (>5)**:
- Good temporal locality in L2
- Working set fits well
- Effective cache hierarchy

**Bimodal distribution**:
- Mix of hot and cold lines
- Potential for intelligent insertion policies
- Consider cache partitioning

See `util/REUSE_COUNT_README.md` for detailed interpretation guidelines and example scenarios.

### Implementation Details

- **Zero overhead on cache operation**: Monitoring uses separate thread
- **Configurable history size**: Default 10,000 eviction records
- **Per-line tracking**: O(sets × ways) memory usage
- **Cycle-accurate**: No sampling, captures all evictions

### Example Statistics Output

```
=== L2 Cache Reuse Count Analysis ===

Total L2 Accesses:    1,234,567
Total Cache Fills:    98,765 (misses)
Total Reuses:         456,789 (hits after fill)
Total Evictions:      87,654
Average Reuse Count:  5.21
Max Reuse Count:      234
Zero Reuse Evictions: 23,456 (26.76%)

--- Reuse Distribution (Histogram) ---
Reuse Count | Frequency | Percentage | Cumulative %
------------|-----------|------------|-------------
          0 |    23456  |     26.76% |      26.76%
          1 |    18234  |     20.80% |      47.56%
          2 |    12456  |     14.21% |      61.77%
...
```

For more information, see the comprehensive documentation in `util/REUSE_COUNT_README.md`.
