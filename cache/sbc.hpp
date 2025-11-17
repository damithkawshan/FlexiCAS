#ifndef CM_CACHE_SBC_HPP
#define CM_CACHE_SBC_HPP

#include <vector>
#include <algorithm>
#include <limits>
#include <fstream>
#include <iomanip>
#include <mutex>
#include "cache/cache.hpp"
#include "cache/metadata.hpp"
#include "cache/replace.hpp"
#include "cache/index.hpp"

/**
 * Set Balancing Cache (SBC) Implementation
 * 
 * Based on "Adaptive Line Placement with the Set Balancing Cache"
 * by Dyer Rolán, Basilio B. Fraguela, Ramón Doallo
 * 
 * SBC reduces miss rates by dynamically redistributing cache lines
 * between over-stressed sets and underutilized sets.
 * 
 * Two variants:
 * - Static SBC (SSBC): Each set paired with fixed partner
 * - Dynamic SBC (DSBC): Saturated sets use least saturated available set
 */

//////////////// SBC Metadata with Saturation Tracking ////////////////////

template <int AW, int IW, int TOfst, typename MT>
  requires C_DERIVE<MT, CMMetadataBase>
class MetadataSBC : public MetadataMixer<AW, IW, TOfst, MT>
{
protected:
  using BaseT = MetadataMixer<AW, IW, TOfst, MT>;
  
  // SBC-specific fields
  bool displaced = false;      // Whether this line was displaced from another set
  uint32_t home_set = 0;       // Original set index (for displaced lines)

public:
  __always_inline void mark_displaced(uint32_t original_set) {
    displaced = true;
    home_set = original_set;
  }
  
  __always_inline void unmark_displaced() {
    displaced = false;
    home_set = 0;
  }
  
  __always_inline bool is_displaced() const { return displaced; }
  __always_inline uint32_t get_home_set() const { return home_set; }
  
  virtual void to_invalid() override {
    BaseT::to_invalid();
    unmark_displaced();
  }
  
  virtual void copy(const CMMetadataBase *m_meta) override {
    BaseT::copy(m_meta);
    auto meta = static_cast<const MetadataSBC *>(m_meta);
    displaced = meta->displaced;
    home_set = meta->home_set;
  }
};

// Convenient typedefs for common coherence protocols
template <int AW, int IW, int TOfst>
using MetadataSBCMSI = MetadataSBC<AW, IW, TOfst, MetadataMSIBroadcast<AW, IW, TOfst>>;

template <int AW, int IW, int TOfst>
using MetadataSBCMESI = MetadataSBC<AW, IW, TOfst, MetadataMESIDirectory<AW, IW, TOfst>>;

//////////////// SBC Replacement Policy ////////////////////

/**
 * SBC Replacement Policy
 * 
 * IW: index width
 * NW: number of ways per set
 * SATURATION_THRESHOLD: Number of active lines before set is "saturated"
 * IS_DYNAMIC: true for DSBC, false for SSBC
 * EF: empty first
 */
template<int IW, int NW, int SATURATION_THRESHOLD = NW, bool IS_DYNAMIC = true, bool EF = true, int SAT_MAX = 15>
class ReplaceSBC : public ReplaceFuncBase<EF>
{
  typedef ReplaceFuncBase<EF> RPT;
  
protected:
  using RPT::alloc_map;
  using RPT::used_map;
  using RPT::free_num;
  using RPT::free_map;
  
  static constexpr uint32_t nset = 1ul << IW;
  
  // Saturation tracking
  std::vector<uint32_t> saturation_counter;  // Current occupancy per set
  std::vector<bool> is_saturated;            // Quick saturation check
  
  // Static SBC: Fixed partner mapping (XOR with middle bit)
  std::vector<uint32_t> partner_set;
  
  // Dynamic SBC: Track global saturation for DSS
  std::vector<std::pair<uint32_t, uint32_t>> saturation_index; // (saturation, set_id)
  bool dss_needs_update = true;

  // Select victim using base LRU policy
  virtual uint32_t select(uint32_t s) override {
    for(uint32_t i=0; i<NW; i++)
      if(used_map[s][i] == 0) return i;
    return 0; // Fallback
  }
  
  /**
   * Destination Set Selector (DSS) for Dynamic SBC
   * Returns the least saturated set index
   */
  uint32_t select_destination_set(uint32_t exclude_set) {
    if constexpr (IS_DYNAMIC) {
      // Find least saturated set (excluding the source set)
      uint32_t min_saturation = std::numeric_limits<uint32_t>::max();
      uint32_t dest_set = exclude_set;
      
      for(uint32_t s = 0; s < nset; s++) {
        if(s != exclude_set && saturation_counter[s] < min_saturation) {
          min_saturation = saturation_counter[s];
          dest_set = s;
        }
      }
      
      return dest_set;
    } else {
      // Static SBC: Use fixed partner
      return partner_set[exclude_set];
    }
  }

public:
  ReplaceSBC() : RPT(nset, NW), 
                 saturation_counter(nset, 0),
                 is_saturated(nset, false),
                 partner_set(nset),
                 saturation_index(nset)
  {
    // Initialize used_map for LRU tracking
    for (auto &s: used_map) {
      s.resize(NW);
      for(uint32_t i=0; i<NW; i++) s[i] = i;
    }
    
    // Initialize static partner mapping (XOR with middle bit pattern)
    // This distributes partners across the cache
    if constexpr (!IS_DYNAMIC) {
      uint32_t xor_mask = 1ul << (IW / 2); // XOR with middle bit
      for(uint32_t s = 0; s < nset; s++) {
        partner_set[s] = s ^ xor_mask;
        //print partner set for 10,5,14 and 2
        if (s == 10 || s == 5 || s == 14 || s == 2) {
          std::cout << "Set " << s << " partner: " << partner_set[s] << std::endl;
        }
      }
    }
    
    // Initialize saturation index for DSS
    for(uint32_t s = 0; s < nset; s++) {
      saturation_index[s] = {0, s};
    }
  }
  
  /**
   * Check if a set is saturated
   */
  __always_inline bool check_saturated(uint32_t s) const {
    return saturation_counter[s] >= SATURATION_THRESHOLD;
  }
  
  /**
   * Get current saturation level of a set
   */
  __always_inline uint32_t get_saturation(uint32_t s) const {
    return saturation_counter[s];
  }
  
  /**
   * Check if displacement is needed for this set
   */
  bool needs_displacement(uint32_t s) const {
    return check_saturated(s) && (free_num[s] == 0);
  }
  
  /**
   * Get destination set for displacement
   */
  uint32_t get_displacement_destination(uint32_t source_set) {
    uint32_t dest = select_destination_set(source_set);
    
    // Only displace if destination has capacity
    if(free_num[dest] > 0 || saturation_counter[dest] < saturation_counter[source_set]) {
      return dest;
    }
    
    return source_set; // No displacement possible
  }
  
  /**
   * Standard cache access - update LRU and saturation
   */
  virtual void access(uint32_t s, uint32_t w, bool demand_acc, bool prefetch) override {
    // Update LRU information
    if((int32_t)w == alloc_map[s] || demand_acc) {
      auto prio = used_map[s][w];
      if(!prefetch) {
        for(uint32_t i=0; i<NW; i++) 
          if(used_map[s][i] > prio) 
            used_map[s][i]--;
        used_map[s][w] = NW-1;
      } else if(prio > 0) {
        for(uint32_t i=0; i<NW; i++) 
          if(used_map[s][i] < prio) 
            used_map[s][i]++;
        used_map[s][w] = 0;
      }
    }
    
    if((int32_t)w == alloc_map[s] && demand_acc) {
      // Clear allocation marker; saturation is updated on miss in CacheSBC::replace
      alloc_map[s] = -1;
    }
    
    RPT::delist_from_free(s, w, demand_acc);
  }
  
  /**
   * Handle invalidation - update saturation
   */
  virtual void invalid(uint32_t s, uint32_t w) override {
    RPT::invalid(s, w);
    // Saturation counters reflect hit/miss behavior, not occupancy; no change on invalidation
  }
  
  /**
   * Update saturation counter and tracking
   */
  void update_saturation(uint32_t s, int delta) {
    // Update counter with true saturating arithmetic on [0, SAT_MAX]
    uint32_t old_val = saturation_counter[s];
    uint32_t new_val = old_val;
    if(delta > 0) {
      uint64_t tmp = (uint64_t)old_val + (uint64_t)delta;
      new_val = (tmp > (uint64_t)SAT_MAX) ? (uint32_t)SAT_MAX : (uint32_t)tmp;
    } else if(delta < 0) {
      int64_t tmp = (int64_t)old_val + (int64_t)delta;
      new_val = (tmp < 0) ? 0u : (uint32_t)tmp;
    }
    saturation_counter[s] = new_val;
    
    // Update saturation flag
    is_saturated[s] = check_saturated(s);
    
    // Mark DSS for update in dynamic mode
    if constexpr (IS_DYNAMIC) {
      dss_needs_update = true;
    }
  }
  
  /**
   * Get statistics for monitoring
   */
  void get_stats(uint32_t *avg_saturation, uint32_t *max_saturation, 
                 uint32_t *num_saturated) const {
    uint64_t total = 0;
    uint32_t max_sat = 0;
    uint32_t saturated_count = 0;
    
    for(uint32_t s = 0; s < nset; s++) {
      total += saturation_counter[s];
      max_sat = std::max(max_sat, saturation_counter[s]);
      if(is_saturated[s]) saturated_count++;
    }
    
    *avg_saturation = total / nset;
    *max_saturation = max_sat;
    *num_saturated = saturated_count;
  }
};

// Type aliases for common configurations
template<int IW, int NW>
using ReplaceSSBC = ReplaceSBC<IW, NW, NW, false, true>; // Static SBC

template<int IW, int NW>
using ReplaceDSBC = ReplaceSBC<IW, NW, NW, true, true>;  // Dynamic SBC

template<int IW, int NW, int THRESH>
using ReplaceDSBCCustom = ReplaceSBC<IW, NW, THRESH, true, true>;  // Custom threshold

//////////////// SBC Logger ////////////////////

/**
 * Logger for SBC-specific events and state
 */
class SBCLogger {
private:
  std::ofstream log_file;
  std::mutex log_mutex;
  bool enabled;
  uint64_t event_counter = 0;

public:
  SBCLogger(const std::string& filename = "log_sbc.log", bool enable = true) 
    : enabled(enable) {
    if(enabled) {
      log_file.open(filename, std::ios::out | std::ios::trunc);
      if(log_file.is_open()) {
        // Write header
        log_file << "# SBC Cache Event Log\n";
        log_file << "# Format: EventID,Timestamp,Event,Address,Set,Way,Saturation,IsDisplaced,"
                 << "HomeSet,DestSet,SecondarySearch,DisplacementAttempt,Success,Details\n";
        log_file << std::hex << std::setfill('0');
      } else {
        enabled = false;
        std::cerr << "Warning: Could not open SBC log file: " << filename << std::endl;
      }
    }
  }

  ~SBCLogger() {
    if(log_file.is_open()) {
      log_file.close();
    }
  }

  void log_access(uint64_t addr, uint32_t set, uint32_t way, uint32_t saturation,
                  bool is_displaced, uint32_t home_set, bool hit, const std::string& details = "") {
    if(!enabled || !log_file.is_open()) return;
    
    std::lock_guard<std::mutex> lock(log_mutex);
    log_file << std::dec << event_counter++ << ","
             << std::dec << event_counter << ","  // timestamp placeholder
             << (hit ? "HIT" : "MISS") << ","
             << "0x" << std::hex << std::setw(16) << addr << ","
             << std::dec << set << ","
             << std::dec << way << ","
             << std::dec << saturation << ","
             << (is_displaced ? "1" : "0") << ","
             << std::dec << home_set << ","
             << "-,-,0,0,"  // DestSet, SecondarySearch, DisplacementAttempt, Success
             << details << "\n";
  }

  void log_displacement(uint64_t victim_addr, uint32_t src_set, uint32_t src_way,
                       uint32_t src_saturation, uint32_t dest_set, uint32_t dest_way,
                       uint32_t dest_saturation, bool secondary_search, bool success,
                       const std::string& details = "") {
    if(!enabled || !log_file.is_open()) return;
    
    std::lock_guard<std::mutex> lock(log_mutex);
    log_file << std::dec << event_counter++ << ","
             << std::dec << event_counter << ","
             << "DISPLACE" << ","
             << "0x" << std::hex << std::setw(16) << victim_addr << ","
             << std::dec << src_set << ","
             << std::dec << src_way << ","
             << std::dec << src_saturation << ","
             << "0,"  // Not yet displaced (being displaced now)
             << std::dec << src_set << ","  // Home set is source
             << std::dec << dest_set << ","
             << (secondary_search ? "1" : "0") << ","
             << "1,"  // Displacement attempt
             << (success ? "1" : "0") << ","
             << details << "\n";
    
    if(success) {
      log_file << std::dec << event_counter++ << ","
               << std::dec << event_counter << ","
               << "DISPLACED_TO" << ","
               << "0x" << std::hex << std::setw(16) << victim_addr << ","
               << std::dec << dest_set << ","
               << std::dec << dest_way << ","
               << std::dec << dest_saturation << ","
               << "1,"  // Now displaced
               << std::dec << src_set << ","  // Home set
               << "-,-,0,0,"
               << "Relocated from set " << src_set << "\n";
    }
  }

  void log_allocation(uint64_t addr, uint32_t set, uint32_t way, uint32_t saturation,
                     bool after_displacement, const std::string& details = "") {
    if(!enabled || !log_file.is_open()) return;
    
    std::lock_guard<std::mutex> lock(log_mutex);
    log_file << std::dec << event_counter++ << ","
             << std::dec << event_counter << ","
             << "ALLOCATE" << ","
             << "0x" << std::hex << std::setw(16) << addr << ","
             << std::dec << set << ","
             << std::dec << way << ","
             << std::dec << saturation << ","
             << "0,"  // New allocation, not displaced
             << std::dec << set << ","  // Home set is current set
             << "-,"
             << (after_displacement ? "1" : "0") << ","
             << "0,0,"
             << (after_displacement ? "After displacement" : "Normal allocation")
             << (details.empty() ? "" : "; ") << details << "\n";
  }

  void log_eviction(uint64_t addr, uint32_t set, uint32_t way, uint32_t saturation,
                   bool was_displaced, uint32_t home_set, const std::string& details = "") {
    if(!enabled || !log_file.is_open()) return;
    
    std::lock_guard<std::mutex> lock(log_mutex);
    log_file << std::dec << event_counter++ << ","
             << std::dec << event_counter << ","
             << "EVICT" << ","
             << "0x" << std::hex << std::setw(16) << addr << ","
             << std::dec << set << ","
             << std::dec << way << ","
             << std::dec << saturation << ","
             << (was_displaced ? "1" : "0") << ","
             << std::dec << home_set << ","
             << "-,-,0,0,"
             << (was_displaced ? "Evicting displaced line" : "Evicting normal line")
             << (details.empty() ? "" : "; ") << details << "\n";
  }

  void log_saturation_change(uint32_t set, uint32_t old_sat, uint32_t new_sat,
                             const std::string& reason = "") {
    if(!enabled || !log_file.is_open()) return;
    
    std::lock_guard<std::mutex> lock(log_mutex);
    log_file << std::dec << event_counter++ << ","
             << std::dec << event_counter << ","
             << "SAT_CHANGE" << ","
             << "-,"  // No specific address
             << std::dec << set << ","
             << "-,"  // No specific way
             << std::dec << new_sat << ","
             << "0,-,-,-,0,0,"
             << "Set " << set << " saturation: " << old_sat << " -> " << new_sat
             << (reason.empty() ? "" : " (") << reason << (reason.empty() ? "" : ")") << "\n";
  }

  void flush() {
    if(log_file.is_open()) {
      log_file.flush();
    }
  }

  bool is_enabled() const { return enabled; }
};

//////////////// Set Balancing Cache ////////////////////

/**
 * Set Balancing Cache implementation
 * Extends CacheNorm with displacement logic
 */
template<int IW, int NW, typename MT, typename DT, typename IDX, typename RPC, typename DLY,
         bool EnMon, bool EnMT = false, int MSHR = 4>
  requires C_DERIVE<MT, CMMetadataBase> && C_DERIVE_OR_VOID<DT, CMDataBase> &&
           C_DERIVE<IDX, IndexFuncBase> && C_DERIVE_OR_VOID<DLY, DelayBase>
class CacheSBC : public CacheNorm<IW, NW, MT, DT, IDX, RPC, DLY, EnMon, EnMT, MSHR>
{
protected:
  typedef CacheNorm<IW, NW, MT, DT, IDX, RPC, DLY, EnMon, EnMT, MSHR> CacheT;
  using CacheT::arrays;
  using CacheT::replacer;
  
  // SBC statistics
  uint64_t displacement_attempts = 0;
  uint64_t successful_displacements = 0;
  uint64_t displacement_prevented_misses = 0;
  
  // SBC logger
  SBCLogger* sbc_logger = nullptr;

public:
  CacheSBC(std::string name, bool enable_logging = true) : CacheT(name) {
    if(enable_logging) {
      std::string log_filename = "log_sbc_" + name + ".log";
      sbc_logger = new SBCLogger(log_filename, true);
    }
  }
  
  virtual ~CacheSBC() override {
    if(displacement_attempts > 0) {
      std::cout << "[SBC Stats] " << CacheT::get_name() << ":" << std::endl;
      std::cout << "  Displacement attempts: " << displacement_attempts << std::endl;
      std::cout << "  Successful displacements: " << successful_displacements << std::endl;
      std::cout << "  Prevented misses: " << displacement_prevented_misses << std::endl;
      std::cout << "  Success rate: " 
                << (100.0 * successful_displacements / displacement_attempts) << "%" << std::endl;
    }
    if(sbc_logger) {
      sbc_logger->flush();
      delete sbc_logger;
    }
  }
  
  /**
   * Enhanced replace with displacement support
   */
  virtual bool replace(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w, 
                      uint16_t prio, unsigned int genre = 0) override {
    *ai = 0;
    *s = CacheT::indexer.index(addr, *ai);
    
    // Miss at set *s: increment saturation (saturating arithmetic)
    replacer[0].update_saturation(*s, +1);

    uint32_t src_saturation = replacer[0].get_saturation(*s);
    
    if(EnMT) {
      this->set_mt_state(*ai, *s, prio);
      if(CacheBase::hit(addr)) {
        this->reset_mt_state(*ai, *s, prio);
        return false;
      }
    }
    
    // Check if displacement is needed
    if(replacer[0].needs_displacement(*s)) {
      displacement_attempts++;
      
      // Try to displace to another set
      uint32_t dest_set = replacer[0].get_displacement_destination(*s);
      bool secondary_search = (dest_set != *s);
      
      if(dest_set != *s && replacer[0].get_free_num(dest_set) > 0) {
        // Displacement is possible
        successful_displacements++;
        
        // Select victim from source set
        replacer[0].replace(*s, w);
        
        // Get the victim line
        auto victim_meta = static_cast<MT*>(arrays[*ai]->get_meta(*s, *w));
        
        // If victim is valid, try to displace it
        if(victim_meta->is_valid()) {
          uint64_t victim_addr = victim_meta->addr(*s);
          uint32_t dest_way;
          uint32_t dest_saturation = replacer[0].get_saturation(dest_set);
          
          // Check if victim was already displaced
          bool victim_was_displaced = false;
          uint32_t victim_home_set = *s;
          if constexpr (requires { victim_meta->is_displaced(); }) {
            victim_was_displaced = victim_meta->is_displaced();
            if(victim_was_displaced) {
              victim_home_set = victim_meta->get_home_set();
            }
          }
          
          // Log eviction of victim
          if(sbc_logger) {
            sbc_logger->log_eviction(victim_addr, *s, *w, src_saturation,
                                    victim_was_displaced, victim_home_set,
                                    "Victim for displacement");
          }
          
          // Allocate in destination set
          replacer[0].replace(dest_set, &dest_way);
          auto dest_meta = static_cast<MT*>(arrays[*ai]->get_meta(dest_set, dest_way));
          
          // Move the victim line to destination
          if constexpr (!C_VOID<DT>) {
            auto victim_data = arrays[*ai]->get_data(*s, *w);
            auto dest_data = arrays[*ai]->get_data(dest_set, dest_way);
            CacheT::relocate(victim_addr, victim_meta, dest_meta, victim_data, dest_data);
          } else {
            CacheT::relocate(victim_addr, victim_meta, dest_meta);
          }
          
          // Mark as displaced (if metadata supports it)
          if constexpr (requires { dest_meta->mark_displaced(*s); }) {
            dest_meta->mark_displaced(*s);
          }
          
          // Log displacement
          if(sbc_logger) {
            std::string details = "DSBC: dest_sat=" + std::to_string(dest_saturation) +
                                " src_sat=" + std::to_string(src_saturation);
            sbc_logger->log_displacement(victim_addr, *s, *w, src_saturation,
                                        dest_set, dest_way, dest_saturation,
                                        secondary_search, true, details);
          }
          
          displacement_prevented_misses++;
        }
        
        // Log allocation of new line after displacement
        if(sbc_logger) {
          sbc_logger->log_allocation(addr, *s, *w, src_saturation, true,
                                    "After successful displacement");
        }
        
        // Now allocate the new line in the original set
        return true;
      } else {
        // Displacement not possible - log failed attempt
        if(sbc_logger) {
          std::string reason = (dest_set == *s) ? "No alternative set found" : 
                              "Destination set full";
          sbc_logger->log_displacement(addr, *s, 0, src_saturation,
                                      dest_set, 0, replacer[0].get_saturation(dest_set),
                                      secondary_search, false, reason);
        }
      }
    }
    
    // Normal replacement without displacement
    replacer[0].replace(*s, w);
    
    // Check if we're evicting a valid line
    auto evict_meta = static_cast<MT*>(arrays[*ai]->get_meta(*s, *w));
    if(evict_meta->is_valid()) {
      uint64_t evict_addr = evict_meta->addr(*s);
      bool was_displaced = false;
      uint32_t home_set = *s;
      
      if constexpr (requires { evict_meta->is_displaced(); }) {
        was_displaced = evict_meta->is_displaced();
        if(was_displaced) {
          home_set = evict_meta->get_home_set();
        }
      }
      
      if(sbc_logger) {
        sbc_logger->log_eviction(evict_addr, *s, *w, src_saturation,
                                was_displaced, home_set, "Normal eviction");
      }
    }
    
    // Log normal allocation
    if(sbc_logger) {
      sbc_logger->log_allocation(addr, *s, *w, src_saturation, false,
                                "Normal allocation");
    }
    
    return true;
  }
  
  /**
   * Override hit to add logging
   */
  virtual bool hit(uint64_t addr, uint32_t *ai, uint32_t *s, uint32_t *w,
                   uint16_t prio, bool check_and_set) override {
    bool result = CacheT::hit(addr, ai, s, w, prio, check_and_set);
    
    // Hit in set *s: decrement saturation (saturating arithmetic)
    if(result) {
      replacer[0].update_saturation(*s, -1);
    }

    if(result && sbc_logger) {
      auto meta = static_cast<MT*>(arrays[*ai]->get_meta(*s, *w));
      uint32_t saturation = replacer[0].get_saturation(*s);
      bool is_displaced = false;
      uint32_t home_set = *s;
      
      if constexpr (requires { meta->is_displaced(); }) {
        is_displaced = meta->is_displaced();
        if(is_displaced) {
          home_set = meta->get_home_set();
        }
      }
      
      std::string details = "State=" + meta->to_string();
      sbc_logger->log_access(addr, *s, *w, saturation, is_displaced, home_set, true, details);
    }
    
    return result;
  }
  
  /**
   * Get SBC-specific statistics
   */
  void get_sbc_stats(uint64_t *displacements, uint64_t *prevented_misses) const {
    *displacements = successful_displacements;
    *prevented_misses = displacement_prevented_misses;
  }
  
  /**
   * Get saturation statistics
   */
  void get_saturation_stats(uint32_t *avg, uint32_t *max, uint32_t *num_sat) const {
    replacer[0].get_stats(avg, max, num_sat);
  }
};

// Type aliases for convenience
template<int IW, int NW, typename MT, typename DT, typename IDX, typename DLY, bool EnMon, bool EnMT = false, int MSHR = 4>
using CacheSSBC = CacheSBC<IW, NW, MT, DT, IDX, ReplaceSSBC<IW, NW>, DLY, EnMon, EnMT, MSHR>;

template<int IW, int NW, typename MT, typename DT, typename IDX, typename DLY, bool EnMon, bool EnMT = false, int MSHR = 4>
using CacheDSBC = CacheSBC<IW, NW, MT, DT, IDX, ReplaceDSBC<IW, NW>, DLY, EnMon, EnMT, MSHR>;

#endif // CM_CACHE_SBC_HPP
