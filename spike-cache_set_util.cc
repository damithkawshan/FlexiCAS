#include "cache/memory.hpp"
#include "util/cache_type.hpp"
#include "cache/slicehash.hpp"
#include "util/query.hpp"
#include "flexicas-pfc.h"
#include "cache/mesi.hpp"
#include "util/set_utilization_monitor.hpp"
#include "util/reuse_count_monitor.hpp"
#include "cache/sbc.hpp"
#include "cache/coherence.hpp"

#include <list>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <string>
#include "flexicas/cache_config.h"

// // 32K 8W, both I and D
// #define L1IW 6
// #define L1WN 8

// // 256K, 4W, exclusive 
// #define L2IW 10
// #define L2WN 4


// Embedded microprocessor configuration (similar to ARM Cortex-M7/SiFive E76)
// Small caches optimized for embedded systems with MESI inclusive hierarchy

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64  // 64 bytes cache line
#endif

// 8KB L1, 4-way set associative (typical for embedded processors like Cortex-M7)
#ifndef L1IW
#define L1IW 8    // 2^8 = 256 sets, 256*64B*4 = 8KB
#endif
#ifndef L1WN
#define L1WN 4    // 4-way set associative
#endif

// 128KB L2, 8-way inclusive (typical for embedded SoCs)
#ifndef L2IW
#define L2IW 9   // 2^9 = 512 sets, 512*64B*4 = 128KB
#endif
#ifndef L2WN
#define L2WN 4   // 4-way set associative
#endif


// multithread support
#define ENABLE_FLEXICAS_THREAD

#define CACHE_OP_READ        0
#define CACHE_OP_WRITE       1
#define CACHE_OP_FLUSH       2
#define CACHE_OP_WRITEBACK   3
#define CACHE_OP_FLUSH_CACHE 4

#define XACT_QUEUE_HIGH    320
#define XACT_QUEUE_LOW     240
#define XACT_QUEUE_BURST   32

namespace {
  static std::vector<CoreInterfaceBase *> core_data, core_inst;
  static std::vector<uint64_t> core_cycle; // record the cycle time in each core
  static uint64_t wall_clock;              // a wall clock shared by all cores
  static MonitorBase *tracer;
  
  // Performance monitors for each cache level
  static SimpleAccMonitor *l1d_perf_monitor;
  static SimpleAccMonitor *l1i_perf_monitor;
  static SimpleAccMonitor *l2_perf_monitor;
  static SimpleAccMonitor *memory_perf_monitor;
  
  // Set utilization monitors
  static SetUtilizationMonitor *l1d_util_monitor;
  static SetUtilizationMonitor *l1i_util_monitor;
  static SetUtilizationMonitor *l2_util_monitor;

  // Reuse count monitor for L2 cache
  // static ReuseCountMonitor *l2_reuse_monitor;

  static int NC = 0;
  std::condition_variable xact_non_empty_notify, xact_non_full_notify;
  std::mutex xact_queue_op_mutex;
  std::mutex xact_queue_full_mutex;
  std::mutex xact_queue_empty_mutex;
  static bool exit_flag = false;
  static uint64_t pfc_value = 0;
  static std::atomic_bool cache_idle = false;

  struct cache_xact {
    char op_t;
    bool ic;
    int core;
    uint64_t addr;
  };

  std::vector<cache_xact> xact_input(XACT_QUEUE_BURST);
  std::deque<cache_xact>  xact_queue;
  std::vector<cache_xact> xact_output(XACT_QUEUE_BURST);
  int input_index = 0, output_index = 0;

  void xact_queue_flush() {
    std::unique_lock queue_full_lock(xact_queue_full_mutex, std::defer_lock);
    while(true) {
      xact_queue_op_mutex.lock();
      if(xact_queue.size() >= XACT_QUEUE_HIGH) {
        //std::cerr << "FlexiCAS lost sync with Spike with a transaction back log of " << xact_queue.size() << " transactions!" << std::endl;
        xact_queue_op_mutex.unlock();
        xact_non_full_notify.wait(queue_full_lock);
      } else {
        for(int i=0; i<XACT_QUEUE_BURST; i++) xact_queue.push_back(xact_input[i]);
        xact_non_empty_notify.notify_all();
        xact_queue_op_mutex.unlock();
        input_index = 0;
        break;
      }
    }
  }

  void xact_queue_add(cache_xact xact) {
    xact_input[input_index++] = xact;
    if(input_index == XACT_QUEUE_BURST) xact_queue_flush();
  }

  inline void read_detailed(uint64_t addr, int core, bool ic) {
    if(ic) core_inst[core]->read(addr, nullptr);
    else   core_data[core]->read(addr, nullptr);
  }

  inline void write_detailed(uint64_t addr, int core) {
    core_data[core]->write(addr, nullptr, nullptr);
  }

  inline void flush_detailed(uint64_t addr, int core) {
    core_data[core]->flush(addr, nullptr);
  }

  inline void writeback_detailed(uint64_t addr, int core) {
    core_data[core]->writeback(addr, nullptr);
  }

  inline void flush_icache_detailed(int core) {
    core_inst[core]->flush_cache(nullptr);
  }

  void cache_server() {
    std::unique_lock queue_empty_lock(xact_queue_empty_mutex, std::defer_lock);
    int burst_size = 0;
    while(!exit_flag) {
      // get the next xact
      xact_queue_op_mutex.lock();
      burst_size = xact_queue.size() > XACT_QUEUE_BURST ? XACT_QUEUE_BURST : xact_queue.size();
      for(int i=0; i<burst_size; i++) { xact_output[i] = xact_queue.front(); xact_queue.pop_front(); }
      if(xact_queue.size() <= XACT_QUEUE_LOW)
        xact_non_full_notify.notify_all();
      xact_queue_op_mutex.unlock();

      for(int i=0; i<burst_size; i++) {
        cache_xact &xact = xact_output[i];
        switch(xact.op_t) {
        case CACHE_OP_READ:        read_detailed(xact.addr, xact.core, xact.ic); break; // read
        case CACHE_OP_WRITE:       write_detailed(xact.addr, xact.core); break;         // write
        case CACHE_OP_FLUSH:       flush_detailed(xact.addr, xact.core); break;         // flush
        case CACHE_OP_WRITEBACK:   writeback_detailed(xact.addr, xact.core); break;     // writeback
        case CACHE_OP_FLUSH_CACHE: flush_icache_detailed(xact.core); break;             // flush the whole cache
        default: assert(0 == "unknown op type!");
        }
      }

      if(burst_size == 0) {
        using namespace std::chrono_literals;
        cache_idle = true;
        xact_non_empty_notify.wait_for(queue_empty_lock, 1ms);
        cache_idle = false;
      }
    }
  }

  void cache_sync() {
#ifdef ENABLE_FLEXICAS_THREAD
    using namespace std::chrono_literals;
    xact_queue_flush();
    while(!cache_idle) std::this_thread::sleep_for(1ms);
#endif
  }

  static std::list<LocInfo> query_locs;
  static CacheBase* coloc_query_cache;
  static uint64_t   coloc_query_target;

  CacheBase* query_cache(uint64_t paddr, int core) {
    query_locs.clear();
    core_data[core]->query_loc(paddr, &query_locs);
    return query_locs.back().cache;
  }

  static std::string pfc_str; // a string buffer used by PFC's CSR interface

  void print_cache_level_stats(const char* level_name, SimpleAccMonitor* monitor) {
    if (!monitor) return;

    auto total_access = monitor->get_access();
    auto total_miss = monitor->get_miss();
    auto total_read = monitor->get_access_read();
    auto total_write = monitor->get_access_write();
    auto read_miss = monitor->get_miss_read();
    auto write_miss = monitor->get_miss_write();
    auto invalidations = monitor->get_invalid();
    
    std::cout << "\n--- " << level_name << " Statistics ---" << std::endl;
    std::cout << "Total Accesses:      " << std::setw(10) << total_access << std::endl;
    std::cout << "Total Misses:        " << std::setw(10) << total_miss << std::endl;
    std::cout << "Total Hits:          " << std::setw(10) << (total_access - total_miss) << std::endl;
    std::cout << std::endl;
    
    std::cout << "Read Operations:     " << std::setw(10) << total_read << std::endl;
    std::cout << "Write Operations:    " << std::setw(10) << total_write << std::endl;
    std::cout << "Read Misses:         " << std::setw(10) << read_miss << std::endl;
    std::cout << "Write Misses:        " << std::setw(10) << write_miss << std::endl;
    std::cout << "Cache Invalidations: " << std::setw(10) << invalidations << std::endl;
    std::cout << std::endl;
    
    // Calculate hit/miss rates
    if (total_access > 0) {
      double hit_rate = 100.0 * (total_access - total_miss) / total_access;
      double miss_rate = 100.0 * total_miss / total_access;
      
      std::cout << "Overall Hit Rate:    " << std::setw(8) << hit_rate << "%" << std::endl;
      std::cout << "Overall Miss Rate:   " << std::setw(8) << miss_rate << "%" << std::endl;
      
      if (total_read > 0) {
        double read_hit_rate = 100.0 * (total_read - read_miss) / total_read;
        double read_miss_rate = 100.0 * read_miss / total_read;
        std::cout << "Read Hit Rate:       " << std::setw(8) << read_hit_rate << "%" << std::endl;
        std::cout << "Read Miss Rate:      " << std::setw(8) << read_miss_rate << "%" << std::endl;
      }
      
      if (total_write > 0) {
        double write_hit_rate = 100.0 * (total_write - write_miss) / total_write;
        double write_miss_rate = 100.0 * write_miss / total_write;
        std::cout << "Write Hit Rate:      " << std::setw(8) << write_hit_rate << "%" << std::endl;
        std::cout << "Write Miss Rate:     " << std::setw(8) << write_miss_rate << "%" << std::endl;
      }
    }
  }

  void print_cache_statistics(const std::string& context = "") {
    std::cout << "\n========================================" << std::endl;
    std::cout << "=== EMBEDDED CACHE PERFORMANCE (MESI Inclusive) ===" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // Print Cache Configuration
    std::cout << "\n--- Cache Configuration ---" << std::endl;
    std::cout << cache_type_suffix() << std::endl;
    std::cout << "Number of Cores:     " << NC << std::endl;
    std::cout << "L1 Data Cache:       " << (1 << L1IW) * CACHE_LINE_SIZE * L1WN / 1024 << "KB, " << (L1WN) << "-way set associative" << std::endl;
    std::cout << "\nL1 Instruction Cache " << (1 << L1IW) * CACHE_LINE_SIZE * L1WN / 1024 << "KB, " << (L1WN) << "-way set associative" << std::endl;
    std::cout << "\nL2 Cache:            " << (1 << L2IW) * CACHE_LINE_SIZE * L2WN / 1024 << "KB, " << (L2WN) << "-way set associative" << std::endl;
    
    std::cout << std::fixed << std::setprecision(2);
    
    // Print L1 Data Cache Statistics
    print_cache_level_stats("L1 Data Cache", l1d_perf_monitor);
    
    // Print L1 Instruction Cache Statistics
    print_cache_level_stats("L1 Instruction Cache", l1i_perf_monitor);
    
    // Print L2 Cache Statistics
    print_cache_level_stats("L2 Cache", l2_perf_monitor);

    // Memory Statistics
    if (memory_perf_monitor) {
      auto mem_access = memory_perf_monitor->get_access();
      auto mem_read = memory_perf_monitor->get_access_read();
      auto mem_write = memory_perf_monitor->get_access_write();
      
      std::cout << "\n--- Memory Statistics ---" << std::endl;
      std::cout << "Total Memory Accesses: " << std::setw(10) << mem_access << std::endl;
      std::cout << "Memory Reads:          " << std::setw(10) << mem_read << std::endl;
      std::cout << "Memory Writes:         " << std::setw(10) << mem_write << std::endl;
    }

    // Print L2 Reuse Count Analysis
    // if (l2_reuse_monitor) {
    //   l2_reuse_monitor->print_statistics("L2 Cache");

    //   // Export reuse distribution to CSV files
    //   l2_reuse_monitor->export_reuse_distribution_csv("l2_reuse_distribution.csv");
    //   l2_reuse_monitor->export_eviction_history_csv("l2_reuse_eviction_history.csv");
    //   l2_reuse_monitor->export_summary_txt("l2_reuse_summary.txt");
    // }

    std::cout << "\n========================================" << std::endl;
    
    // Print Set Utilization Statistics
    if (l1d_util_monitor) {
      l1d_util_monitor->print_statistics("L1 Data Cache");
    }
    
    if (l1i_util_monitor) {
      l1i_util_monitor->print_statistics("L1 Instruction Cache");
    }
    
    if (l2_util_monitor) {
      l2_util_monitor->print_statistics("L2 Cache");
    }
    
    // Export detailed set utilization to CSV files
    if (l1d_util_monitor || l1i_util_monitor || l2_util_monitor) {
      // Compute cache sizes in KB for filename suffixes
      uint32_t l1_size_kb = (1u << L1IW) * CACHE_LINE_SIZE * L1WN / 1024u;
      uint32_t l2_size_kb = (1u << L2IW) * CACHE_LINE_SIZE * L2WN / 1024u;

      if (l1d_util_monitor) {
        l1d_util_monitor->export_to_csv( context + "l1d_set_utilization_" + std::string(cache_type_suffix()) +"_"+ std::to_string(l1_size_kb) + "KB.csv");
        // l1d_util_monitor->export_eviction_history_to_csv("l1d_eviction_history_" + std::to_string(l1_size_kb) + "KB.csv");
      }
      if (l1i_util_monitor) {
        l1i_util_monitor->export_to_csv( context + "l1i_set_utilization_" + std::string(cache_type_suffix()) +"_"+ std::to_string(l1_size_kb) + "KB.csv");
        // l1i_util_monitor->export_eviction_history_to_csv("l1i_eviction_history_" + std::to_string(l1_size_kb) + "KB.csv");
      }
      if (l2_util_monitor) {
        l2_util_monitor->export_to_csv( context + "l2_set_utilization_" + std::string(cache_type_suffix()) +"_"+ std::to_string(l2_size_kb) + "KB.csv");
        // l2_util_monitor->export_eviction_history_to_csv("l2_eviction_history_" + std::to_string(l2_size_kb) + "KB.csv");
      }
    }
    
    std::cout << "\n========================================" << std::endl;
  }

}

namespace flexicas {
  typedef std::function<uint64_t(uint64_t)> tlb_translate_func;

  int  ncore() { return NC; }

  int  cache_level() {return 2; }

  int  cache_set(int level, bool ic) {
    switch(level) {
    case 1: return 1 << L1IW;
    case 2: return 1 << L2IW;
    default: return 0;
    }
  }

  int  cache_way(int level, bool ic) {
    switch(level) {
    case 1: return L1WN;
    case 2: return L2WN;
    default: return 0;
    }
  }

  void init(int ncore, const char *prefix) {
    std::cout << "================ 27/11/2025 6.14PM =============" << std::endl;
    std::cout << "Initializing FlexiCAS Embedded Cache Model with " << ncore << " cores..." << std::endl;
    std::cout << "\nL1 Data Cache:       " << (1 << L1IW) * CACHE_LINE_SIZE * L1WN / 1024 << "KB, " << (L1WN) << "-way set associative" << std::endl;
    std::cout << "L1 Instruction Cache: " << (1 << L1IW) * CACHE_LINE_SIZE * L1WN / 1024 << "KB, " << (L1WN) << "-way set associative" << std::endl;
    std::cout << "L2 Cache:            " << (1 << L2IW) * CACHE_LINE_SIZE * L2WN / 1024 << "KB, " << (L2WN) << "-way set associative" << std::endl;
    using policy_l2 = MESIPolicy<false, false, policy_memory>;
    using policy_l1d = MESIPolicy<false, false, policy_l2>;  // L1 is not topmost in inclusive hierarchy
    using policy_l1i = MESIPolicy<false, true, policy_l2>;   // L1I is instruction cache
    NC = ncore;
    core_cycle.resize(NC, 0);
    wall_clock = 0;
    auto l1d = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MESIPolicy, policy_l1d, false, void, true>(NC, "l1d");
    core_data = get_l1_core_interface(l1d);
    auto l1i = cache_gen_l1<L1IW, L1WN, void, MetadataBroadcastBase, ReplaceLRU, MESIPolicy, policy_l1i, true, void, true>(NC, "l1i");
    core_inst = get_l1_core_interface(l1i);

    #if CACHE_TYPE == CACHE_TYPE_BL
      auto l2 = cache_gen_inc<L2IW, L2WN, void, MetadataDirectoryBase, ReplaceLRU, MESIPolicy, policy_l2, false, void, true>(NC, "l2");
      std::cout << "Using Baseline (LRU) for L2 Cache" << std::endl;
    #elif CACHE_TYPE == CACHE_TYPE_DB
      auto l2 = cache_gen_dsbc<L2IW, L2WN, void, MetadataDirectoryBase, MESIPolicy, policy_l2, false, void, true>(NC, "l2-dsbc", true);
      std::cout << "Using Dynamic SBC for L2 Cache" << std::endl;
    #elif CACHE_TYPE == CACHE_TYPE_SB
      auto l2 = cache_gen_ssbc<L2IW, L2WN, void, MetadataDirectoryBase, MESIPolicy, policy_l2, false, void, true>(NC, "l2-ssbc", true);
      std::cout << "Using Static SBC for L2 Cache" << std::endl;
    #else
      #error "Unsupported CACHE_TYPE specified"
    #endif
    auto mem = new SimpleMemoryModel<void,void,true>("mem");
    tracer = new SimpleTracer(true);
    if(prefix) tracer->set_prefix(std::string(prefix));

    // Create performance monitors for each cache level
    l1d_perf_monitor = new SimpleAccMonitor;
    l1i_perf_monitor = new SimpleAccMonitor;
    l2_perf_monitor = new SimpleAccMonitor;
    memory_perf_monitor = new SimpleAccMonitor; 

    // Create reuse count monitor for L2 (track up to 10000 evictions in history)
    // l2_reuse_monitor = new ReuseCountMonitor(1 << L2IW, L2WN, 10000000);

    // Create set utilization monitors
    l1d_util_monitor = new SetUtilizationMonitor(1 << L1IW, L1WN);
    l1i_util_monitor = new SetUtilizationMonitor(1 << L1IW, L1WN);
    l2_util_monitor = new SetUtilizationMonitor(1 << L2IW, L2WN);

    for(int i=0; i<NC; i++) {
      l1i[i]->outer->connect(l2[i]->inner);
      l1d[i]->outer->connect(l2[i]->inner);
      l2[i]->outer->connect(mem);
      
      // Attach monitors to all cache levels
      l1i[i]->attach_monitor(tracer);
      l1d[i]->attach_monitor(tracer);
      l2[i]->attach_monitor(tracer);
      
      // Attach performance monitors
      l1i[i]->attach_monitor(l1i_perf_monitor);
      l1d[i]->attach_monitor(l1d_perf_monitor);
      l2[i]->attach_monitor(l2_perf_monitor);
      
      // Attach utilization monitors
      l1i[i]->attach_monitor(l1i_util_monitor);
      l1d[i]->attach_monitor(l1d_util_monitor);
      l2[i]->attach_monitor(l2_util_monitor);

      // Attach reuse count monitor to L2 cache
      // l2[i]->attach_monitor(l2_reuse_monitor);
    }

    mem->attach_monitor(memory_perf_monitor);
    mem->attach_monitor(tracer);

    //start tracer
    // tracer->start();

    std::cout << "FlexiCAS initialization completed." << std::endl;
    std::cout << "Starting performance and utilization monitors..." << std::endl;
    l1d_perf_monitor->reset();
    l1i_perf_monitor->reset();
    l2_perf_monitor->reset();
    memory_perf_monitor->reset();

    l1d_util_monitor->reset();
    l1i_util_monitor->reset();
    l2_util_monitor->reset();

    // l2_reuse_monitor->reset();
    l1d_perf_monitor->start();
    l1i_perf_monitor->start();
    l2_perf_monitor->start();
    memory_perf_monitor->start();

    l1d_util_monitor->start();
    l1i_util_monitor->start();
    l2_util_monitor->start();


#ifdef ENABLE_FLEXICAS_THREAD
    // set up the cache server
    std::thread t(cache_server);
    t.detach();
#endif
  }

  void exit() {
    std::cout << "Stopping FlexiCAS..." << std::endl;
    exit_flag = true;
    l1d_perf_monitor->stop();
    l1i_perf_monitor->stop();
    l2_perf_monitor->stop();
    memory_perf_monitor->stop();

    l1d_util_monitor->stop();
    l1i_util_monitor->stop();
    l2_util_monitor->stop();

    // l2_reuse_monitor->stop();
    // tracer->stop();
    std::cout << "FlexiCAS exiting..." << std::endl;
    print_cache_statistics("terminated");
    // l1d_perf_monitor->reset();
    // l1i_perf_monitor->reset();
    // l2_perf_monitor->reset();
    // memory_perf_monitor->reset();

    // l1d_util_monitor->reset();
    // l1i_util_monitor->reset();
    // l2_util_monitor->reset();
  }

  void read(uint64_t addr, int core, bool ic) {
    assert(core < NC);
#ifdef ENABLE_FLEXICAS_THREAD
    xact_queue_add({CACHE_OP_READ, ic, core, addr});
#else
    read_detailed(addr, core, ic);
#endif
  }

  void write(uint64_t addr, int core) {
    assert(core < NC);
#ifdef ENABLE_FLEXICAS_THREAD
    xact_queue_add({CACHE_OP_WRITE, false, core, addr});
#else
    write_detailed(addr, core);
#endif
  }

  void flush(uint64_t addr, int core) {
    assert(core < NC);
#ifdef ENABLE_FLEXICAS_THREAD
    xact_queue_add({CACHE_OP_FLUSH, false, core, addr});
#else
    flush_detailed(addr, core);
#endif
  }

  void flush_icache(int core) {
#ifdef ENABLE_FLEXICAS_THREAD
    xact_queue_add({CACHE_OP_FLUSH_CACHE, true, core, 0});
#else
    flush_icache_detailed(core);
#endif
  }

  void writeback(uint64_t addr, int core) {
    assert(core < NC);
#ifdef ENABLE_FLEXICAS_THREAD
    xact_queue_add({CACHE_OP_WRITEBACK, false, core, addr});
#else
    writeback_detailed(addr, core);
#endif
  }

  void csr_write(uint64_t cmd, int core, tlb_translate_func translator) {
    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_CMD && (cmd & FLEXICAS_PFC_CMD_MASK) == FLEXICAS_PFC_START) {
      std::cout << "FLEXICAS_PFC_START received. Starting monitors..." <<  std::endl;
      l1d_perf_monitor->reset();
      l1i_perf_monitor->reset();
      l2_perf_monitor->reset();
      memory_perf_monitor->reset();

      l1d_util_monitor->reset();
      l1i_util_monitor->reset();
      l2_util_monitor->reset();

      // l2_reuse_monitor->reset();
      l1d_perf_monitor->start();
      l1i_perf_monitor->start();
      l2_perf_monitor->start();
      memory_perf_monitor->start();

      l1d_util_monitor->start();
      l1i_util_monitor->start();
      l2_util_monitor->start();

      // l2_reuse_monitor->start();
      // tracer->start();
      return;
    }

    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_CMD && (cmd & FLEXICAS_PFC_CMD_MASK) == FLEXICAS_PFC_STOP) {
      std::cout << "FLEXICAS_PFC_STOP received. Stopping monitors and printing statistics..." <<  std::endl;
      l1d_perf_monitor->stop();
      l1i_perf_monitor->stop();
      l2_perf_monitor->stop();
      memory_perf_monitor->stop();

      l1d_util_monitor->stop();
      l1i_util_monitor->stop();
      l2_util_monitor->stop();

      // l2_reuse_monitor->stop();
      // tracer->stop();
      print_cache_statistics();
      l1d_perf_monitor->reset();
      l1i_perf_monitor->reset();
      l2_perf_monitor->reset();
      memory_perf_monitor->reset();

      l1d_util_monitor->reset();
      l1i_util_monitor->reset();
      l2_util_monitor->reset();
      return;
    }

    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_CMD && (cmd & FLEXICAS_PFC_CMD_MASK) == FLEXICAS_PFC_STR_CHAR) {
      char c = (cmd >> 16) & 0xff;
      pfc_str.push_back(c);
      return;
    }

    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_CMD && (cmd & FLEXICAS_PFC_CMD_MASK) == FLEXICAS_PFC_STR_CLR) {
      pfc_str.clear();
      return;
    }

    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_CMD && (cmd & FLEXICAS_PFC_CMD_MASK) == FLEXICAS_PFC_PREFIX) {
      if(tracer) tracer->set_prefix(pfc_str);
      return;
    }

    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_QUERY) {
      uint64_t addr = FLEXICAS_PFC_EXTRACT_ADDR(cmd);
      uint64_t paddr = translator(addr);
      cache_sync();
      pfc_value = query_cache(paddr, core)->hit(paddr);
      return;
    }

    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_FLUSH) {
      uint64_t addr = FLEXICAS_PFC_EXTRACT_ADDR(cmd);
      uint64_t paddr = translator(addr);
      flush(paddr, core);
      cache_sync();
      return;
    }

    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_CONGRU_TARGET) {
      uint64_t addr = FLEXICAS_PFC_EXTRACT_ADDR(cmd);
      uint64_t paddr = translator(addr);
      cache_sync();
      coloc_query_cache = query_cache(paddr, core);
      coloc_query_target = paddr;
      return;
    }

    if((cmd & (~FLEXICAS_PFC_ADDR)) == FLEXICAS_PFC_CONGRU_QUERY) {
      uint64_t addr = FLEXICAS_PFC_EXTRACT_ADDR(cmd);
      uint64_t paddr = translator(addr);
      cache_sync();
      pfc_value = coloc_query_cache->query_coloc(coloc_query_target, paddr);
      return;
    }

  }

  uint64_t csr_read(int core) {
    // ToDo: connect this with monitor
    return pfc_value;
  }

  void bump_cycle(int step, int core) {
    core_cycle[core] += step;
  }

  void bump_wall_clock(int step) {
    wall_clock += step;
  }
}
