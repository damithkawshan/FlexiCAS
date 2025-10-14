#ifndef CM_UTIL_SET_UTILIZATION_MONITOR_HPP
#define CM_UTIL_SET_UTILIZATION_MONITOR_HPP

#include "util/monitor.hpp"
#include <vector>
#include <map>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <fstream>

// Monitor to track cache set utilization statistics
class SetUtilizationMonitor : public MonitorBase
{
protected:
  // Configuration
  uint32_t num_sets;
  uint32_t num_ways;
  bool active;
  
  // Per-set statistics
  std::vector<uint64_t> set_access_count;      // Number of accesses per set
  std::vector<uint64_t> set_miss_count;        // Number of misses per set
  std::vector<uint64_t> set_hit_count;         // Number of hits per set
  std::vector<uint64_t> set_eviction_count;    // Number of evictions per set
  std::vector<std::vector<bool>> way_usage;    // Track which ways have been used in each set
  
  // Eviction history tracking
  struct EvictionRecord {
    uint64_t addr;         // Address that was evicted
    int32_t set;           // Set index
    int32_t way;           // Way index
    uint64_t timestamp;    // Timestamp (access count at time of eviction)
  };
  std::vector<EvictionRecord> eviction_history;
  size_t max_history_size;  // Maximum number of eviction records to keep (0 = unlimited)
  
  // Global statistics
  uint64_t total_accesses;
  uint64_t total_misses;
  uint64_t total_evictions;

public:
  SetUtilizationMonitor(uint32_t sets, uint32_t ways, size_t max_history = 10000) 
    : num_sets(sets), num_ways(ways), active(false), max_history_size(max_history),
      total_accesses(0), total_misses(0), total_evictions(0) {
    set_access_count.resize(num_sets, 0);
    set_miss_count.resize(num_sets, 0);
    set_hit_count.resize(num_sets, 0);
    set_eviction_count.resize(num_sets, 0);
    
    // Initialize way usage tracking
    way_usage.resize(num_sets);
    for (auto& ways : way_usage) {
      ways.resize(num_ways, false);
    }
    
    // Reserve space for eviction history
    if (max_history_size > 0) {
      eviction_history.reserve(max_history_size);
    }
  }

  virtual bool attach(uint64_t cache_id) override { 
    return true; 
  }

  virtual void read(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, 
                   bool hit, const CMMetadataBase *meta, const CMDataBase *data) override {
    if (!active || ai < 0 || s < 0 || static_cast<uint32_t>(s) >= num_sets) return;
    
    total_accesses++;
    set_access_count[s]++;
    
    if (hit) {
      //print hit address info with set and way
      // std::cout << "Hit Address: " << std::hex << addr << " Set: " << s << " Way: " << w << std::dec << std::endl;
      set_hit_count[s]++;
      if (w >= 0 && static_cast<uint32_t>(w) < num_ways) {
        way_usage[s][w] = true;
      }
    } else {
      total_misses++;
      set_miss_count[s]++;
    }
  }

  virtual void write(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, 
                    bool hit, const CMMetadataBase *meta, const CMDataBase *data) override {
    if (!active || ai < 0 || s < 0 || static_cast<uint32_t>(s) >= num_sets) return;
    
    total_accesses++;
    set_access_count[s]++;
    
    if (hit) {
      set_hit_count[s]++;
      if (w >= 0 && static_cast<uint32_t>(w) < num_ways) {
        way_usage[s][w] = true;
      }
    } else {
      total_misses++;
      set_miss_count[s]++;
    }
  }

  virtual void invalid(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, 
                      const CMMetadataBase *meta, const CMDataBase *data) override {
    if (!active || s < 0 || static_cast<uint32_t>(s) >= num_sets) return;
    std::string msg;  msg.reserve(100);
    msg += (boost::format("%-10s evict %016x %02d %04d %02d  ") % UniqueID::name(cache_id) % addr % ai % s % w).str() ;
    //APPEND meta or data info if needed
    if(meta)
      msg.append(" [").append(meta->to_string()).append("]");
    else if(data)
      msg.append("      ");
    std::cout << msg << std::endl;
    
    // Track eviction
    total_evictions++;
    set_eviction_count[s]++;
    
    // Record eviction in history
    if (max_history_size == 0 || eviction_history.size() < max_history_size) {
      eviction_history.push_back({addr, s, w, total_accesses});
    } else if (max_history_size > 0) {
      // Circular buffer: overwrite oldest entry
      size_t idx = total_evictions % max_history_size;
      eviction_history[idx] = {addr, s, w, total_accesses};
    }
  }

  virtual void start() override { active = true; }
  virtual void stop() override { active = false; }
  virtual void pause() override { active = false; }
  virtual void resume() override { active = true; }
  
  virtual void reset() override {
    active = false;
    total_accesses = 0;
    total_misses = 0;
    total_evictions = 0;
    std::fill(set_access_count.begin(), set_access_count.end(), 0);
    std::fill(set_miss_count.begin(), set_miss_count.end(), 0);
    std::fill(set_hit_count.begin(), set_hit_count.end(), 0);
    std::fill(set_eviction_count.begin(), set_eviction_count.end(), 0);
    eviction_history.clear();
    for (auto& ways : way_usage) {
      std::fill(ways.begin(), ways.end(), false);
    }
  }

  // Utility methods to get statistics
  uint32_t get_num_sets() const { return num_sets; }
  uint32_t get_num_ways() const { return num_ways; }
  uint64_t get_total_accesses() const { return total_accesses; }
  uint64_t get_total_misses() const { return total_misses; }
  uint64_t get_total_evictions() const { return total_evictions; }
  
  uint64_t get_set_accesses(uint32_t set) const {
    if (set >= num_sets) return 0;
    return set_access_count[set];
  }
  
  uint64_t get_set_misses(uint32_t set) const {
    if (set >= num_sets) return 0;
    return set_miss_count[set];
  }
  
  uint64_t get_set_hits(uint32_t set) const {
    if (set >= num_sets) return 0;
    return set_hit_count[set];
  }
  
  uint64_t get_set_evictions(uint32_t set) const {
    if (set >= num_sets) return 0;
    return set_eviction_count[set];
  }
  
  // Get eviction history
  const std::vector<EvictionRecord>& get_eviction_history() const {
    return eviction_history;
  }
  
  size_t get_eviction_history_size() const {
    return eviction_history.size();
  }
  
  // Get number of ways actually used in a set
  uint32_t get_ways_used(uint32_t set) const {
    if (set >= num_sets) return 0;
    return std::count(way_usage[set].begin(), way_usage[set].end(), true);
  }
  
  // Get number of sets that were accessed at least once
  uint32_t get_utilized_sets() const {
    return std::count_if(set_access_count.begin(), set_access_count.end(),
                        [](uint64_t count) { return count > 0; });
  }
  
  // Calculate set utilization percentage
  double get_set_utilization() const {
    return (get_utilized_sets() * 100.0) / num_sets;
  }
  
  // Calculate average way utilization across all sets
  double get_avg_way_utilization() const {
    uint64_t total_ways_used = 0;
    uint32_t active_sets = 0;
    
    for (uint32_t s = 0; s < num_sets; s++) {
      if (set_access_count[s] > 0) {
        total_ways_used += get_ways_used(s);
        active_sets++;
      }
    }
    
    if (active_sets == 0) return 0.0;
    return (total_ways_used * 100.0) / (active_sets * num_ways);
  }

  // Get distribution of access counts
  std::map<uint64_t, uint32_t> get_access_distribution() const {
    std::map<uint64_t, uint32_t> distribution;
    for (const auto& count : set_access_count) {
      distribution[count]++;
    }
    return distribution;
  }
  
  // Print detailed statistics
  void print_statistics(const std::string& cache_name = "") const {
    if (!cache_name.empty()) {
      std::cout << "\n========================================" << std::endl;
      std::cout << "=== " << cache_name << " Set Utilization ===" << std::endl;
      std::cout << "========================================" << std::endl;
    }
    
    std::cout << "\n--- Configuration ---" << std::endl;
    std::cout << "Total Sets:           " << num_sets << std::endl;
    std::cout << "Ways per Set:         " << num_ways << std::endl;
    std::cout << "Total Cache Lines:    " << (num_sets * num_ways) << std::endl;
    
    std::cout << "\n--- Access Statistics ---" << std::endl;
    std::cout << "Total Accesses:       " << total_accesses << std::endl;
    std::cout << "Total Misses:         " << total_misses << std::endl;
    std::cout << "Total Hits:           " << (total_accesses - total_misses) << std::endl;
    std::cout << "Total Evictions:      " << total_evictions << std::endl;
    if (total_accesses > 0) {
      double miss_rate = (total_misses * 100.0) / total_accesses;
      double eviction_rate = (total_evictions * 100.0) / total_accesses;
      std::cout << "Miss Rate:            " << std::fixed << std::setprecision(2) 
                << miss_rate << "%" << std::endl;
      std::cout << "Eviction Rate:        " << std::setprecision(2) 
                << eviction_rate << "%" << std::endl;
    }
    
    std::cout << "\n--- Set Utilization ---" << std::endl;
    uint32_t utilized_sets = get_utilized_sets();
    std::cout << "Sets Accessed:        " << utilized_sets << " / " << num_sets;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << " (" << get_set_utilization() << "%)" << std::endl;
    std::cout << "Unused Sets:          " << (num_sets - utilized_sets) << std::endl;
    
    std::cout << "\n--- Way Utilization ---" << std::endl;
    std::cout << "Avg Ways Used:        " << std::setprecision(2) 
              << get_avg_way_utilization() << "%" << std::endl;
    
    // Calculate way utilization distribution
    std::map<uint32_t, uint32_t> way_dist;
    for (uint32_t s = 0; s < num_sets; s++) {
      if (set_access_count[s] > 0) {
        way_dist[get_ways_used(s)]++;
      }
    }
    
    std::cout << "\nWay Usage Distribution (for active sets):" << std::endl;
    for (const auto& [ways_used, count] : way_dist) {
      double pct = (count * 100.0) / utilized_sets;
      std::cout << "  " << ways_used << " way(s) used: " 
                << std::setw(6) << count << " sets (" 
                << std::setw(5) << std::setprecision(2) << pct << "%)" << std::endl;
    }
    
    // Access frequency statistics
    std::cout << "\n--- Access Frequency Statistics ---" << std::endl;
    if (total_accesses > 0) {
      uint64_t max_accesses = *std::max_element(set_access_count.begin(), set_access_count.end());
      uint64_t min_accesses_nonzero = max_accesses;
      for (const auto& count : set_access_count) {
        if (count > 0 && count < min_accesses_nonzero) {
          min_accesses_nonzero = count;
        }
      }
      
      std::cout << "Max Accesses/Set:     " << max_accesses << std::endl;
      std::cout << "Min Accesses/Set:     " << min_accesses_nonzero << " (non-zero)" << std::endl;
      
      // Calculate average for utilized sets
      uint64_t total_utilized_accesses = 0;
      for (const auto& count : set_access_count) {
        if (count > 0) total_utilized_accesses += count;
      }
      double avg_accesses = static_cast<double>(total_utilized_accesses) / utilized_sets;
      std::cout << "Avg Accesses/Set:     " << std::setprecision(2) << avg_accesses 
                << " (for utilized sets)" << std::endl;
    }
    
    // Eviction frequency statistics
    std::cout << "\n--- Eviction Statistics ---" << std::endl;
    std::cout << "Total Evictions:      " << total_evictions << std::endl;
    std::cout << "Eviction History Size:" << eviction_history.size();
    if (max_history_size > 0) {
      std::cout << " (max: " << max_history_size << ")";
    }
    std::cout << std::endl;
    
    if (total_evictions > 0) {
      // Find sets with evictions
      uint32_t sets_with_evictions = 0;
      uint64_t max_evictions = 0;
      uint64_t min_evictions_nonzero = UINT64_MAX;
      
      for (uint32_t s = 0; s < num_sets; s++) {
        uint64_t evictions = set_eviction_count[s];
        if (evictions > 0) {
          sets_with_evictions++;
          max_evictions = std::max(max_evictions, evictions);
          min_evictions_nonzero = std::min(min_evictions_nonzero, evictions);
        }
      }
      
      std::cout << "Sets with Evictions:  " << sets_with_evictions << " / " << num_sets;
      if (num_sets > 0) {
        std::cout << " (" << std::setprecision(2) << (sets_with_evictions * 100.0 / num_sets) << "%)";
      }
      std::cout << std::endl;
      std::cout << "Max Evictions/Set:    " << max_evictions << std::endl;
      if (min_evictions_nonzero != UINT64_MAX) {
        std::cout << "Min Evictions/Set:    " << min_evictions_nonzero << " (non-zero)" << std::endl;
      }
      if (sets_with_evictions > 0) {
        double avg_evictions = static_cast<double>(total_evictions) / sets_with_evictions;
        std::cout << "Avg Evictions/Set:    " << std::setprecision(2) << avg_evictions 
                  << " (for sets with evictions)" << std::endl;
      }
      
      // Show top eviction-heavy sets
      std::vector<std::pair<uint32_t, uint64_t>> eviction_sets;
      for (uint32_t s = 0; s < num_sets; s++) {
        if (set_eviction_count[s] > 0) {
          eviction_sets.push_back({s, set_eviction_count[s]});
        }
      }
      
      if (!eviction_sets.empty()) {
        std::sort(eviction_sets.begin(), eviction_sets.end(), 
                 [](const auto& a, const auto& b) { return a.second > b.second; });
        
        size_t show_count = std::min(eviction_sets.size(), size_t(10));
        std::cout << "\nTop " << show_count << " Sets by Evictions:" << std::endl;
        for (size_t i = 0; i < show_count; i++) {
          uint32_t set = eviction_sets[i].first;
          uint64_t evictions = eviction_sets[i].second;
          uint64_t accesses = set_access_count[set];
          double evict_rate = (accesses > 0) ? (evictions * 100.0 / accesses) : 0.0;
          std::cout << "  Set " << std::setw(6) << set << ": " 
                    << std::setw(10) << evictions << " evictions";
          if (accesses > 0) {
            std::cout << " (" << std::setw(5) << std::setprecision(2) << evict_rate << "% of accesses)";
          }
          std::cout << std::endl;
        }
      }
    }
    
    // Hotspot analysis (sets with >1% of total accesses)
    if (total_accesses > 0) {
      std::vector<std::pair<uint32_t, uint64_t>> hot_sets;
      uint64_t threshold = total_accesses / 100; // 1% threshold
      
      for (uint32_t s = 0; s < num_sets; s++) {
        if (set_access_count[s] > threshold) {
          hot_sets.push_back({s, set_access_count[s]});
        }
      }
      
      if (!hot_sets.empty()) {
        std::sort(hot_sets.begin(), hot_sets.end(), 
                 [](const auto& a, const auto& b) { return a.second > b.second; });
        
        std::cout << "\n--- Hot Sets (>1% of accesses) ---" << std::endl;
        std::cout << "Number of Hot Sets:   " << hot_sets.size() << std::endl;
        
        // Show top 10 hot sets
        size_t show_count = std::min(hot_sets.size(), size_t(10));
        std::cout << "\nTop " << show_count << " Hottest Sets:" << std::endl;
        for (size_t i = 0; i < show_count; i++) {
          uint32_t set = hot_sets[i].first;
          uint64_t accesses = hot_sets[i].second;
          double pct = (accesses * 100.0) / total_accesses;
          std::cout << "  Set " << std::setw(6) << set << ": " 
                    << std::setw(10) << accesses << " accesses (" 
                    << std::setw(5) << std::setprecision(2) << pct << "%), "
                    << get_ways_used(set) << "/" << num_ways << " ways used" << std::endl;
        }
      }
    }
    
    std::cout << std::endl;
  }
  
  // Export data to CSV for further analysis
  void export_to_csv(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
      std::cerr << "Failed to open " << filename << " for writing" << std::endl;
      return;
    }
    
    file << "Set,Accesses,Hits,Misses,Evictions,WaysUsed,HitRate,EvictionRate\n";
    for (uint32_t s = 0; s < num_sets; s++) {
      uint64_t accesses = set_access_count[s];
      uint64_t hits = set_hit_count[s];
      uint64_t misses = set_miss_count[s];
      uint64_t evictions = set_eviction_count[s];
      uint32_t ways_used = get_ways_used(s);
      double hit_rate = (accesses > 0) ? (hits * 100.0 / accesses) : 0.0;
      double eviction_rate = (accesses > 0) ? (evictions * 100.0 / accesses) : 0.0;
      
      file << s << "," << accesses << "," << hits << "," << misses << "," 
           << evictions << "," << ways_used << "," 
           << std::fixed << std::setprecision(2) << hit_rate << "," 
           << eviction_rate << "\n";
    }
    
    file.close();
    std::cout << "Set utilization data exported to: " << filename << std::endl;
  }
  
  // Export eviction history to CSV
  void export_eviction_history_to_csv(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
      std::cerr << "Failed to open " << filename << " for writing" << std::endl;
      return;
    }
    
    file << "Address,Set,Way,Timestamp\n";
    for (const auto& record : eviction_history) {
      file << "0x" << std::hex << record.addr << std::dec << "," 
           << record.set << "," << record.way << "," << record.timestamp << "\n";
    }
    
    file.close();
    std::cout << "Eviction history exported to: " << filename << std::endl;
  }
};

#endif // CM_UTIL_SET_UTILIZATION_MONITOR_HPP
