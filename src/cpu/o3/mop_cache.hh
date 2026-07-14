/*
 * Copyright (c) 2026 The Regents of The University of California
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_O3_MOP_CACHE_HH__
#define __CPU_O3_MOP_CACHE_HH__

#include <cstddef>
#include <cstdint>
#include <vector>

#include "base/types.hh"
#include "cpu/static_inst.hh"

namespace gem5
{

namespace o3
{

/**
 * All architectural state which can affect instruction translation or
 * decoding.  Fetch constructs this key from untruncated CSR values; keeping
 * the fields separate makes accidental hash aliases impossible.
 */
struct MopContext
{
    ThreadID tid = InvalidThreadID;
    uint64_t satp = 0;
    uint64_t vsatp = 0;
    uint64_t hgatp = 0;
    uint8_t privilege = 0;
    bool virt = false;
    bool vtypeReady = true;
    uint64_t vtype = 0;

    bool operator==(const MopContext &other) const;
    bool operator!=(const MopContext &other) const
    {
        return !(*this == other);
    }
};

/** A single-PC decoded instruction cache entry. */
struct MopEntry
{
    Addr pc = 0;
    Addr nextPC = 0;
    StaticInstPtr staticInst;
    bool compressed = false;
    MopContext context;
    bool valid = false;
    uint64_t lruSequence = 0;
};

/**
 * A bounded, set-associative cache of decoded instructions.
 *
 * This class deliberately does not model lookup latency.  A caller consumes
 * a read-port token here and holds the returned bundle in its own pipeline for
 * the configured number of cycles.
 */
class MopCache
{
  public:
    struct Config
    {
        size_t entries = 1024;
        size_t ways = 4;
        size_t lookupWidth = 8;
        size_t readPorts = 1;
        size_t fillWidth = 8;
    };

    struct LookupResult
    {
        bool portAvailable = false;
        std::vector<MopEntry> entries;
        Addr nextPC = 0;
        Addr missPC = 0;
        bool missed = false;
        bool reachedEnd = false;
        bool widthLimited = false;
    };

    enum class FillResult
    {
        Filled,
        Updated,
        Evicted,
        NoBandwidth,
    };

    explicit MopCache(const Config &config);

    /** Replenish the shared pipelined-read and fill tokens for a new cycle. */
    void beginCycle();

    /**
     * Look up a sequential bundle beginning at start_pc.  A non-zero end_pc
     * is an exclusive predicted-block boundary.  The lookup consumes one read
     * port even when its first entry misses.
     */
    LookupResult lookupBundle(Addr start_pc, const MopContext &context,
                              Addr end_pc = 0);

    /** Insert or refresh one decoded instruction, consuming one fill token. */
    FillResult fill(Addr pc, Addr next_pc, const StaticInstPtr &static_inst,
                    bool compressed, const MopContext &context);

    /** Invalidate all entries and return the number which had been valid. */
    size_t invalidate();

    size_t size() const { return validEntries; }
    size_t capacity() const { return config.entries; }
    size_t numWays() const { return config.ways; }
    size_t numSets() const { return sets.size(); }
    size_t lookupWidth() const { return config.lookupWidth; }
    size_t readPortsAvailable() const { return readTokens; }
    size_t fillSlotsAvailable() const { return fillTokens; }

  private:
    using Set = std::vector<MopEntry>;

    size_t setIndex(Addr pc) const;
    MopEntry *find(Addr pc, const MopContext &context);
    void touch(MopEntry &entry);

    const Config config;
    std::vector<Set> sets;
    size_t readTokens;
    size_t fillTokens;
    size_t validEntries = 0;
    uint64_t lruSequence = 0;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_MOP_CACHE_HH__
