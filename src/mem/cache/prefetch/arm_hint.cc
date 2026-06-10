/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
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

#include "mem/cache/prefetch/arm_hint.hh"

#include <algorithm>
#include <limits>

#include "params/ARMHintPrefetcher.hh"
#include "sim/core.hh"
#include "sim/system.hh"

namespace gem5
{

namespace prefetch
{

ARMHintPrefetcher::ARMHintPrefetcher(const ARMHintPrefetcherParams &p)
    : Queued(p),
      sourceTableEntries(p.source_table_entries),
      recentSourceEntries(p.recent_source_entries),
      pendingEntries(p.pending_entries),
      strideConfidenceThreshold(p.stride_confidence_threshold),
      indirectConfidenceThreshold(p.indirect_confidence_threshold),
      degree(p.degree),
      lookahead(p.lookahead),
      addressIndicatingBytes(p.address_indicating_bytes),
      sourceElementBytes(p.source_element_bytes),
      sourceElementStride(p.source_element_stride),
      maxSourceElements(p.max_source_elements),
      signedIndex(p.signed_index),
      minCandidateAddress(p.min_candidate_address),
      maxCandidateAddress(p.max_candidate_address),
      targetAlignment(p.target_alignment),
      shiftValues(p.shift_values),
      requireIndirectHint(p.require_indirect_hint),
      hintPCs(p.hint_pcs),
      useRequestorId(p.use_requestor_id),
      enableDirectPointer(p.enable_direct_pointer),
      enableStaticOffset(p.enable_static_offset),
      staticBase(p.static_base),
      staticShift(p.static_shift),
      enableProcessorHintTarget(p.enable_processor_hint_target),
      processorHintBase(p.processor_hint_base),
      processorHintShift(p.processor_hint_shift),
      validateCandidateAddresses(p.validate_candidate_addresses),
      byteOrder(p.sys->getGuestByteOrder()),
      stats(this)
{
}

ARMHintPrefetcher::ARMHintStats::ARMHintStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(sourceLoadsObserved, statistics::units::Count::get(),
               "address-indicating source loads observed"),
      ADD_STAT(sourcePrefetches, statistics::units::Count::get(),
               "first-level source prefetches generated"),
      ADD_STAT(sourceCacheHits, statistics::units::Count::get(),
               "first-level source candidates satisfied by cache data"),
      ADD_STAT(pendingInserted, statistics::units::Count::get(),
               "indirect prefetch buffer entries inserted"),
      ADD_STAT(pendingCompleted, statistics::units::Count::get(),
               "indirect prefetch buffer entries completed by fills"),
      ADD_STAT(indirectMappingsLearned, statistics::units::Count::get(),
               "source-to-target offset/shift mappings learned"),
      ADD_STAT(targetsGenerated, statistics::units::Count::get(),
               "second-level target prefetches generated"),
      ADD_STAT(targetsRejected, statistics::units::Count::get(),
               "candidate second-level targets rejected"),
      ADD_STAT(
          directPointerTargets, statistics::units::Count::get(),
          "second-level target prefetches using the loaded value directly"),
      ADD_STAT(explicitHintsObserved, statistics::units::Count::get(),
               "source requests carrying an explicit indirect-memory hint"),
      ADD_STAT(vectorElementsDecoded, statistics::units::Count::get(),
               "address-indicating scalar/vector elements decoded")
{}

RequestorID
ARMHintPrefetcher::normalizeRequestor(RequestorID requestor) const
{
    return useRequestorId
               ? requestor
               : static_cast<RequestorID>(Request::invldRequestorId);
}

Addr
ARMHintPrefetcher::getPC(const PrefetchInfo &pfi) const
{
    return pfi.hasPC() ? pfi.getPC() : 0;
}

bool
ARMHintPrefetcher::isHintPC(Addr pc) const
{
    return std::find(hintPCs.begin(), hintPCs.end(), pc) != hintPCs.end();
}

ARMHintPrefetcher::SourceContext
ARMHintPrefetcher::defaultContext(const PrefetchInfo &pfi) const
{
    return contextFromRequest(nullptr, pfi.getSize(), getPC(pfi));
}

ARMHintPrefetcher::SourceContext
ARMHintPrefetcher::contextFromRequest(const RequestPtr &req,
                                      unsigned request_size, Addr pc) const
{
    SourceContext context;
    context.valid = !requireIndirectHint;
    context.trainTargets = true;
    context.directPointer = false;
    context.signedIndex = signedIndex;
    bool configuredVector =
        sourceElementBytes != 0 || sourceElementStride != 0;
    context.elementBytes =
        sourceElementBytes != 0 ? sourceElementBytes : addressIndicatingBytes;
    context.elementStride = sourceElementStride;
    context.elements = configuredVector ? 0 : 1;

    if (isHintPC(pc)) {
        context.valid = true;
        context.explicitHint = true;
    }

    return normalizeContext(context, request_size);
}

ARMHintPrefetcher::SourceContext
ARMHintPrefetcher::contextFromPacket(const PacketPtr &pkt,
                                     const PrefetchInfo &pfi) const
{
    return contextFromRequest(pkt != nullptr ? pkt->req : nullptr,
                              pfi.getSize(), getPC(pfi));
}

void
ARMHintPrefetcher::refreshSourceContextFromHint(SourceContext &context) const
{
    (void)context;
}

ARMHintPrefetcher::SourceContext
ARMHintPrefetcher::normalizeContext(SourceContext context,
                                    unsigned request_size) const
{
    if (context.elementBytes == 0) {
        if (request_size != 0 && request_size <= sizeof(uint64_t)) {
            context.elementBytes = request_size;
        } else {
            context.elementBytes = sizeof(uint64_t);
        }
    }

    if (context.elementStride == 0) {
        context.elementStride = context.elementBytes;
    }

    bool explicit_offsets = !context.elementOffsets.empty();
    if (explicit_offsets) {
        std::vector<unsigned> offsets;
        for (unsigned offset : context.elementOffsets) {
            uint64_t end =
                static_cast<uint64_t>(offset) + context.elementBytes;
            if (request_size != 0 && end > request_size) {
                continue;
            }
            offsets.push_back(offset);
        }
        context.elementOffsets = offsets;
        context.elements = context.elementOffsets.size();
        if (context.elements == 0) {
            return context;
        }
    }

    if (context.elements == 0 && context.elementBytes != 0 &&
        context.elementStride != 0 && request_size >= context.elementBytes) {
        context.elements =
            1 + (request_size - context.elementBytes) / context.elementStride;
    }

    if (context.elements == 0) {
        context.elements = 1;
    }

    if (maxSourceElements != 0 && context.elements > maxSourceElements) {
        context.elements = maxSourceElements;
        if (!context.elementOffsets.empty() &&
            context.elementOffsets.size() > maxSourceElements) {
            context.elementOffsets.resize(maxSourceElements);
        }
    }

    return context;
}

bool
ARMHintPrefetcher::sourceAllowed(const SourceContext &context) const
{
    return context.valid && (!requireIndirectHint || context.explicitHint);
}

unsigned
ARMHintPrefetcher::contextElementBytes(const SourceContext &context,
                                       unsigned request_size) const
{
    return normalizeContext(context, request_size).elementBytes;
}

unsigned
ARMHintPrefetcher::contextElementStride(const SourceContext &context,
                                        unsigned elem_bytes) const
{
    SourceContext normalized = context;
    normalized.elementBytes = elem_bytes;
    return normalizeContext(normalized, elem_bytes).elementStride;
}

unsigned
ARMHintPrefetcher::contextElementCount(const SourceContext &context,
                                       unsigned request_size,
                                       unsigned elem_bytes,
                                       unsigned elem_stride) const
{
    SourceContext normalized = context;
    normalized.elementBytes = elem_bytes;
    normalized.elementStride = elem_stride;
    return normalizeContext(normalized, request_size).elements;
}

bool
ARMHintPrefetcher::contextElementOffset(const SourceContext &context,
                                        unsigned element, unsigned elem_stride,
                                        uint64_t &offset) const
{
    if (!context.elementOffsets.empty()) {
        if (element >= context.elementOffsets.size()) {
            return false;
        }
        offset = context.elementOffsets[element];
        return true;
    }

    if (elem_stride != 0 &&
        element > std::numeric_limits<uint64_t>::max() / elem_stride) {
        return false;
    }
    offset = static_cast<uint64_t>(element) * elem_stride;
    return true;
}

void
ARMHintPrefetcher::updateStreamContext(StreamEntry &entry,
                                       const SourceContext &context)
{
    if (!sourceAllowed(context)) {
        return;
    }
    entry.context = context;
    entry.context.valid = true;
}

bool
ARMHintPrefetcher::inheritSourceContext(StreamEntry *entry,
                                        SourceContext &context) const
{
    if (sourceAllowed(context)) {
        return true;
    }
    if (entry == nullptr || !entry->context.valid ||
        !sourceAllowed(entry->context)) {
        return false;
    }

    context = entry->context;
    return true;
}

ARMHintPrefetcher::StreamEntry *
ARMHintPrefetcher::findStream(Addr pc, bool secure, RequestorID requestor)
{
    requestor = normalizeRequestor(requestor);
    for (auto &entry : streamTable) {
        if (entry.valid && entry.pc == pc && entry.secure == secure &&
            entry.requestor == requestor) {
            entry.age = ++nextAge;
            return &entry;
        }
    }
    return nullptr;
}

ARMHintPrefetcher::StreamEntry &
ARMHintPrefetcher::getOrCreateStream(Addr pc, bool secure,
                                     RequestorID requestor)
{
    if (auto *entry = findStream(pc, secure, requestor)) {
        return *entry;
    }

    requestor = normalizeRequestor(requestor);
    StreamEntry *new_entry = nullptr;
    if (streamTable.size() < sourceTableEntries) {
        streamTable.emplace_back();
        new_entry = &streamTable.back();
    } else if (!streamTable.empty()) {
        auto victim =
            std::min_element(streamTable.begin(), streamTable.end(),
                             [](const StreamEntry &a, const StreamEntry &b) {
                                 return a.age < b.age;
                             });
        new_entry = &*victim;
    } else {
        streamTable.emplace_back();
        new_entry = &streamTable.back();
    }

    *new_entry = StreamEntry();
    new_entry->valid = true;
    new_entry->pc = pc;
    new_entry->requestor = requestor;
    new_entry->secure = secure;
    new_entry->age = ++nextAge;
    new_entry->candidates.resize(shiftValues.size());
    return *new_entry;
}

void
ARMHintPrefetcher::updateSourceStream(StreamEntry &entry, Addr addr,
                                      unsigned value_bytes)
{
    if (entry.lastAddrValid) {
        int64_t stride = static_cast<int64_t>(addr - entry.lastAddr);
        if (stride != 0 && stride == entry.stride) {
            entry.strideConfidence++;
        } else {
            entry.stride = stride;
            entry.strideConfidence = stride != 0 ? 1 : 0;
        }
    }

    entry.lastAddr = addr;
    entry.lastAddrValid = true;
    entry.valueBytes = value_bytes;
    entry.age = ++nextAge;
}

int64_t
ARMHintPrefetcher::signExtend(uint64_t value, unsigned value_bytes) const
{
    if (value_bytes == 0 || value_bytes >= sizeof(uint64_t)) {
        return static_cast<int64_t>(value);
    }

    unsigned bits = value_bytes * 8;
    uint64_t sign_bit = 1ULL << (bits - 1);
    uint64_t mask = (1ULL << bits) - 1;
    value &= mask;
    if ((value & sign_bit) != 0) {
        value |= ~mask;
    }
    return static_cast<int64_t>(value);
}

bool
ARMHintPrefetcher::decodeValue(const uint8_t *data, unsigned value_bytes,
                               bool signed_index, IndexValue &value) const
{
    if (value_bytes == 0 || value_bytes > sizeof(uint64_t)) {
        return false;
    }

    uint64_t raw = 0;
    if (byteOrder == ByteOrder::little) {
        for (unsigned i = 0; i < value_bytes; ++i) {
            raw |= static_cast<uint64_t>(data[i]) << (8 * i);
        }
    } else if (byteOrder == ByteOrder::big) {
        for (unsigned i = 0; i < value_bytes; ++i) {
            raw = (raw << 8) | data[i];
        }
    } else {
        return false;
    }

    value.raw = raw;
    value.signedIndex = signed_index;
    value.signedValue = signed_index ? signExtend(raw, value_bytes)
                                     : static_cast<int64_t>(raw);
    value.valueBytes = value_bytes;
    return true;
}

bool
ARMHintPrefetcher::decodeElementsFromPrefetchInfo(
    const PrefetchInfo &pfi, const SourceContext &context,
    std::vector<SourceElement> &values) const
{
    if (pfi.isWrite() || pfi.isCacheMiss() || pfi.getDataPtr() == nullptr) {
        return false;
    }

    SourceContext normalized = normalizeContext(context, pfi.getSize());
    unsigned elem_bytes = normalized.elementBytes;
    unsigned elem_stride = normalized.elementStride;
    if (elem_bytes == 0 || elem_bytes > sizeof(uint64_t) || elem_stride == 0) {
        return false;
    }

    for (unsigned i = 0; i < normalized.elements; ++i) {
        uint64_t offset = 0;
        if (!contextElementOffset(normalized, i, elem_stride, offset)) {
            break;
        }
        if (offset > std::numeric_limits<unsigned>::max()) {
            break;
        }

        if (offset + elem_bytes > pfi.getSize()) {
            continue;
        }
        const uint8_t *data =
            reinterpret_cast<const uint8_t *>(pfi.getDataPtr());

        SourceElement element;
        element.addr = pfi.getAddr() + offset;
        if (decodeValue(data + offset, elem_bytes, normalized.signedIndex,
                        element.value)) {
            values.push_back(element);
        }
    }

    return !values.empty();
}

bool
ARMHintPrefetcher::decodeElementsFromPacket(
    const PacketPtr &pkt, Addr base_source_addr, const SourceContext &context,
    std::vector<SourceElement> &values) const
{
    if (!pkt->hasData()) {
        return false;
    }

    SourceContext normalized = normalizeContext(context, pkt->getSize());
    unsigned elem_bytes = normalized.elementBytes;
    unsigned elem_stride = normalized.elementStride;
    if (elem_bytes == 0 || elem_bytes > sizeof(uint64_t) || elem_stride == 0) {
        return false;
    }

    Addr pkt_addr = pkt->getAddr();
    for (unsigned i = 0; i < normalized.elements; ++i) {
        uint64_t elem_offset = 0;
        if (!contextElementOffset(normalized, i, elem_stride, elem_offset)) {
            break;
        }
        if (base_source_addr >
            std::numeric_limits<Addr>::max() - elem_offset) {
            break;
        }

        Addr source_addr = base_source_addr + elem_offset;
        if (source_addr < pkt_addr) {
            continue;
        }

        Addr pkt_offset = source_addr - pkt_addr;
        if (pkt_offset + elem_bytes > pkt->getSize()) {
            continue;
        }

        SourceElement element;
        element.addr = source_addr;
        if (decodeValue(pkt->getConstPtr<uint8_t>() + pkt_offset, elem_bytes,
                        normalized.signedIndex, element.value)) {
            values.push_back(element);
        }
    }

    return !values.empty();
}

bool
ARMHintPrefetcher::decodeElementsFromCache(
    const CacheAccessor &cache, Addr base_source_addr, bool secure,
    const SourceContext &context, std::vector<SourceElement> &values) const
{
    unsigned request_size = context.elements * context.elementStride;
    if (!context.elementOffsets.empty()) {
        unsigned elem_bytes = context.elementBytes != 0
                                  ? context.elementBytes
                                  : addressIndicatingBytes;
        unsigned max_offset = *std::max_element(context.elementOffsets.begin(),
                                                context.elementOffsets.end());
        if (max_offset <= std::numeric_limits<unsigned>::max() - elem_bytes) {
            request_size = std::max(request_size, max_offset + elem_bytes);
        }
    }
    SourceContext normalized = normalizeContext(context, request_size);
    unsigned elem_bytes = normalized.elementBytes;
    unsigned elem_stride = normalized.elementStride;
    if (elem_bytes == 0 || elem_bytes > sizeof(uint64_t) || elem_stride == 0) {
        return false;
    }

    for (unsigned i = 0; i < normalized.elements; ++i) {
        uint64_t elem_offset = 0;
        if (!contextElementOffset(normalized, i, elem_stride, elem_offset)) {
            break;
        }
        if (base_source_addr >
            std::numeric_limits<Addr>::max() - elem_offset) {
            break;
        }

        Addr source_addr = base_source_addr + elem_offset;
        const Addr block_addr = blockAddress(source_addr);
        const Addr block_offset = source_addr - block_addr;
        if (block_offset + elem_bytes > blkSize) {
            continue;
        }

        const uint8_t *block_data = cache.findBlock(block_addr, secure);
        if (block_data == nullptr) {
            continue;
        }

        SourceElement element;
        element.addr = source_addr;
        if (decodeValue(block_data + block_offset, elem_bytes,
                        normalized.signedIndex, element.value)) {
            values.push_back(element);
        }
    }

    return !values.empty();
}

bool
ARMHintPrefetcher::shiftedUnsigned(uint64_t value, int shift,
                                   uint64_t &shifted) const
{
    if (shift >= 0) {
        if (shift >= static_cast<int>(8 * sizeof(Addr))) {
            return false;
        }
        uint64_t max_addr = std::numeric_limits<Addr>::max();
        if (value > (max_addr >> shift)) {
            return false;
        }
        shifted = value << shift;
    } else {
        int right_shift = -shift;
        if (right_shift >= static_cast<int>(8 * sizeof(Addr))) {
            return false;
        }
        shifted = value >> right_shift;
    }
    return true;
}

bool
ARMHintPrefetcher::shiftedSigned(int64_t value, int shift,
                                 int64_t &shifted) const
{
    __int128 wide = value;
    if (shift >= 0) {
        if (shift >= static_cast<int>(8 * sizeof(int64_t))) {
            return false;
        }
        wide <<= shift;
    } else {
        int right_shift = -shift;
        if (right_shift >= static_cast<int>(8 * sizeof(int64_t))) {
            return false;
        }
        wide >>= right_shift;
    }

    if (wide > std::numeric_limits<int64_t>::max() ||
        wide < std::numeric_limits<int64_t>::min()) {
        return false;
    }
    shifted = static_cast<int64_t>(wide);
    return true;
}

bool
ARMHintPrefetcher::addSignedOffset(Addr base, int64_t offset,
                                   Addr &target) const
{
    __int128 wide = static_cast<__int128>(base) + offset;
    if (wide < 0 || wide > std::numeric_limits<Addr>::max()) {
        return false;
    }
    target = static_cast<Addr>(wide);
    return true;
}

bool
ARMHintPrefetcher::isCandidateAddress(Addr target) const
{
    if (target < minCandidateAddress) {
        return false;
    }
    if (maxCandidateAddress != 0 && target > maxCandidateAddress) {
        return false;
    }
    if (targetAlignment > 1 && target % targetAlignment != 0) {
        return false;
    }
    if (validateCandidateAddresses && system != nullptr &&
        !system->getPhysMem().isMemAddr(blockAddress(target))) {
        return false;
    }
    return true;
}

bool
ARMHintPrefetcher::generateTarget(const StreamEntry &entry,
                                  const SourceContext &context,
                                  const IndexValue &value, Addr &target)
{
    auto generate_base_shift = [&](Addr base, int shift) -> bool {
        if (value.signedIndex) {
            int64_t shifted = 0;
            if (!shiftedSigned(value.signedValue, shift, shifted) ||
                !addSignedOffset(base, shifted, target)) {
                stats.targetsRejected++;
                return false;
            }
        } else {
            uint64_t shifted = 0;
            if (!shiftedUnsigned(value.raw, shift, shifted) ||
                base > std::numeric_limits<Addr>::max() - shifted) {
                stats.targetsRejected++;
                return false;
            }
            target = base + shifted;
        }

        if (!isCandidateAddress(target)) {
            stats.targetsRejected++;
            return false;
        }
        return true;
    };

    if (context.hasBase) {
        return generate_base_shift(context.base, context.shift);
    }

    if (entry.indirectValid) {
        return generate_base_shift(entry.base, entry.shift);
    }

    if (enableStaticOffset) {
        return generate_base_shift(staticBase, staticShift);
    }

    bool allow_direct_pointer =
        context.directPointer || (enableDirectPointer && !context.signedIndex);
    if (allow_direct_pointer) {
        target = static_cast<Addr>(value.raw);
        if (!isCandidateAddress(target)) {
            stats.targetsRejected++;
            return false;
        }
        stats.directPointerTargets++;
        return true;
    }

    return false;
}

void
ARMHintPrefetcher::addRecentSource(const StreamEntry &entry, Addr source_addr,
                                   const SourceContext &context,
                                   const IndexValue &value)
{
    if (recentSourceEntries == 0 || !context.trainTargets) {
        return;
    }

    IndirectSource source;
    source.sourceAddr = source_addr;
    source.pc = entry.pc;
    source.requestor = entry.requestor;
    source.secure = entry.secure;
    source.value = value;
    source.observed = curTick();
    source.context = context;

    recentSources.push_back(source);
    while (recentSources.size() > recentSourceEntries) {
        recentSources.pop_front();
    }
}

void
ARMHintPrefetcher::addPending(Addr source_addr, const StreamEntry &entry,
                              const SourceContext &context, int32_t priority,
                              bool from_prefetch)
{
    if (pendingEntries == 0 || !sourceAllowed(context)) {
        return;
    }

    SourceContext pending_context = context;
    if (!pending_context.hasBase && entry.indirectValid) {
        pending_context.hasBase = true;
        pending_context.base = entry.base;
        pending_context.shift = entry.shift;
    }
    pending_context.elements = 1;
    pending_context.elementStride = pending_context.elementBytes;
    pending_context.elementOffsets.clear();

    for (auto &pending_entry : pending) {
        if (pending_entry.sourceAddr == source_addr &&
            pending_entry.secure == entry.secure &&
            pending_entry.pc == entry.pc &&
            pending_entry.requestor == entry.requestor) {
            pending_entry.context = pending_context;
            pending_entry.priority =
                std::max(pending_entry.priority, priority);
            pending_entry.fromPrefetch |= from_prefetch;
            return;
        }
    }

    while (pending.size() >= pendingEntries) {
        pending.pop_front();
    }

    PendingIndirect pending_entry;
    pending_entry.sourceAddr = source_addr;
    pending_entry.pc = entry.pc;
    pending_entry.requestor = entry.requestor;
    pending_entry.secure = entry.secure;
    pending_entry.context = pending_context;
    pending_entry.priority = priority;
    pending_entry.fromPrefetch = from_prefetch;
    pending_entry.created = curTick();
    pending.push_back(pending_entry);
    stats.pendingInserted++;
}

void
ARMHintPrefetcher::matchRecentSources(Addr target, bool secure,
                                      RequestorID requestor)
{
    requestor = normalizeRequestor(requestor);

    for (auto it = recentSources.rbegin(); it != recentSources.rend(); ++it) {
        const IndirectSource &source = *it;
        if (!source.context.trainTargets || source.secure != secure) {
            continue;
        }
        if (useRequestorId && source.requestor != requestor) {
            continue;
        }

        StreamEntry &entry =
            getOrCreateStream(source.pc, source.secure, source.requestor);
        if (entry.candidates.size() != shiftValues.size()) {
            entry.candidates.resize(shiftValues.size());
        }

        for (size_t i = 0; i < shiftValues.size(); ++i) {
            Addr base = 0;
            if (source.value.signedIndex) {
                int64_t shifted = 0;
                if (!shiftedSigned(source.value.signedValue, shiftValues[i],
                                   shifted)) {
                    continue;
                }
                __int128 wide_base = static_cast<__int128>(target) - shifted;
                if (wide_base < 0 ||
                    wide_base > std::numeric_limits<Addr>::max()) {
                    continue;
                }
                base = static_cast<Addr>(wide_base);
            } else {
                uint64_t shifted = 0;
                if (!shiftedUnsigned(source.value.raw, shiftValues[i],
                                     shifted) ||
                    target < shifted) {
                    continue;
                }
                base = target - shifted;
            }

            auto &candidate = entry.candidates[i];
            if (candidate.valid && candidate.base == base &&
                candidate.shift == shiftValues[i]) {
                candidate.confidence++;
            } else {
                candidate.valid = true;
                candidate.base = base;
                candidate.shift = shiftValues[i];
                candidate.confidence = 1;
            }

            if (candidate.confidence >= indirectConfidenceThreshold) {
                bool learned_new_mapping = !entry.indirectValid ||
                                           entry.base != candidate.base ||
                                           entry.shift != candidate.shift;
                entry.indirectValid = true;
                entry.base = candidate.base;
                entry.shift = candidate.shift;
                entry.indirectConfidence = candidate.confidence;
                entry.valueBytes = source.value.valueBytes;
                entry.context = source.context;
                if (learned_new_mapping) {
                    stats.indirectMappingsLearned++;
                }
            }
        }
    }
}

bool
ARMHintPrefetcher::enqueueTargetFromValue(
    const PacketPtr &pkt, const CacheAccessor &cache, const StreamEntry &entry,
    const SourceContext &context, const IndexValue &value, int32_t priority)
{
    Addr target = 0;
    if (!generateTarget(entry, context, value, target)) {
        return false;
    }

    PrefetchInfo fill_pfi(pkt, pkt->req->getPaddr(), false);
    PrefetchInfo target_pfi(fill_pfi, blockAddress(target));
    AddrPriority target_cmd(blockAddress(target), priority,
                            PrefetchSourceType::ARMHint);
    insert(pkt, target_pfi, target_cmd);
    stats.targetsGenerated++;
    return true;
}

void
ARMHintPrefetcher::notify(const PacketPtr &pkt, const PrefetchInfo &pfi)
{
    if (pkt->cmd == MemCmd::HardPFReq) {
        return;
    }

    currentSourceContext = contextFromPacket(pkt, pfi);
    currentSourceContextValid = true;
    if (currentSourceContext.explicitHint && currentSourceContext.valid) {
        stats.explicitHintsObserved++;
    }
    Queued::notify(pkt, pfi);
    currentSourceContextValid = false;
}

void
ARMHintPrefetcher::calculatePrefetch(const PrefetchInfo &pfi,
                                     std::vector<AddrPriority> &addresses)
{
    const CacheAccessor *cache_accessor = cache;
    Addr pc = getPC(pfi);
    bool secure = pfi.isSecure();
    RequestorID requestor = normalizeRequestor(pfi.getRequestorId());
    SourceContext context =
        currentSourceContextValid ? currentSourceContext : defaultContext(pfi);

    matchRecentSources(pfi.getAddr(), secure, requestor);

    StreamEntry *entry_ptr = findStream(pc, secure, requestor);
    if (!inheritSourceContext(entry_ptr, context)) {
        return;
    }

    StreamEntry &entry = entry_ptr != nullptr
                             ? *entry_ptr
                             : getOrCreateStream(pc, secure, requestor);
    updateStreamContext(entry, context);
    updateSourceStream(entry, pfi.getAddr(),
                       contextElementBytes(context, pfi.getSize()));

    SourceContext normalized_context =
        normalizeContext(context, pfi.getSize());
    unsigned elem_stride = normalized_context.elementStride;
    unsigned elem_count = normalized_context.elements;

    const bool mayReceiveLateDataflowTarget =
        context.explicitHint && context.hint != nullptr;
    if (!pfi.isWrite() && pfi.isCacheMiss()) {
        if (entry.indirectValid || context.hasBase || enableStaticOffset ||
            context.directPointer ||
            (enableDirectPointer && !context.signedIndex) ||
            mayReceiveLateDataflowTarget) {
            for (unsigned i = 0; i < elem_count; ++i) {
                uint64_t elem_offset = 0;
                if (!contextElementOffset(normalized_context, i, elem_stride,
                                          elem_offset) ||
                    pfi.getAddr() >
                        std::numeric_limits<Addr>::max() - elem_offset) {
                    break;
                }
                Addr elem_addr = pfi.getAddr() + elem_offset;
                addPending(elem_addr, entry, context, 1, false);
            }
        }
    }

    std::vector<SourceElement> source_values;
    if (decodeElementsFromPrefetchInfo(pfi, context, source_values) ||
        (!pfi.isWrite() && !pfi.isCacheMiss() &&
         cache_accessor != nullptr &&
         decodeElementsFromCache(*cache_accessor, pfi.getAddr(), secure,
                                 context, source_values))) {
        stats.sourceLoadsObserved++;
        stats.vectorElementsDecoded += source_values.size();
        for (const auto &source : source_values) {
            entry.valueBytes = source.value.valueBytes;
            addRecentSource(entry, source.addr, context, source.value);

            Addr target = 0;
            if (generateTarget(entry, context, source.value, target)) {
                addresses.push_back(AddrPriority(target, 2, PrefetchSourceType::ARMHint));
                stats.targetsGenerated++;
            }
        }
    }

    if (entry.lastAddrValid && entry.stride != 0 &&
        entry.strideConfidence >= strideConfidenceThreshold) {
        SourceContext pf_context =
            entry.context.valid ? entry.context : context;
        for (unsigned i = 1; i <= degree; ++i) {
            uint64_t distance = lookahead + i;
            int64_t delta = entry.stride * static_cast<int64_t>(distance);
            Addr source_pf = 0;
            if (delta < 0) {
                uint64_t magnitude = static_cast<uint64_t>(-delta);
                if (pfi.getAddr() < magnitude) {
                    continue;
                }
                source_pf = pfi.getAddr() - magnitude;
            } else {
                uint64_t magnitude = static_cast<uint64_t>(delta);
                if (pfi.getAddr() >
                    std::numeric_limits<Addr>::max() - magnitude) {
                    continue;
                }
                source_pf = pfi.getAddr() + magnitude;
            }

            if (entry.indirectValid || pf_context.hasBase ||
                enableStaticOffset || pf_context.directPointer ||
                (enableDirectPointer && !pf_context.signedIndex)) {
                std::vector<SourceElement> cached_values;
                if (cache_accessor != nullptr &&
                    decodeElementsFromCache(*cache_accessor, source_pf, secure,
                                            pf_context, cached_values)) {
                    stats.sourceCacheHits++;
                    stats.vectorElementsDecoded += cached_values.size();
                    for (const auto &source : cached_values) {
                        addRecentSource(entry, source.addr, pf_context,
                                        source.value);
                        Addr target = 0;
                        if (generateTarget(entry, pf_context, source.value,
                                           target)) {
                            addresses.push_back(AddrPriority(target, 2, PrefetchSourceType::ARMHint));
                            stats.targetsGenerated++;
                        }
                    }
                    continue;
                }

                SourceContext normalized_pf_context =
                    normalizeContext(pf_context, pfi.getSize());
                unsigned pf_elem_stride = normalized_pf_context.elementStride;
                unsigned pf_elem_count = normalized_pf_context.elements;
                for (unsigned elem = 0; elem < pf_elem_count; ++elem) {
                    uint64_t elem_offset = 0;
                    if (!contextElementOffset(normalized_pf_context, elem,
                                              pf_elem_stride, elem_offset) ||
                        source_pf >
                            std::numeric_limits<Addr>::max() - elem_offset) {
                        break;
                    }
                    Addr elem_addr = source_pf + elem_offset;
                    addPending(elem_addr, entry, pf_context, 0, true);
                }
            }

            addresses.push_back(AddrPriority(source_pf, 1, PrefetchSourceType::ARMHint));
            stats.sourcePrefetches++;
        }
    }
}

void
ARMHintPrefetcher::notifyFill(const PacketPtr &pkt)
{
    const CacheAccessor *cache_accessor = cache;

    if (cache_accessor == nullptr || !pkt->req->hasPaddr()) {
        return;
    }

    Addr fill_block = blockAddress(pkt->getAddr());
    bool secure = pkt->isSecure();
    bool matched_pending = false;

    auto it = pending.begin();
    while (it != pending.end()) {
        PendingIndirect pending_entry = *it;
        refreshSourceContextFromHint(pending_entry.context);
        pending_entry.context =
            normalizeContext(pending_entry.context, pkt->getSize());
        if (blockAddress(pending_entry.sourceAddr) != fill_block ||
            pending_entry.secure != secure) {
            ++it;
            continue;
        }

        matched_pending = true;
        StreamEntry *entry = findStream(pending_entry.pc, pending_entry.secure,
                                        pending_entry.requestor);
        StreamEntry pendingFallbackStream;
        pendingFallbackStream.valid = true;
        pendingFallbackStream.pc = pending_entry.pc;
        pendingFallbackStream.requestor = pending_entry.requestor;
        pendingFallbackStream.secure = pending_entry.secure;
        pendingFallbackStream.context = pending_entry.context;
        if (pending_entry.context.hasBase) {
            pendingFallbackStream.indirectValid = true;
            pendingFallbackStream.base = pending_entry.context.base;
            pendingFallbackStream.shift = pending_entry.context.shift;
        }
        StreamEntry &target_entry =
            entry != nullptr ? *entry : pendingFallbackStream;
        std::vector<SourceElement> values;
        if (decodeElementsFromPacket(pkt, pending_entry.sourceAddr,
                                     pending_entry.context, values)) {
            stats.vectorElementsDecoded += values.size();
            for (const auto &source : values) {
                addRecentSource(target_entry, source.addr,
                                pending_entry.context, source.value);
                enqueueTargetFromValue(pkt, *cache_accessor, target_entry,
                                       pending_entry.context, source.value,
                                       pending_entry.priority);
                stats.pendingCompleted++;
            }
        }
        it = pending.erase(it);
    }

    if (!matched_pending && pkt->cmd != MemCmd::HardPFReq) {
        Addr source_addr = pkt->req->getPaddr();
        Addr pc = pkt->req->hasPC() ? pkt->req->getPC() : 0;
        SourceContext context =
            contextFromRequest(pkt->req, pkt->req->getSize(), pc);
        if (!sourceAllowed(context)) {
            return;
        }

        std::vector<SourceElement> values;
        if (decodeElementsFromPacket(pkt, source_addr, context, values)) {
            RequestorID requestor =
                normalizeRequestor(pkt->req->requestorId());
            StreamEntry &entry = getOrCreateStream(pc, secure, requestor);
            updateStreamContext(entry, context);
            stats.vectorElementsDecoded += values.size();
            for (const auto &source : values) {
                entry.valueBytes = source.value.valueBytes;
                addRecentSource(entry, source.addr, context, source.value);
            }
        }
    }
}

} // namespace prefetch
} // namespace gem5
