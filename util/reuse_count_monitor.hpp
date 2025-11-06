#ifndef CM_UTIL_REUSE_COUNT_MONITOR_HPP
#define CM_UTIL_REUSE_COUNT_MONITOR_HPP

#include "util/monitor.hpp"
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <fstream>

/**
 * ReuseCountMonitor - Tracks cache line reuse patterns in L2 cache
 * 
 * This monitor implements reuse count tracking as described:
 * - When an L2 cache line is installed, its reuse count is initialized to zero
 * - Each subsequent L2 access to the cache line increments its reuse count by one
 * - When a line is evicted from L2, its reuse count is recorded in a global reuse distribution
 * 
 * The reuse distribution helps understand temporal locality at the L2 level, where
 * the reference stream is filtered by L1 (only L1 misses reach L2).
 */
class ReuseCountMonitor : public MonitorBase
{
protected:
  // Configuration
  uint32_t num_sets;
  uint32_t num_ways;
  bool active;
  
  // Cache line tracking: maps (set, way) -> reuse_count
  // We use set*num_ways + way as the key for fast access
  std::vector<uint64_t> line_reuse_count;
  std::vector<bool> line_valid;
  
  // Global reuse distribution: reuse_count -> frequency
  // This is the histogram of reuse counts observed upon eviction
  std::map<uint64_t, uint64_t> reuse_distribution;
  
  // Additional statistics
  uint64_t total_fills;          // Total number of cache line fills (misses)
  uint64_t total_reuses;         // Total number of reuses (hits after initial fill)
  uint64_t total_evictions;      // Total number of evictions
  uint64_t total_accesses;       // Total number of L2 accesses (hits + misses)
  
  // Optional: Track detailed eviction records
  struct EvictionRecord {
    uint64_t addr;
    int32_t set;
    int32_t way;
    uint64_t reuse_count;
    uint64_t timestamp;
  };
  std::vector<EvictionRecord> eviction_history;
  size_t max_history_size;
  
  // Helper to get index for line tracking
  inline size_t get_line_index(int32_t s, int32_t w) const {
    return static_cast<size_t>(s) * num_ways + static_cast<size_t>(w);
  }

public:
  ReuseCountMonitor(uint32_t sets, uint32_t ways, size_t max_history = 10000)
    : num_sets(sets), num_ways(ways), active(false), max_history_size(max_history),
      total_fills(0), total_reuses(0), total_evictions(0), total_accesses(0) {
    
    // Initialize per-line tracking arrays
    size_t total_lines = static_cast<size_t>(num_sets) * num_ways;
    line_reuse_count.resize(total_lines, 0);
    line_valid.resize(total_lines, false);
    
    // Reserve space for eviction history
    if (max_history_size > 0) {
      eviction_history.reserve(max_history_size);
    }
  }

  virtual bool attach(uint64_t cache_id) override { 
    return true; 
  }

  /**
   * Handle read access to L2 cache
   * - If hit: increment reuse count for this cache line
   * - If miss: initialize reuse count to 0 (will be set on subsequent access)
   */
  virtual void read(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, 
                   bool hit, const CMMetadataBase *meta, const CMDataBase *data) override {
    if (!active || ai < 0 || s < 0 || w < 0) return;
    if (static_cast<uint32_t>(s) >= num_sets || static_cast<uint32_t>(w) >= num_ways) return;
    
    total_accesses++;
    size_t idx = get_line_index(s, w);
    
    if (hit) {
      // This is a reuse - increment the reuse count
      line_reuse_count[idx]++;
      total_reuses++;
    } else {
      // This is a miss - cache line fill
      // Initialize reuse count to 0 for this new line
      line_reuse_count[idx] = 0;
      line_valid[idx] = true;
      total_fills++;
    }
  }

  /**
   * Handle write access to L2 cache
   * Same logic as read - increment reuse count on hit, initialize on miss
   */
  virtual void write(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, 
                    bool hit, const CMMetadataBase *meta, const CMDataBase *data) override {
    if (!active || ai < 0 || s < 0 || w < 0) return;
    if (static_cast<uint32_t>(s) >= num_sets || static_cast<uint32_t>(w) >= num_ways) return;
    
    total_accesses++;
    size_t idx = get_line_index(s, w);
    
    if (hit) {
      // This is a reuse - increment the reuse count
      line_reuse_count[idx]++;
      total_reuses++;
    } else {
      // This is a miss - cache line fill
      line_reuse_count[idx] = 0;
      line_valid[idx] = true;
      total_fills++;
    }
  }

  /**
   * Handle cache line eviction
   * Read the reuse count and update the global reuse distribution
   */
  virtual void invalid(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, 
                      const CMMetadataBase *meta, const CMDataBase *data) override {
    if (!active || s < 0 || w < 0) return;
    if (static_cast<uint32_t>(s) >= num_sets || static_cast<uint32_t>(w) >= num_ways) return;
    
    size_t idx = get_line_index(s, w);
    
    // Only process if this line was valid (not already evicted)
    if (!line_valid[idx]) return;
    
    // Read the reuse count for this evicted line
    uint64_t reuse_count = line_reuse_count[idx];
    
    // Update the global reuse distribution
    reuse_distribution[reuse_count]++;
    total_evictions++;
    
    // Record in history if enabled
    if (max_history_size == 0 || eviction_history.size() < max_history_size) {
      eviction_history.push_back({addr, s, w, reuse_count, total_accesses});
    } else if (max_history_size > 0) {
      // Circular buffer
      size_t history_idx = (total_evictions - 1) % max_history_size;
      eviction_history[history_idx] = {addr, s, w, reuse_count, total_accesses};
    }
    
    // Reset the line tracking
    line_reuse_count[idx] = 0;
    line_valid[idx] = false;
  }

  virtual void start() override { active = true; }
  virtual void stop() override { active = false; }
  virtual void pause() override { active = false; }
  virtual void resume() override { active = true; }
  
  virtual void reset() override {
    active = false;
    total_fills = 0;
    total_reuses = 0;
    total_evictions = 0;
    total_accesses = 0;
    std::fill(line_reuse_count.begin(), line_reuse_count.end(), 0);
    std::fill(line_valid.begin(), line_valid.end(), false);
    reuse_distribution.clear();
    eviction_history.clear();
  }

  // Accessor methods
  uint64_t get_total_fills() const { return total_fills; }
  uint64_t get_total_reuses() const { return total_reuses; }
  uint64_t get_total_evictions() const { return total_evictions; }
  uint64_t get_total_accesses() const { return total_accesses; }
  
  const std::map<uint64_t, uint64_t>& get_reuse_distribution() const {
    return reuse_distribution;
  }
  
  const std::vector<EvictionRecord>& get_eviction_history() const {
    return eviction_history;
  }
  
  /**
   * Calculate statistics about the reuse distribution
   */
  double get_average_reuse() const {
    if (total_evictions == 0) return 0.0;
    
    uint64_t total_reuse_sum = 0;
    for (const auto& [reuse_count, frequency] : reuse_distribution) {
      total_reuse_sum += reuse_count * frequency;
    }
    
    return static_cast<double>(total_reuse_sum) / total_evictions;
  }
  
  uint64_t get_max_reuse_count() const {
    if (reuse_distribution.empty()) return 0;
    return reuse_distribution.rbegin()->first;
  }
  
  uint64_t get_zero_reuse_evictions() const {
    auto it = reuse_distribution.find(0);
    return (it != reuse_distribution.end()) ? it->second : 0;
  }
  
  /**
   * Print detailed reuse distribution statistics
   */
  void print_statistics(const std::string& cache_name = "") const {
    if (!cache_name.empty()) {
      std::cout << "\n========================================" << std::endl;
      std::cout << "=== " << cache_name << " Reuse Count Analysis ===" << std::endl;
      std::cout << "========================================" << std::endl;
    }
    
    std::cout << "\n--- Overall Statistics ---" << std::endl;
    std::cout << "Total L2 Accesses:    " << total_accesses << std::endl;
    std::cout << "Total Cache Fills:    " << total_fills << " (misses)" << std::endl;
    std::cout << "Total Reuses:         " << total_reuses << " (hits after fill)" << std::endl;
    std::cout << "Total Evictions:      " << total_evictions << std::endl;
    
    if (total_accesses > 0) {
      double miss_rate = (total_fills * 100.0) / total_accesses;
      double hit_rate = 100.0 - miss_rate;
      std::cout << "L2 Hit Rate:          " << std::fixed << std::setprecision(2) 
                << hit_rate << "%" << std::endl;
      std::cout << "L2 Miss Rate:         " << std::setprecision(2) 
                << miss_rate << "%" << std::endl;
    }
    
    if (total_evictions == 0) {
      std::cout << "\nNo evictions recorded yet." << std::endl;
      return;
    }
    
    std::cout << "\n--- Reuse Distribution Summary ---" << std::endl;
    std::cout << "Average Reuse Count:  " << std::fixed << std::setprecision(2) 
              << get_average_reuse() << std::endl;
    std::cout << "Max Reuse Count:      " << get_max_reuse_count() << std::endl;
    
    uint64_t zero_reuse = get_zero_reuse_evictions();
    double zero_reuse_pct = (zero_reuse * 100.0) / total_evictions;
    std::cout << "Zero Reuse Evictions: " << zero_reuse 
              << " (" << std::setprecision(2) << zero_reuse_pct << "%)" << std::endl;
    
    // Calculate cumulative statistics
    std::cout << "\n--- Reuse Distribution (Histogram) ---" << std::endl;
    std::cout << "Reuse Count | Frequency | Percentage | Cumulative %" << std::endl;
    std::cout << "------------|-----------|------------|-------------" << std::endl;
    
    uint64_t cumulative_count = 0;
    for (const auto& [reuse_count, frequency] : reuse_distribution) {
      cumulative_count += frequency;
      double percentage = (frequency * 100.0) / total_evictions;
      double cumulative_pct = (cumulative_count * 100.0) / total_evictions;
      
      std::cout << std::setw(11) << reuse_count << " | "
                << std::setw(9) << frequency << " | "
                << std::setw(9) << std::fixed << std::setprecision(2) << percentage << "% | "
                << std::setw(10) << std::setprecision(2) << cumulative_pct << "%" << std::endl;
      
      // Limit output for very large distributions
      if (reuse_distribution.size() > 50 && reuse_count > 25) {
        std::cout << "... (showing first 25 entries, " 
                  << (reuse_distribution.size() - 25) << " more entries omitted)" << std::endl;
        break;
      }
    }
    
    // Show percentiles
    std::cout << "\n--- Reuse Count Percentiles ---" << std::endl;
    std::vector<double> percentiles = {0.25, 0.50, 0.75, 0.90, 0.95, 0.99};
    
    for (double p : percentiles) {
      uint64_t target = static_cast<uint64_t>(p * total_evictions);
      uint64_t cumulative = 0;
      uint64_t percentile_value = 0;
      
      for (const auto& [reuse_count, frequency] : reuse_distribution) {
        cumulative += frequency;
        if (cumulative >= target) {
          percentile_value = reuse_count;
          break;
        }
      }
      
      std::cout << "P" << std::setw(2) << static_cast<int>(p * 100) << ": " 
                << std::setw(10) << percentile_value << " reuses" << std::endl;
    }
    
    // Locality analysis
    std::cout << "\n--- Locality Analysis ---" << std::endl;
    uint64_t low_reuse = 0;  // 0-1 reuses
    uint64_t medium_reuse = 0;  // 2-9 reuses
    uint64_t high_reuse = 0;  // 10+ reuses
    
    for (const auto& [reuse_count, frequency] : reuse_distribution) {
      if (reuse_count <= 1) {
        low_reuse += frequency;
      } else if (reuse_count <= 9) {
        medium_reuse += frequency;
      } else {
        high_reuse += frequency;
      }
    }
    
    std::cout << "Low Reuse (0-1):      " << std::setw(10) << low_reuse 
              << " (" << std::fixed << std::setprecision(2) 
              << (low_reuse * 100.0 / total_evictions) << "%)" << std::endl;
    std::cout << "Medium Reuse (2-9):   " << std::setw(10) << medium_reuse 
              << " (" << std::setprecision(2) 
              << (medium_reuse * 100.0 / total_evictions) << "%)" << std::endl;
    std::cout << "High Reuse (10+):     " << std::setw(10) << high_reuse 
              << " (" << std::setprecision(2) 
              << (high_reuse * 100.0 / total_evictions) << "%)" << std::endl;
    
    std::cout << "\nInterpretation:" << std::endl;
    if (zero_reuse_pct > 50) {
      std::cout << "  * High zero-reuse rate suggests poor temporal locality in L2" << std::endl;
      std::cout << "    (many lines evicted before reuse due to L1 filtering)" << std::endl;
    }
    if (high_reuse * 100.0 / total_evictions > 20) {
      std::cout << "  * Significant high-reuse lines indicate good working set capture" << std::endl;
    }
    if (get_average_reuse() < 2.0) {
      std::cout << "  * Low average reuse suggests L2 may be undersized or" << std::endl;
      std::cout << "    working set doesn't fit well in L2 cache" << std::endl;
    }
    
    std::cout << std::endl;
  }
  
  /**
   * Export reuse distribution to CSV
   */
  void export_reuse_distribution_csv(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
      std::cerr << "Failed to open " << filename << " for writing" << std::endl;
      return;
    }
    
    file << "ReuseCount,Frequency,Percentage,CumulativePercentage\n";
    
    uint64_t cumulative = 0;
    for (const auto& [reuse_count, frequency] : reuse_distribution) {
      cumulative += frequency;
      double percentage = (frequency * 100.0) / total_evictions;
      double cumulative_pct = (cumulative * 100.0) / total_evictions;
      
      file << reuse_count << "," << frequency << "," 
           << std::fixed << std::setprecision(4) << percentage << "," 
           << cumulative_pct << "\n";
    }
    
    file.close();
    std::cout << "Reuse distribution exported to: " << filename << std::endl;
  }
  
  /**
   * Export detailed eviction history to CSV
   */
  void export_eviction_history_csv(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
      std::cerr << "Failed to open " << filename << " for writing" << std::endl;
      return;
    }
    
    file << "Address,Set,Way,ReuseCount,Timestamp\n";
    for (const auto& record : eviction_history) {
      file << "0x" << std::hex << record.addr << std::dec << "," 
           << record.set << "," << record.way << "," 
           << record.reuse_count << "," << record.timestamp << "\n";
    }
    
    file.close();
    std::cout << "Eviction history with reuse counts exported to: " << filename << std::endl;
  }
  
  /**
   * Export summary statistics to text file
   */
  void export_summary_txt(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
      std::cerr << "Failed to open " << filename << " for writing" << std::endl;
      return;
    }
    
    file << "L2 Cache Reuse Count Analysis Summary\n";
    file << "=====================================\n\n";
    
    file << "Total L2 Accesses:    " << total_accesses << "\n";
    file << "Total Cache Fills:    " << total_fills << "\n";
    file << "Total Reuses:         " << total_reuses << "\n";
    file << "Total Evictions:      " << total_evictions << "\n";
    file << "Average Reuse Count:  " << std::fixed << std::setprecision(2) 
         << get_average_reuse() << "\n";
    file << "Max Reuse Count:      " << get_max_reuse_count() << "\n";
    file << "Zero Reuse Evictions: " << get_zero_reuse_evictions() 
         << " (" << std::setprecision(2) 
         << (get_zero_reuse_evictions() * 100.0 / total_evictions) << "%)\n";
    
    file.close();
    std::cout << "Summary statistics exported to: " << filename << std::endl;
  }
};

#endif // CM_UTIL_REUSE_COUNT_MONITOR_HPP
