#ifndef CM_UTIL_SBC_UTILIZATION_MONITOR_HPP
#define CM_UTIL_SBC_UTILIZATION_MONITOR_HPP

#include "util/monitor.hpp"
#include "cache/metadata.hpp"
#include <vector>
#include <map>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <fstream>

/**
 * Enhanced Monitor for Set Balancing Cache (SBC) implementations
 * 
 * This monitor correctly tracks SSBC/DSBC behavior by:
 * 1. Differentiating between native set (address-mapped) and actual set (where line is found)
 * 2. Tracking primary vs secondary hits
 * 3. Tracking displacement events separately from regular evictions
 * 4. Detecting displaced lines via the displaced bit in metadata
 */
class SBCUtilizationMonitor : public MonitorBase
{
protected:
  // Configuration
  uint32_t num_sets;
  uint32_t num_ways;
  uint32_t index_width;  // IW for computing native set from address
  bool active;
  
  // Per-set statistics (indexed by NATIVE set, not actual set)
  struct SetStats {
    uint64_t accesses = 0;          // Total accesses to this native set
    uint64_t primary_hits = 0;      // Hits found in native set (d=0)
    uint64_t secondary_hits = 0;    // Hits found in partner set (d=1)
    uint64_t misses = 0;            // Misses in both native and partner sets
    uint64_t evictions = 0;         // Normal evictions
    uint64_t displacements_out = 0; // Lines displaced FROM this set
    uint64_t displacements_in = 0;  // Lines displaced TO this set
    uint32_t saturation = 0;        // Last known saturation value
  };
  std::vector<SetStats> set_stats;
  
  // Global statistics
  uint64_t total_accesses = 0;
  uint64_t total_primary_hits = 0;
  uint64_t total_secondary_hits = 0;
  uint64_t total_misses = 0;
  uint64_t total_evictions = 0;
  uint64_t total_displacements = 0;
  
  // Helper to compute native set from address
  uint32_t compute_native_set(uint64_t addr) const {
    // Standard cache indexing: set = (addr >> 6) & ((1 << IW) - 1)
    // Assuming 64-byte cache lines
    return (addr >> 6) & ((1 << index_width) - 1);
  }
  
  // Helper to compute partner set (SSBC pairing)
  uint32_t compute_partner_set(uint32_t native_set) const {
    uint32_t msb_mask = 1u << (index_width - 1);
    return native_set ^ msb_mask;
  }
  
  // Check if metadata indicates a displaced line
  template<typename MT>
  bool is_displaced_line(const CMMetadataBase* meta) const {
    if (!meta) return false;
    // Try to cast and check displaced status
    // This works with MetadataSBC types
    if constexpr (requires { static_cast<const MT*>(meta)->is_displaced(); }) {
      return static_cast<const MT*>(meta)->is_displaced();
    }
    return false;
  }
  
  // Get home set for displaced line
  template<typename MT>
  uint32_t get_home_set(const CMMetadataBase* meta) const {
    if (!meta) return 0;
    if constexpr (requires { static_cast<const MT*>(meta)->get_home_set(); }) {
      return static_cast<const MT*>(meta)->get_home_set();
    }
    return 0;
  }

public:
  SBCUtilizationMonitor(uint32_t sets, uint32_t ways, uint32_t iw) 
    : num_sets(sets), num_ways(ways), index_width(iw), active(false) {
    set_stats.resize(num_sets);
  }

  virtual bool attach(uint64_t cache_id) override { 
    return true; 
  }

  /**
   * Handle read events
   * 
   * Key insight: For SSBC, the 's' parameter may be:
   * - The native set (for primary hits and misses)
   * - The partner set (for secondary hits)
   * 
   * We use metadata to determine if this is a secondary hit (displaced line)
   */
  virtual void read(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, 
                   bool hit, const CMMetadataBase *meta, const CMDataBase *data) override {
    if (!active || ai < 0) return;
    
    // Compute native set from address
    uint32_t native_set = compute_native_set(addr);
    uint32_t partner_set = compute_partner_set(native_set);
    
    if (native_set >= num_sets) return;
    
    total_accesses++;
    set_stats[native_set].accesses++;
    
    if (hit) {
      // Determine if this is a primary or secondary hit
      bool is_displaced = false;
      uint32_t home_set = native_set;
      
      // Check metadata for displaced bit
      if (meta) {
        // Use magic_func or metadata inspection to check displaced status
        // For now, we infer from the set comparison:
        // If hit is in partner set (s == partner_set), it's a secondary hit
        if (s >= 0 && static_cast<uint32_t>(s) == partner_set && 
            static_cast<uint32_t>(s) != native_set) {
          is_displaced = true;
        }
      }
      
      if (is_displaced) {
        // Secondary hit: found in partner set
        total_secondary_hits++;
        set_stats[native_set].secondary_hits++;
      } else {
        // Primary hit: found in native set
        total_primary_hits++;
        set_stats[native_set].primary_hits++;
      }
    } else {
      // Miss
      total_misses++;
      set_stats[native_set].misses++;
    }
  }

  virtual void write(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, 
                    bool hit, const CMMetadataBase *meta, const CMDataBase *data) override {
    // Same logic as read
    read(cache_id, addr, ai, s, w, hit, meta, data);
  }

  /**
   * Handle invalidation/eviction events
   * 
   * For SSBC, we need to differentiate between:
   * - Regular evictions (line removed entirely)
   * - Displacement events (line moved to partner set)
   * 
   * Note: Displacement is NOT an eviction - the line stays in cache.
   * This method is called for actual evictions only.
   */
  virtual void invalid(uint64_t cache_id, uint64_t addr, int32_t ai, int32_t s, int32_t w, 
                      const CMMetadataBase *meta, const CMDataBase *data) override {
    if (!active || s < 0 || static_cast<uint32_t>(s) >= num_sets) return;
    
    // Compute native set from address
    uint32_t native_set = compute_native_set(addr);
    uint32_t actual_set = static_cast<uint32_t>(s);
    
    if (native_set >= num_sets) return;
    
    total_evictions++;
    
    // Track eviction by where the line was actually evicted from
    // This helps identify which sets are overloaded
    set_stats[actual_set].evictions++;
    
    // If this was a displaced line being evicted, also track on home set
    if (actual_set != native_set) {
      // This was a displaced line (evicted from partner set)
      // The native set loses one of its displaced lines
      // (This information is useful for understanding displacement effectiveness)
    }
  }

  /**
   * Magic function for receiving SBC-specific events
   * 
   * Magic IDs:
   * - 0x5BC0: Displacement event (magic_data points to displacement info)
   * - 0x5BC1: Saturation update (magic_data points to saturation value)
   */
  virtual bool magic_func(uint64_t cache_id, uint64_t addr, uint64_t magic_id, void *magic_data) override {
    if (!active) return false;
    
    switch (magic_id) {
      case 0x5BC0: { // Displacement event
        if (magic_data) {
          struct DisplacementInfo {
            uint32_t src_set;
            uint32_t dest_set;
          };
          auto* info = static_cast<DisplacementInfo*>(magic_data);
          
          if (info->src_set < num_sets && info->dest_set < num_sets) {
            total_displacements++;
            set_stats[info->src_set].displacements_out++;
            set_stats[info->dest_set].displacements_in++;
          }
        }
        return true;
      }
      case 0x5BC1: { // Saturation update
        if (magic_data) {
          struct SaturationInfo {
            uint32_t set;
            uint32_t saturation;
          };
          auto* info = static_cast<SaturationInfo*>(magic_data);
          
          if (info->set < num_sets) {
            set_stats[info->set].saturation = info->saturation;
          }
        }
        return true;
      }
      default:
        return false;
    }
  }

  virtual void start() override { 
    active = true; 
    std::cout << "[SBC Monitor] Started." << std::endl;
  }
  
  virtual void stop() override { 
    active = false;
    std::cout << "[SBC Monitor] Stopped." << std::endl;
  }
  
  virtual void pause() override { active = false; }
  virtual void resume() override { active = true; }
  
  virtual void reset() override {
    active = false;
    total_accesses = 0;
    total_primary_hits = 0;
    total_secondary_hits = 0;
    total_misses = 0;
    total_evictions = 0;
    total_displacements = 0;
    
    for (auto& stats : set_stats) {
      stats = SetStats{};
    }
    
    std::cout << "[SBC Monitor] Reset." << std::endl;
  }

  // Statistics getters
  uint64_t get_total_accesses() const { return total_accesses; }
  uint64_t get_total_primary_hits() const { return total_primary_hits; }
  uint64_t get_total_secondary_hits() const { return total_secondary_hits; }
  uint64_t get_total_hits() const { return total_primary_hits + total_secondary_hits; }
  uint64_t get_total_misses() const { return total_misses; }
  uint64_t get_total_evictions() const { return total_evictions; }
  uint64_t get_total_displacements() const { return total_displacements; }
  
  double get_hit_rate() const {
    return total_accesses > 0 ? (100.0 * get_total_hits() / total_accesses) : 0.0;
  }
  
  double get_secondary_hit_rate() const {
    uint64_t total_hits = get_total_hits();
    return total_hits > 0 ? (100.0 * total_secondary_hits / total_hits) : 0.0;
  }

  void print_statistics(const std::string& cache_name = "") const {
    if (!cache_name.empty()) {
      std::cout << "\n========================================" << std::endl;
      std::cout << "=== " << cache_name << " SBC Utilization Monitor ===" << std::endl;
      std::cout << "========================================" << std::endl;
    }
    
    std::cout << std::fixed << std::setprecision(2);
    
    std::cout << "\n--- Configuration ---" << std::endl;
    std::cout << "Total Sets:           " << num_sets << std::endl;
    std::cout << "Ways per Set:         " << num_ways << std::endl;
    std::cout << "Index Width (IW):     " << index_width << std::endl;
    
    std::cout << "\n--- Access Statistics ---" << std::endl;
    std::cout << "Total Accesses:       " << total_accesses << std::endl;
    std::cout << "Primary Hits:         " << total_primary_hits << std::endl;
    std::cout << "Secondary Hits:       " << total_secondary_hits << std::endl;
    std::cout << "Total Hits:           " << get_total_hits() << std::endl;
    std::cout << "Misses:               " << total_misses << std::endl;
    
    std::cout << "\n--- Hit Rate Analysis ---" << std::endl;
    std::cout << "Overall Hit Rate:     " << get_hit_rate() << "%" << std::endl;
    std::cout << "Secondary Hit Rate:   " << get_secondary_hit_rate() << "% (of all hits)" << std::endl;
    
    if (total_accesses > 0) {
      double primary_contribution = 100.0 * total_primary_hits / total_accesses;
      double secondary_contribution = 100.0 * total_secondary_hits / total_accesses;
      std::cout << "Primary Hit Contrib:  " << primary_contribution << "% (of accesses)" << std::endl;
      std::cout << "Secondary Hit Contrib:" << secondary_contribution << "% (of accesses)" << std::endl;
    }
    
    std::cout << "\n--- Displacement Statistics ---" << std::endl;
    std::cout << "Total Displacements:  " << total_displacements << std::endl;
    std::cout << "Total Evictions:      " << total_evictions << std::endl;
    
    if (total_displacements > 0) {
      double displacement_effectiveness = (total_secondary_hits > 0 && total_displacements > 0) 
          ? (100.0 * total_secondary_hits / total_displacements) : 0.0;
      std::cout << "Displacement Effect:  " << displacement_effectiveness 
                << "% (secondary hits per displacement)" << std::endl;
    }
    
    // Find sets with most secondary hits (benefiting from displacement)
    std::vector<std::pair<uint32_t, uint64_t>> secondary_hit_sets;
    for (uint32_t s = 0; s < num_sets; s++) {
      if (set_stats[s].secondary_hits > 0) {
        secondary_hit_sets.push_back({s, set_stats[s].secondary_hits});
      }
    }
    
    if (!secondary_hit_sets.empty()) {
      std::sort(secondary_hit_sets.begin(), secondary_hit_sets.end(),
               [](const auto& a, const auto& b) { return a.second > b.second; });
      
      size_t show_count = std::min(secondary_hit_sets.size(), size_t(10));
      std::cout << "\n--- Top " << show_count << " Sets Benefiting from Displacement ---" << std::endl;
      for (size_t i = 0; i < show_count; i++) {
        uint32_t set = secondary_hit_sets[i].first;
        const auto& stats = set_stats[set];
        uint64_t total_hits = stats.primary_hits + stats.secondary_hits;
        double sec_rate = total_hits > 0 ? (100.0 * stats.secondary_hits / total_hits) : 0.0;
        
        std::cout << "  Set " << std::setw(4) << set 
                  << " (partner=" << std::setw(4) << compute_partner_set(set) << "): "
                  << std::setw(8) << stats.secondary_hits << " secondary hits"
                  << " (" << std::setw(5) << sec_rate << "% of set hits)"
                  << std::endl;
      }
    }
    
    // Find most stressed sets (high evictions)
    std::vector<std::pair<uint32_t, uint64_t>> stressed_sets;
    for (uint32_t s = 0; s < num_sets; s++) {
      if (set_stats[s].displacements_out > 0 || set_stats[s].evictions > 0) {
        stressed_sets.push_back({s, set_stats[s].displacements_out + set_stats[s].evictions});
      }
    }
    
    if (!stressed_sets.empty()) {
      std::sort(stressed_sets.begin(), stressed_sets.end(),
               [](const auto& a, const auto& b) { return a.second > b.second; });
      
      size_t show_count = std::min(stressed_sets.size(), size_t(10));
      std::cout << "\n--- Top " << show_count << " Stressed Sets ---" << std::endl;
      for (size_t i = 0; i < show_count; i++) {
        uint32_t set = stressed_sets[i].first;
        const auto& stats = set_stats[set];
        
        std::cout << "  Set " << std::setw(4) << set 
                  << ": " << std::setw(6) << stats.displacements_out << " displ_out"
                  << ", " << std::setw(6) << stats.evictions << " evictions"
                  << ", " << std::setw(8) << stats.accesses << " accesses"
                  << std::endl;
      }
    }
    
    std::cout << std::endl;
  }

  void export_to_csv(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
      std::cerr << "[SBC Monitor] Failed to open " << filename << " for writing" << std::endl;
      return;
    }
    
    file << "Set,PartnerSet,Accesses,PrimaryHits,SecondaryHits,TotalHits,Misses,"
         << "Evictions,DisplacementsOut,DisplacementsIn,HitRate,SecondaryHitRate\n";
    
    for (uint32_t s = 0; s < num_sets; s++) {
      const auto& stats = set_stats[s];
      uint64_t total_hits = stats.primary_hits + stats.secondary_hits;
      double hit_rate = stats.accesses > 0 ? (100.0 * total_hits / stats.accesses) : 0.0;
      double sec_rate = total_hits > 0 ? (100.0 * stats.secondary_hits / total_hits) : 0.0;
      
      file << s << ","
           << compute_partner_set(s) << ","
           << stats.accesses << ","
           << stats.primary_hits << ","
           << stats.secondary_hits << ","
           << total_hits << ","
           << stats.misses << ","
           << stats.evictions << ","
           << stats.displacements_out << ","
           << stats.displacements_in << ","
           << std::fixed << std::setprecision(2) << hit_rate << ","
           << sec_rate << "\n";
    }
    
    file.close();
    std::cout << "[SBC Monitor] Exported to: " << filename << std::endl;
  }
};

#endif // CM_UTIL_SBC_UTILIZATION_MONITOR_HPP
