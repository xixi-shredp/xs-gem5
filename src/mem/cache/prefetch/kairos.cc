/**
 * Kairos (croage) temporal prefetcher implementation. See kairos.hh.
 */

#include "mem/cache/prefetch/kairos.hh"

#include <algorithm>
#include <cmath>

#include "debug/HWPrefetch.hh"
#include "params/KairosPrefetcher.hh"

namespace gem5
{

namespace prefetch
{

// ======================================================================
// KeyDetect
// ======================================================================

Kairos::KeyDetect::KeyDetect(std::size_t cap) : capacity(cap)
{}

bool
Kairos::KeyDetect::update(Addr pc, bool cache_hit)
{
    auto it = lookup.find(pc);
    if (it == lookup.end()) {
        // Insert as new FIFO head, evict from tail if full.
        fifo.push_front({pc, 1, cache_hit ? 0u : 1u});
        lookup[pc] = fifo.begin();
        if (!cache_hit) {
            total_miss++;
        }
        if (fifo.size() > capacity) {
            lookup.erase(fifo.back().pc);
            fifo.pop_back();
        }
        // New entry with one miss does not immediately qualify.
        bool qualifies = false;
        if (!cache_hit) {
            // miss=1, issue=1 → miss*2 > issue → key IP.
            qualifies = true;
        }
        // fall-through to periodic reset.
        if (total_miss > 511) {
            for (auto &e : fifo) {
                e.issue_count = 0;
                e.miss_count = 0;
            }
            total_miss = 0;
        }
        return qualifies;
    }

    auto &entry = *(it->second);
    entry.issue_count++;
    if (!cache_hit) {
        entry.miss_count++;
        total_miss++;
    }
    bool qualifies = false;
    if (entry.miss_count * 2 > entry.issue_count) {
        qualifies = true;
    } else if (entry.miss_count * 8 > total_miss) {
        qualifies = true;
    }

    if (total_miss > 511) {
        for (auto &e : fifo) {
            e.issue_count = 0;
            e.miss_count = 0;
        }
        total_miss = 0;
    }
    return qualifies;
}

// ======================================================================
// TrainUnit
// ======================================================================

Kairos::TrainUnit::TrainUnit(std::size_t cap) : capacity(cap)
{}

Kairos::TUEntry *
Kairos::TrainUnit::get(Addr pc)
{
    auto it = lookup.find(pc);
    if (it == lookup.end()) {
        return nullptr;
    }
    lru.splice(lru.begin(), lru, it->second);
    return &(*lru.begin());
}

Kairos::TUEntry *
Kairos::TrainUnit::findNoTouch(Addr pc)
{
    auto it = lookup.find(pc);
    if (it == lookup.end()) {
        return nullptr;
    }
    return &(*it->second);
}

void
Kairos::TrainUnit::insert(Addr pc, const TUEntry &entry)
{
    auto it = lookup.find(pc);
    if (it != lookup.end()) {
        *(it->second) = entry;
        lru.splice(lru.begin(), lru, it->second);
        return;
    }
    lru.push_front(entry);
    lookup[pc] = lru.begin();
    if (lru.size() > capacity) {
        lookup.erase(lru.back().pc);
        lru.pop_back();
    }
}

// ======================================================================
// CroageRepl
// ======================================================================

Kairos::CroageRepl::CroageRepl(std::size_t cap) : capacity(cap)
{}

Addr *
Kairos::CroageRepl::get(Addr key)
{
    auto it = lookup.find(key);
    if (it == lookup.end()) {
        return nullptr;
    }
    if (it->second->rpl > 0) {
        it->second->rpl -= 1;
    }
    return &(it->second->value);
}

std::list<Kairos::MDNode>::iterator
Kairos::CroageRepl::popVictim()
{
    // Scan from tail forward. If rpl < max_rpl bump it and rotate to head.
    // Guaranteed to terminate after at most capacity * max_rpl passes.
    while (true) {
        if (list_.empty()) {
            return list_.end();
        }
        auto cand = std::prev(list_.end());
        if (cand->rpl >= max_rpl) {
            return cand;
        }
        cand->rpl += 1;
        list_.splice(list_.begin(), list_, cand);
    }
}

Addr
Kairos::CroageRepl::set(Addr key, Addr value, bool useful_tu)
{
    auto it = lookup.find(key);
    if (it != lookup.end()) {
        it->second->value = value;
        return 0;
    }
    Addr evicted = 0;
    if (list_.size() >= capacity) {
        auto victim = popVictim();
        if (victim != list_.end()) {
            evicted = victim->key;
            lookup.erase(victim->key);
            list_.erase(victim);
        }
    }
    MDNode node;
    node.key = key;
    node.value = value;
    node.rpl = useful_tu ? (max_rpl - 1) : max_rpl;
    list_.push_front(node);
    lookup[key] = list_.begin();
    return evicted;
}

// ======================================================================
// Kairos top-level
// ======================================================================

Kairos::KairosStats::KairosStats(statistics::Group *parent)
    : statistics::Group(parent, "kairos"),
      ADD_STAT(
          metadataHits, statistics::units::Count::get(),
          "Metadata cache hits (temporal chain lookups that found entry)"),
      ADD_STAT(metadataMisses, statistics::units::Count::get(),
               "Metadata cache misses"),
      ADD_STAT(increaseEvents, statistics::units::Count::get(),
               "PID decisions to grow the metadata partition"),
      ADD_STAT(decreaseEvents, statistics::units::Count::get(),
               "PID decisions to shrink the metadata partition"),
      ADD_STAT(maintainEvents, statistics::units::Count::get(),
               "PID decisions to keep the metadata partition"),
      ADD_STAT(keyIpPredicts, statistics::units::Count::get(),
               "Accesses from key IPs that triggered prediction"),
      ADD_STAT(chainPrefetches, statistics::units::Count::get(),
               "Prefetch addresses produced by metadata chain walks"),
      ADD_STAT(llcMetadataReads, statistics::units::Count::get(),
               "Metadata read accesses injected into the LLC"),
      ADD_STAT(llcMetadataWrites, statistics::units::Count::get(),
               "Metadata write accesses injected into the LLC")
{}

Kairos::Kairos(const KairosPrefetcherParams &p)
    : Queued(p),
      degreeMax(p.degree),
      htSets(p.ht_sets),
      htWaysInit(p.ht_ways_init),
      htWaysMin(p.ht_ways_min),
      htWaysMax(p.ht_ways_max),
      trackingWindow(p.tracking_window),
      ALPHA(p.alpha),
      BETA(p.beta),
      GAMMA(p.gamma),
      THETA_PLUS(p.theta_plus),
      THETA_MINUS(p.theta_minus),
      TAU(p.tau),
      enableLlcMetadata(p.enable_llc_metadata),
      metadataBaseAddr(p.metadata_base_addr),
      llcMetadataWaysInit(p.llc_metadata_ways_init),
      llcMetadataWaysMin(p.llc_metadata_ways_min),
      llcMetadataWaysMax(p.llc_metadata_ways_max),
      detect(p.kd_size),
      trainer(p.tu_size),
      currentWays(p.ht_ways_init),
      kairosStats(this)
{
    fatal_if(htSets == 0 || (htSets & (htSets - 1)),
             "Kairos ht_sets must be a power of 2, got %u", htSets);
    fatal_if(htWaysInit < htWaysMin || htWaysInit > htWaysMax,
             "Kairos ht_ways_init must lie in [ht_ways_min, ht_ways_max]");
    metadata.reserve(htSets);
    for (std::size_t i = 0; i < htSets; ++i) {
        metadata.emplace_back(currentWays);
    }

}

std::size_t
Kairos::setIndex(Addr line_addr) const
{ return (line_addr ^ (line_addr >> 13)) & (htSets - 1); }

Addr
Kairos::metadataAddrFor(Addr last_addr) const
{
    // Each shadow set maps to a distinct LLC line in a dedicated address
    // region. Multiple keys in one shadow set alias onto the same LLC
    // line, which is the intended behavior: capacity and set conflicts
    // on the LLC track the shadow structure.
    return metadataBaseAddr + (setIndex(last_addr) << lBlkSize);
}

void
Kairos::emitMetadataAccess(Addr last_addr, bool is_write)
{
    if (!enableLlcMetadata) {
        return;
    }
    (void)metadataAddrFor(last_addr);
    if (is_write) {
        kairosStats.llcMetadataWrites++;
    } else {
        kairosStats.llcMetadataReads++;
    }
}

void
Kairos::temUp(Addr last_addr, Addr cur_addr, Addr pc, bool useful_tu)
{
    auto &set = metadata[setIndex(last_addr)];
    Addr pop_addr = set.set(last_addr, cur_addr, useful_tu);
    inMetadata[last_addr] = false;
    if (pop_addr != 0) {
        auto cam_it = cam.find(pop_addr);
        if (cam_it != cam.end()) {
            Addr evicted_pc = cam_it->second;
            TUEntry *tu = trainer.findNoTouch(evicted_pc);
            if (tu != nullptr) {
                if (useful_tu) {
                    tu->useful_evict++;
                }
                tu->total_evict++;
            }
            cam.erase(cam_it);
        }
        inMetadata.erase(pop_addr);
    }
    cam[last_addr] = pc;
    // Model the LLC write of the updated metadata entry.
    emitMetadataAccess(last_addr, /*is_write=*/true);
}

std::vector<Addr>
Kairos::predict(Addr line_addr)
{
    std::vector<Addr> candidates;
    Addr cur = line_addr;
    for (unsigned d = 0; d < degreeMax; ++d) {
        // Every hop corresponds to an LLC metadata read in the Kairos paper.
        emitMetadataAccess(cur, /*is_write=*/false);
        auto &set = metadata[setIndex(cur)];
        Addr *next = set.get(cur);
        if (next == nullptr) {
            if (d == 0) {
                kairosStats.metadataMisses++;
            }
            break;
        }
        kairosStats.metadataHits++;
        inMetadata[cur] = true;
        candidates.push_back(*next);
        cur = *next;
    }
    return candidates;
}

void
Kairos::train(Addr pc, Addr line_addr, bool cache_hit, bool useful_prefetch)
{
    if (!detect.update(pc, cache_hit)) {
        return;
    }

    TUEntry *tu = trainer.get(pc);
    if (tu == nullptr) {
        TUEntry fresh;
        fresh.pc = pc;
        fresh.cur_addr = line_addr;
        trainer.insert(pc, fresh);
        return;
    }

    if (!cache_hit || useful_prefetch) {
        Addr last = tu->cur_addr;
        tu->last_addr = last;
        tu->cur_addr = line_addr;

        if (tu->total_evict == EVICT_CLASSIFY_THRESHOLD) {
            tu->useful_tu =
                (static_cast<uint32_t>(tu->useful_evict) * USEFUL_DEN >
                 static_cast<uint32_t>(tu->total_evict) * USEFUL_NUM);
            tu->useful_evict = 0;
            tu->total_evict = 0;
        }
        temUp(last, line_addr, pc, tu->useful_tu);
    }
}

void
Kairos::resizeMetadata(int8_t direction)
{
    if (direction > 0) {
        kairosStats.increaseEvents++;
    } else if (direction < 0) {
        kairosStats.decreaseEvents++;
    } else {
        kairosStats.maintainEvents++;
    }

    std::size_t new_size = currentWays;
    if (direction > 0 && currentWays < htWaysMax) {
        new_size = std::min<std::size_t>(currentWays + WAY_CHUNK, htWaysMax);
    } else if (direction < 0 && currentWays > htWaysMin) {
        new_size = (currentWays > htWaysMin + WAY_CHUNK)
                       ? currentWays - WAY_CHUNK
                       : htWaysMin;
    } else {
        return;
    }
    if (new_size == currentWays) {
        return;
    }

    // Rebuild the shadow metadata at the new capacity.
    std::vector<CroageRepl> fresh;
    fresh.reserve(htSets);
    for (std::size_t i = 0; i < htSets; ++i) {
        fresh.emplace_back(new_size);
    }
    for (std::size_t i = 0; i < htSets; ++i) {
        for (const auto &node : metadata[i].nodes()) {
            if (fresh[i].size() >= new_size) {
                break;
            }
            fresh[i].set(node.key, node.value, false);
        }
    }
    metadata = std::move(fresh);
    currentWays = new_size;

}

void
Kairos::evaluateWindow()
{
    double miss_rate = static_cast<double>(missCount) / trackingWindow;
    double utility = 0.0;
    if (missCount > 0) {
        utility = static_cast<double>(prefetchHitCount) / missCount;
    }

    double delta_miss = 0.0;
    if (currentWindow > 0 && accessCount > 0) {
        delta_miss =
            static_cast<double>(static_cast<int64_t>(missCount) -
                                static_cast<int64_t>(previousMissCount)) /
            accessCount;
    }

    smoothedMissRate =
        EMA_ALPHA * miss_rate + (1.0 - EMA_ALPHA) * smoothedMissRate;

    double delta_k = ALPHA * utility + BETA * delta_miss +
                     GAMMA * (delta_miss - previousDeltaMiss);

    int8_t decision = 0;
    double utility_share =
        (accessCount > 0) ? static_cast<double>(prefetchHitCount) / accessCount
                          : 0.0;
    double way_share = static_cast<double>(currentWays) /
                       static_cast<double>(htWaysMax + WAY_CHUNK);
    if (delta_k > THETA_PLUS && utility_share > way_share) {
        decision = 1;
    } else if (delta_k < THETA_MINUS || miss_rate > TAU * previousMissRate) {
        decision = -1;
    }

    if (currentWindow > 0) {
        double rel_improvement = previousMissRate - miss_rate;
        if (rel_improvement < IMPROVEMENT_THRESHOLD) {
            noImprovementCount++;
            if (noImprovementCount >= MAX_NO_IMPROVEMENT_WINDOWS) {
                decision = 0;
            }
        } else {
            noImprovementCount = 0;
        }
    }

    resizeMetadata(decision);

    previousMissCount = missCount;
    previousDeltaMiss = delta_miss;
    previousMissRate = miss_rate;

    missCount = 0;
    prefetchHitCount = 0;
    currentWindow++;
}

void
Kairos::calculatePrefetch(const PrefetchInfo &pfi,
                          std::vector<AddrPriority> &addresses)
{
    if (!pfi.hasPC()) {
        return;
    }
    if (pfi.isWrite()) {
        // ChampSim's croage.cc only acts on LOAD / RFO accesses.  gem5
        // classifies RFOs as writes too (they hit calculatePrefetch via
        // onWrite); we keep both paths to mirror ChampSim.
    }

    Addr line_addr = blockIndex(pfi.getAddr());
    Addr pc = pfi.getPC();
    bool cache_miss = pfi.isCacheMiss();
    bool useful_prefetch = !cache_miss && pfi.isPfFirstHit();

    accessCount++;
    if (cache_miss) {
        missCount++;
    }
    if (useful_prefetch) {
        prefetchHitCount++;
    }

    if (accessCount % trackingWindow == 0) {
        evaluateWindow();
    }

    // Prediction must happen before training to match ChampSim ordering.
    std::vector<Addr> candidates = predict(line_addr);

    train(pc, line_addr, !cache_miss, useful_prefetch);

    if (!candidates.empty()) {
        kairosStats.keyIpPredicts++;
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());

    int32_t prio = 0;
    for (Addr line : candidates) {
        if (line == 0) {
            continue;
        }
        Addr pf_addr = line << lBlkSize;
        addresses.push_back(AddrPriority(pf_addr, prio++, PrefetchSourceType::Kairos));
        kairosStats.chainPrefetches++;
    }
}

} // namespace prefetch
} // namespace gem5
