# Copyright (c) 2012, 2014, 2019 ARM Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Copyright (c) 2005 The Regents of The University of Michigan
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.SimObject import *
from m5.params import *
from m5.proxy import *

from m5.objects.ClockedObject import ClockedObject
from m5.objects.IndexingPolicies import *
from m5.objects.ReplacementPolicies import *

class HWPProbeEvent(object):
    def __init__(self, prefetcher, obj, *listOfNames):
        self.obj = obj
        self.prefetcher = prefetcher
        self.names = listOfNames

    def register(self):
        if self.obj:
            for name in self.names:
                self.prefetcher.getCCObject().addEventProbe(
                    self.obj.getCCObject(), name)

class BasePrefetcher(ClockedObject):
    type = 'BasePrefetcher'
    abstract = True
    cxx_class = 'gem5::prefetch::Base'
    cxx_header = "mem/cache/prefetch/base.hh"
    cxx_exports = [
        PyBindMethod("addEventProbe"),
        PyBindMethod("addTLB"),
        PyBindMethod("addHintDownStream"),
    ]
    sys = Param.System(Parent.any, "System this prefetcher belongs to")
    arch_db = Param.ArchDBer(Parent.any, "Arch DB")

    # Get the block size from the parent (system)
    block_size = Param.Int(Parent.cache_line_size, "Block size in bytes")

    on_miss = Param.Bool(False, "Only notify prefetcher on misses")
    on_read = Param.Bool(True, "Notify prefetcher on reads")
    on_write = Param.Bool(True, "Notify prefetcher on writes")
    on_data  = Param.Bool(True, "Notify prefetcher on data accesses")
    on_inst  = Param.Bool(True, "Notify prefetcher on instruction accesses")
    prefetch_train = Param.Bool(True, "Allow upstream PF req train low level Prefetcher")
    prefetch_on_access = Param.Bool(False,
        "Notify the hardware prefetcher on every access (not just misses)")
    prefetch_on_pf_hit = Param.Bool(False,
        "Notify the hardware prefetcher on hit on prefetched lines")
    use_virtual_addresses = Param.Bool(False,
        "Use virtual addresses for prefetching")
    page_bytes = Param.MemorySize('4KiB',
            "Size of pages for virtual addresses")

    is_sub_prefetcher = Param.Bool(False, "Is this a sub-prefetcher")

    # pf-ahead (cross cache-level prefetch) master switches.
    no_pfahead = Param.Bool(False,
        "Completely disable pf-ahead: drop prefetch requests that target a "
        "deeper cache level than the owning cache")
    no_pfahead_reserved = Param.Bool(False,
        "Disable pf-ahead cross-level offloading but keep the request as a "
        "normal current-level prefetch (demote instead of drop)")

    training_buffer_size = Param.Unsigned(8,
        "Maximum number of training requests buffered per cycle")

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._events = []
        self._tlbs = []
        self._functional_tlb = False
        self._downstream_pf = []

    def addEvent(self, newObject):
        self._events.append(newObject)

    # Override the normal SimObject::regProbeListeners method and
    # register deferred event handlers.
    def regProbeListeners(self):
        print("Registering probe listeners for Prefetcher {}".format(self))
        for tlb in self._tlbs:
            print(f"{self} addTLB {tlb}")
            self.getCCObject().addTLB(tlb.getCCObject(), self._functional_tlb)

        assert len(self._downstream_pf) <= 1
        if len(self._downstream_pf):
            print(f"{self} addHintDownStream {self._downstream_pf[0]}")
            self.getCCObject().addHintDownStream(self._downstream_pf[0].getCCObject())

        for event in self._events:
            event.register()
        self.getCCObject().regProbeListeners()

    def listenFromProbe(self, simObj, *probeNames):
        if not isinstance(simObj, SimObject):
            raise TypeError("argument must be of SimObject type")
        if len(probeNames) <= 0:
            raise TypeError("probeNames must have at least one element")
        self.addEvent(HWPProbeEvent(self, simObj, *probeNames))

    def registerTLB(self, simObj, functional):
        if not isinstance(simObj, SimObject):
            raise TypeError("argument must be a SimObject type")
        self._tlbs.append(simObj)
        self._functional_tlb = functional

    def add_pf_downstream(self, other_prefetcher):
        if not isinstance(other_prefetcher, SimObject):
            raise TypeError("other_prefetcher must be a SimObject type")
        self._downstream_pf.append(other_prefetcher)


class PrefetcherForwarder(BasePrefetcher):
    type = 'PrefetcherForwarder'
    cxx_header = "mem/cache/prefetch/forwarder.hh"
    cxx_class = 'gem5::prefetch::PrefetcherForwarder'
    cxx_exports = [
        PyBindMethod("setRealPrefetcher")
    ]

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._real_pf = None

    def regProbeListeners(self):
        if self._real_pf is None:
            print(f"real_pf of PrefetcherForwarder is None")
        else:
            self.getCCObject().setRealPrefetcher(self._real_pf.getCCObject())
        super().regProbeListeners()

    def setRealPrefetcher(self, real_pf):
        self._real_pf = real_pf


class QueuedPrefetcher(BasePrefetcher):
    type = "QueuedPrefetcher"
    abstract = True
    cxx_class = 'gem5::prefetch::Queued'
    cxx_header = "mem/cache/prefetch/queued.hh"
    latency = Param.Int(1, "Latency for generated prefetches")
    queue_size = Param.Int(32, "Maximum number of queued prefetches")
    max_prefetch_requests_with_pending_translation = Param.Int(32,
        "Maximum number of queued prefetches that have a missing translation")
    queue_squash = Param.Bool(True, "Squash queued prefetch on demand access")
    queue_filter = Param.Bool(True, "Don't queue redundant prefetches")
    cache_snoop = Param.Bool(True, "Snoop cache to eliminate redundant request")

    tag_prefetch = Param.Bool(True, "Tag prefetch with PC of generating access")

    # The throttle_control_percentage controls how many of the candidate
    # addresses generated by the prefetcher will be finally turned into
    # prefetch requests
    # - If set to 100, all candidates can be discarded (one request
    #   will always be allowed to be generated)
    # - Setting it to 0 will disable the throttle control, so requests are
    #   created for all candidates
    # - If set to 60, 40% of candidates will generate a request, and the
    #   remaining 60% will be generated depending on the current accuracy
    throttle_control_percentage = Param.Percent(0, "Percentage of requests \
        that can be throttled depending on the accuracy of the prefetcher.")

    max_pfahead_recv = Param.Int(1,"Maximum number of pfahead received")
    use_pf_buffer = Param.Bool(False, "use prefetch buffer to filter prefetches")
    max_pf_buffer_size = Param.Int(16, "size of prefetch buffer")


class XSStridePrefetcher(QueuedPrefetcher):
    type = 'XSStridePrefetcher'
    cxx_class = 'gem5::prefetch::XSStridePrefetcher'
    cxx_header = "mem/cache/prefetch/xs_stride.hh"

    use_virtual_addresses = True
    prefetch_on_pf_hit = True
    on_read = True
    on_write = False
    on_data = True
    on_inst = False
    region_size = Param.Int(1024, "region size")

    use_xs_depth = Param.Bool(True,"use xs rtl stride depth")
    fuzzy_stride_matching = Param.Bool(False, "Match stride with fuzzy condition")
    short_stride_thres = Param.Unsigned(512, "Ignore short strides when there are long strides (Bytes)")
    stride_dyn_depth = Param.Bool(False, "Dynamic depth of stride table")
    stride_entries = Param.MemorySize("10", "Stride Entries")
    stride_unique_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.stride_entries,
            size=Parent.stride_entries),
        "Indexing policy of stride table"
    )
    stride_unique_replacement_policy = Param.BaseReplacementPolicy(
        TreePLRURP(num_leaves=Parent.stride_entries),
        "Replacement policy of stride table"
    )
    stride_redundant_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.stride_entries,
            size=Parent.stride_entries),
        "Indexing policy of stride table"
    )
    stride_redundant_replacement_policy = Param.BaseReplacementPolicy(
        TreePLRURP(num_leaves=Parent.stride_entries),
        "Replacement policy of stride table"
    )
    use_redundant_table = Param.Bool(False, "Use redundant stride table")
    fuzzy_stride_matching = Param.Bool(False, "Match stride with fuzzy condition")

    # stride black list
    enable_non_stride_filter= Param.Bool(False, "Prevent non-stride PCs to touch stride table")
    non_stride_entries = Param.MemorySize("256", "Non-Stride Entries")
    non_stride_assoc = Param.Int(4, "Associativity of the non-stride pc table")
    non_stride_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.non_stride_assoc,
            size=Parent.non_stride_entries),
        "Indexing policy of non-stride PC table"
    )
    non_stride_replacement_policy = Param.BaseReplacementPolicy(
        TreePLRURP(num_leaves=Parent.non_stride_assoc),
        "Replacement policy of non-stride pc table"
    )

class OptPrefetcher(QueuedPrefetcher):
    # Opt is short for Offset pattern table prefetcher, a variant of SMS' pattern history table
    type = 'OptPrefetcher'
    cxx_class = 'gem5::prefetch::OptPrefetcher'
    cxx_header = "mem/cache/prefetch/opt.hh"
    region_size_64 = Param.Int(4096, "region size")
    opt_pf_level = Param.Int(3, "Prefetch target level")

    act_64_entries = Param.MemorySize(
        "64",
        "num of active generation table entries"
    )
    act_64_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.act_64_entries,
            size=Parent.act_64_entries),
        "Indexing policy of active generation table"
    )
    act_64_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(),
        "Replacement policy of active generation table"
    )
    opt_entries = Param.MemorySize("64","num of offset history table entried")
    opt_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.opt_entries,
            size=Parent.opt_entries),
        "Indexing policy of offset history table"
    )
    opt_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(),
        "Replacement policy of opt table"
    )

class XsStreamPrefetcher(QueuedPrefetcher):
    type = "XsStreamPrefetcher"
    cxx_class = "gem5::prefetch::XsStreamPrefetcher"
    cxx_header = "mem/cache/prefetch/xs_stream.hh"
    region_size = Param.Int(1024, "region size")
    use_virtual_addresses = True
    prefetch_on_pf_hit = True
    on_read = True
    on_write = False
    on_data  = True
    on_inst  = False
    xs_stream_depth = Param.Int(32, "The depth of xs_stream_depth")
    enable_auto_depth = Param.Bool(False, "enable autp depth.")
    enable_l3_stream_pre = Param.Bool(False, "enable l3 stream pre.")
    xs_stream_entries = Param.MemorySize(
        "16",
        "num of active generation table entries"
    )
    xs_stream_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.xs_stream_entries,
            size=Parent.xs_stream_entries),
        "Indexing policy of active generation table"
    )
    xs_stream_replacement_policy = Param.BaseReplacementPolicy(
         TreePLRURP(num_leaves = Parent.xs_stream_entries),
        "Replacement policy of active generation table"
    )



class BertiPrefetcher(QueuedPrefetcher):
    type = "BertiPrefetcher"
    cxx_class = "gem5::prefetch::BertiPrefetcher"
    cxx_header = "mem/cache/prefetch/berti.hh"

    use_virtual_addresses = True
    prefetch_on_pf_hit = True
    on_read = True
    on_write = False
    on_data  = True
    on_inst  = False

    addrlist_size = Param.Int(6, "The size of address list")

    deltalist_size = Param.Int(4, "The size of delta list")

    max_deltafound = Param.Int(4, "The maximum number of delta can be found")

    aggressive_pf = Param.Bool(False, "Issue pf reqs as many as possible.")
    history_table_entries = Param.MemorySize(
        "64", "Number of history table entries."
    )
    history_table_assoc = Param.Int(4, "Associativity of the history table.")
    history_table_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.history_table_assoc,
            size=Parent.history_table_entries
        ),
        "Indexing policy of history table."
    )
    history_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(),
        "Replacement policy of history table"
    )
    use_byte_addr = Param.Bool(True, "Use byte address")
    trigger_pht = Param.Bool(True, "Use Berti's prediction to trigger PHT")
    dump_top_deltas = Param.Bool(True, "Dump top deltas on exit")

class StridePrefetcherHashedSetAssociative(SetAssociative):
    type = 'StridePrefetcherHashedSetAssociative'
    cxx_class = 'gem5::prefetch::StridePrefetcherHashedSetAssociative'
    cxx_header = "mem/cache/prefetch/stride.hh"

class WorkerPrefetcher(QueuedPrefetcher):
    type = 'WorkerPrefetcher'
    cxx_class = 'gem5::prefetch::WorkerPrefetcher'
    cxx_header = "mem/cache/prefetch/worker.hh"

    on_inst = False
    on_data = True
    on_miss = False

    prefetch_on_access = True
    prefetch_on_pf_hit = True
    use_virtual_addresses = True

class StridePrefetcher(QueuedPrefetcher):
    type = 'StridePrefetcher'
    cxx_class = 'gem5::prefetch::Stride'
    cxx_header = "mem/cache/prefetch/stride.hh"

    # Do not consult stride prefetcher on instruction accesses
    on_inst = False
    on_data = True
    on_miss = True

    prefetch_on_pf_hit = True

    confidence_counter_bits = Param.Unsigned(3,
        "Number of bits of the confidence counter")
    initial_confidence = Param.Unsigned(4,
        "Starting confidence of new entries")
    confidence_threshold = Param.Percent(50,
        "Prefetch generation confidence threshold")

    use_requestor_id = Param.Bool(True, "Use requestor id based history")

    degree = Param.Int(4, "Number of prefetches to generate")

    table_assoc = Param.Int(4, "Associativity of the PC table")
    table_entries = Param.MemorySize("64", "Number of entries of the PC table")
    table_indexing_policy = Param.BaseIndexingPolicy(
        StridePrefetcherHashedSetAssociative(entry_size = 1,
        assoc = Parent.table_assoc, size = Parent.table_entries),
        "Indexing policy of the PC table")
    table_replacement_policy = Param.BaseReplacementPolicy(RandomRP(),
        "Replacement policy of the PC table")


class AMDContiguousStreamPrefetcher(QueuedPrefetcher):
    type = "AMDContiguousStreamPrefetcher"
    cxx_class = "gem5::prefetch::AMDContiguousStreamPrefetcher"
    cxx_header = "mem/cache/prefetch/amd_contiguous_stream.hh"

    stream_entries = Param.Unsigned(16, "Active contiguous stream entries")
    last_access_entries = Param.Unsigned(
        16, "Recent accesses used to create new streams")
    degree = Param.Unsigned(4, "Maximum prefetches per stream update")
    use_requestor_id = Param.Bool(False, "Include requestor ID in matching")

    prefetch_on_access = True
    prefetch_on_pf_hit = False
    on_inst = False


class AMDRIPRegionPrefetcher(QueuedPrefetcher):
    type = "AMDRIPRegionPrefetcher"
    cxx_class = "gem5::prefetch::AMDRIPRegionPrefetcher"
    cxx_header = "mem/cache/prefetch/amd_rip_region.hh"

    line_entry_entries = Param.Unsigned(
        32, "Entries in the line entry training table")
    region_history_entries = Param.Unsigned(
        512, "Entries in the RIP/Addr[5:4] region history table")
    negative_lines = Param.Unsigned(
        4, "Cache lines before the home line covered by a region")
    positive_lines = Param.Unsigned(
        6, "Cache lines after the home line covered by a region")
    rip_bits = Param.Unsigned(20, "Low RIP bits used by the predictor")
    address_offset_shift = Param.Unsigned(
        4, "First address bit in the line-alignment offset field")
    address_offset_bits = Param.Unsigned(
        2, "Number of line-alignment offset bits")
    counter_bits = Param.Unsigned(
        2, "Bits per region-history line-offset counter")
    counter_threshold = Param.Unsigned(
        2, "Minimum counter value required to issue a prefetch")
    min_pattern_bits = Param.Unsigned(
        2, "Minimum non-home lines needed to train a pseudo-random pattern")
    use_requestor_id = Param.Bool(False, "Include requestor ID in matching")
    degree = Param.Unsigned(10, "Maximum prefetches to generate per miss")

    prefetch_on_access = True
    prefetch_on_pf_hit = False
    on_inst = False


class AMDRegionTypePrefetcher(QueuedPrefetcher):
    type = "AMDRegionTypePrefetcher"
    cxx_class = "gem5::prefetch::AMDRegionTypePrefetcher"
    cxx_header = "mem/cache/prefetch/amd_region_type.hh"

    region_size = Param.Unsigned(2048, "Memory region size in bytes")
    observation_entries = Param.Unsigned(
        64, "Entries in the pattern observation table")
    region_type_entries = Param.Unsigned(
        512, "Entries in the region-address to region-type table")
    recorded_pattern_entries = Param.Unsigned(
        1024, "Entries in the region-type to recorded-pattern table")
    observation_window = Param.Unsigned(
        256, "Access-count window before completing an active observation")
    observation_timeout = Param.Unsigned(
        1024, "Idle access-count timeout before completing an observation")
    duplicate_prefetch_window = Param.Unsigned(
        32, "Access-count window suppressing duplicate pattern replays")
    recorded_pattern_assoc = Param.Unsigned(
        16, "Logical associativity for recorded-pattern way metadata")
    region_type_candidates = Param.Unsigned(
        2, "Candidate recorded patterns retained per region-type entry")
    use_requestor_id = Param.Bool(
        False, "Include RequestorID in region observation and mapping keys")
    confidence_counter_bits = Param.Unsigned(
        3, "Bits in recorded-pattern and region-type confidence counters")
    initial_confidence = Param.Unsigned(
        4, "Initial confidence for newly learned region-type mappings")
    confidence_threshold = Param.Unsigned(
        2, "Minimum confidence required before replaying a recorded pattern")
    aging_interval = Param.Unsigned(
        4096, "Access-count interval for confidence aging")
    similarity_threshold = Param.Unsigned(
        2, "Maximum exclusive Hamming distance for near pattern matching")
    min_pattern_bits = Param.Unsigned(
        2, "Minimum set subdivision bits required before installing a pattern")
    degree = Param.Unsigned(8, "Maximum prefetches generated per trigger")
    prefetch_distance = Param.Unsigned(
        0, "Maximum byte distance from trigger; zero means full region")
    merge_policy = Param.String(
        "or", "Merge policy for matching patterns: or, and, or replace")
    prefetch_current = Param.Bool(
        False, "Allow replay to prefetch the triggering subdivision")

    prefetch_on_access = True
    prefetch_on_pf_hit = False
    on_inst = False

    queue_squash = True
    queue_filter = True
    cache_snoop = True


class AppleAMPMPrefetcher(QueuedPrefetcher):
    type = "AppleAMPMPrefetcher"
    cxx_class = "gem5::prefetch::AppleAMPM"
    cxx_header = "mem/cache/prefetch/apple_ampm.hh"

    on_inst = False
    prefetch_on_access = True
    prefetch_on_pf_hit = True

    limit_stride = Param.Unsigned(
        0, "Limit the strides checked up to -X/X; zero disables the limit"
    )
    degree = Param.Unsigned(4, "Maximum prefetches generated per access")
    hot_zone_size = Param.MemorySize("2KiB", "Memory covered by a hot zone")

    access_map_table_entries = Param.MemorySize(
        "256", "Number of entries in the access map table"
    )
    access_map_table_assoc = Param.Unsigned(
        8, "Associativity of the access map table"
    )
    access_map_table_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.access_map_table_assoc,
            size=Parent.access_map_table_entries,
        ),
        "Indexing policy of the access map table",
    )
    access_map_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the access map table"
    )

    initial_quality_factor = Param.Unsigned(
        75, "Initial per-access-map quality factor tokens"
    )
    max_quality_factor = Param.Unsigned(
        100, "Maximum per-access-map quality factor tokens"
    )
    prefetch_token_cost = Param.Unsigned(
        8, "Quality factor tokens consumed by a non-store-only prefetch"
    )
    store_only_prefetch_token_cost = Param.Unsigned(
        10, "Quality factor tokens consumed by a store-only prefetch"
    )
    successful_prefetch_tokens = Param.Unsigned(
        12, "Quality factor tokens restored by a successful prefetch"
    )
    cache_hit_penalty_tokens = Param.Unsigned(
        4, "Quality factor tokens removed when a generated prefetch hits cache"
    )
    pointer_prefetch_tokens = Param.Unsigned(
        12, "Quality factor tokens restored when pointer activity is active"
    )
    quality_factor_bypass_accesses = Param.Unsigned(
        0,
        "Bypass quality factor after this many accessed lines in a map; "
        "zero means never bypass",
    )

    use_pointer_value_heuristic = Param.Bool(
        True,
        "Approximate pointer-read detection by tracking loaded values that "
        "are later used as load addresses",
    )
    pointer_field_max = Param.Unsigned(15, "Maximum pointer field value")
    pointer_initial_value = Param.Unsigned(0, "Initial pointer field value")
    pointer_increment = Param.Unsigned(
        4, "Pointer field increment for detected pointer reads"
    )
    pointer_decrement = Param.Unsigned(
        1, "Pointer field decrement for load accesses without pointer signal"
    )
    pointer_threshold = Param.Unsigned(
        1, "Pointer field threshold that marks pointer activity active"
    )
    pointer_tracking_entries = Param.Unsigned(
        64, "Loaded pointer-like values retained for future load matching"
    )
    pointer_tracking_window = Param.Unsigned(
        256,
        "Maximum later accesses before a retained pointer-like value ages out",
    )
    pointer_min_addr = Param.Addr(
        4096, "Minimum loaded value considered as a possible pointer"
    )
    pointer_value_distance = Param.MemorySize(
        "0B",
        "Optional maximum distance between the load address and loaded value; "
        "zero disables the locality filter",
    )

class AMDAOPPrefetcher(QueuedPrefetcher):
    type = "AMDAOPPrefetcher"
    cxx_class = "gem5::prefetch::AMDAOP"
    cxx_header = "mem/cache/prefetch/amd_aop.hh"

    on_inst = False
    on_write = True
    prefetch_on_access = True
    prefetch_on_pf_hit = True
    use_virtual_addresses = True
    cache_snoop = True
    page_bytes = "4KiB"

    stride_table_entries = Param.Unsigned(
        128, "Entries in the striding load table corresponding to table 304"
    )
    target_table_entries = Param.Unsigned(
        256,
        "Entries in the pointer target PC-pair table corresponding to table 306",
    )
    recent_pointer_entries = Param.Unsigned(
        128, "Recently loaded pointer values retained for target-pair learning"
    )
    pending_address_load_entries = Param.Unsigned(
        128, "Outstanding address-load prefetches waiting for fill data"
    )
    pointer_value_entries = Param.Unsigned(
        512, "Cached pointer-array values indexed by element address"
    )

    confidence_counter_bits = Param.Unsigned(
        3, "Number of bits in stride and target confidence counters"
    )
    initial_confidence = Param.Unsigned(
        1, "Initial confidence for new entries"
    )
    stride_confidence_threshold = Param.Unsigned(
        3, "Minimum confidence for a load PC to be considered striding"
    )
    target_confidence_threshold = Param.Unsigned(
        3, "Minimum confidence for a pointer target PC pair"
    )

    use_requestor_id = Param.Bool(False, "Partition tables by requestor id")
    address_load_degree = Param.Unsigned(
        2, "Number of future pointer-array elements to prefetch per trigger"
    )
    target_degree = Param.Unsigned(
        4, "Maximum pointer-target prefetches generated per trigger"
    )
    lookahead = Param.Unsigned(
        2, "Number of strides skipped before the first future address load"
    )
    pointer_bytes = Param.Unsigned(8, "Pointer element width in bytes")
    pointer_align_bits = Param.Unsigned(
        3, "Required low zero bits for values treated as pointers"
    )
    index_scale = Param.Unsigned(
        0,
        "Scale applied to non-address loaded values used as index operands; "
        "zero disables index-mode target learning",
    )
    min_pointer_addr = Param.Addr(
        4096, "Minimum loaded value considered as a possible pointer"
    )
    max_target_offset = Param.MemorySize(
        "2KiB",
        "Maximum absolute pointer-target offset; for index-mode targets this "
        "bounds the tolerated offset delta during pair matching",
    )
    pointer_tracking_window = Param.Unsigned(
        512, "Maximum accesses before a recent pointer value ages out"
    )
    cache_status_threshold = Param.Unsigned(
        64, "Halve per-target cache hit/miss counters after this many samples"
    )
    min_cache_status_for_throttling = Param.Unsigned(
        16, "Minimum samples before low-miss-rate target throttling"
    )
    low_miss_rate_threshold_pct = Param.Unsigned(
        10, "Detrain target entries below this miss-rate percentage"
    )
    prefetch_current_pointer = Param.Bool(
        False, "Also prefetch the target of the currently loaded pointer"
    )

class TaggedPrefetcher(QueuedPrefetcher):
    type = 'TaggedPrefetcher'
    cxx_class = 'gem5::prefetch::Tagged'
    cxx_header = "mem/cache/prefetch/tagged.hh"

    degree = Param.Int(2, "Number of prefetches to generate")

class DSPatchPrefetcher(QueuedPrefetcher):
    type = "DSPatchPrefetcher"
    cxx_class = "gem5::prefetch::DSPatch"
    cxx_header = "mem/cache/prefetch/dspatch.hh"

    on_inst = False
    prefetch_on_access = True
    page_buffer_entries = Param.Unsigned(
        64, "Number of 4KiB pages tracked in the DSPatch page buffer"
    )
    signature_table_entries = Param.Unsigned(
        256, "Number of tagless direct-mapped DSPatch signature entries"
    )
    region_size = Param.MemorySize(
        "4KiB", "Spatial region tracked by each DSPatch page-buffer entry"
    )
    bandwidth_utilization_quartile = Param.Unsigned(
        0,
        "Fallback static memory bandwidth utilization quartile used for "
        "CovP/AccP selection when memory-controller and local bandwidth "
        "tracking are disabled: 0 <25%, 1 25-50%, 2 50-75%, 3 >=75%",
    )
    use_memory_controller_bandwidth = Param.Bool(
        True,
        "Use the MemCtrl CAS-count bandwidth quartile for DSPatch "
        "CovP/AccP selection",
    )
    dynamic_bandwidth_monitor = Param.Bool(
        False,
        "Use a local rolling access-rate estimate to update the DSPatch "
        "bandwidth quartile when a real DRAM bandwidth signal is unavailable",
    )
    bandwidth_window_cycles = Param.Unsigned(
        4096, "Cycles per DSPatch local bandwidth-estimation window"
    )
    bandwidth_low_threshold = Param.Unsigned(
        64, "Observed accesses per window for the 25% bandwidth quartile"
    )
    bandwidth_mid_threshold = Param.Unsigned(
        128, "Observed accesses per window for the 50% bandwidth quartile"
    )
    bandwidth_high_threshold = Param.Unsigned(
        256, "Observed accesses per window for the 75% bandwidth quartile"
    )
    max_or_count = Param.Unsigned(
        3, "Maximum CovP OR updates before DSPatch stops growing the pattern"
    )
    accuracy_threshold_pct = Param.Percent(
        50, "Accuracy threshold for DSPatch CovP/AccP quality counters"
    )
    coverage_threshold_pct = Param.Percent(
        50, "Coverage threshold for DSPatch CovP quality counter"
    )


class IndirectMemoryPrefetcher(QueuedPrefetcher):
    type = 'IndirectMemoryPrefetcher'
    cxx_class = 'gem5::prefetch::IndirectMemory'
    cxx_header = "mem/cache/prefetch/indirect_memory.hh"
    pt_table_entries = Param.MemorySize("16",
        "Number of entries of the Prefetch Table")
    pt_table_assoc = Param.Unsigned(16, "Associativity of the Prefetch Table")
    pt_table_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(entry_size = 1, assoc = Parent.pt_table_assoc,
        size = Parent.pt_table_entries),
        "Indexing policy of the pattern table")
    pt_table_replacement_policy = Param.BaseReplacementPolicy(LRURP(),
        "Replacement policy of the pattern table")
    max_prefetch_distance = Param.Unsigned(16, "Maximum prefetch distance")
    num_indirect_counter_bits = Param.Unsigned(3,
        "Number of bits of the indirect counter")
    ipd_table_entries = Param.MemorySize("4",
        "Number of entries of the Indirect Pattern Detector")
    ipd_table_assoc = Param.Unsigned(4,
        "Associativity of the Indirect Pattern Detector")
    ipd_table_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(entry_size = 1, assoc = Parent.ipd_table_assoc,
        size = Parent.ipd_table_entries),
        "Indexing policy of the Indirect Pattern Detector")
    ipd_table_replacement_policy = Param.BaseReplacementPolicy(LRURP(),
        "Replacement policy of the Indirect Pattern Detector")
    shift_values = VectorParam.Int([2, 3, 4, -3], "Shift values to evaluate")
    addr_array_len = Param.Unsigned(4, "Number of misses tracked")
    prefetch_threshold = Param.Unsigned(2,
        "Counter threshold to start the indirect prefetching")
    stream_counter_threshold = Param.Unsigned(4,
        "Counter threshold to enable the stream prefetcher")
    streaming_distance = Param.Unsigned(4,
        "Number of prefetches to generate when using the stream prefetcher")

class ARMOffsetBasedPointerPrefetcher(QueuedPrefetcher):
    type = "ARMOffsetBasedPointerPrefetcher"
    cxx_class = "gem5::prefetch::ARMOffsetBasedPointerPrefetcher"
    cxx_header = "mem/cache/prefetch/arm_offset_based_pointer.hh"

    on_inst = False
    prefetch_on_access = True

    history_entries = Param.Unsigned(
        64, "Number of trigger access PCs retained in the history buffer"
    )
    pointer_cache_entries = Param.Unsigned(
        64, "Number of recent detected pointers retained in the pointer cache"
    )
    structure_entries = Param.Unsigned(
        64, "Number of learned data structure relationships"
    )
    pending_entries = Param.Unsigned(
        32, "Number of pending pointer-line prefetches"
    )
    spatial_entries = Param.Unsigned(
        8, "Number of SMS-style offsets retained per trigger PC"
    )
    recent_pointer_search_entries = Param.Unsigned(
        16, "Recent pointer cache entries searched while learning"
    )
    max_element_bytes = Param.MemorySize(
        "512B", "Maximum trigger-to-trigger distance considered structural"
    )
    max_pointer_offset_bytes = Param.MemorySize(
        "256B", "Maximum pointer-location offset considered structural"
    )
    max_pointer_target_offset_bytes = Param.MemorySize(
        "256B", "Maximum pointer-target to trigger offset"
    )
    min_pointer_address = Param.Addr(
        4096, "Ignore candidate pointer values below this address"
    )
    pointer_bytes = Param.Unsigned(
        8, "Pointer detector width in bytes; use 4 for 32-bit targets"
    )
    pointer_msw_match_bits = Param.Unsigned(
        16, "Most-significant address bits that must match pointer context"
    )
    pointer_align_bits = Param.Unsigned(
        3, "Required low zero bits for pointer candidates"
    )
    confidence_bits = Param.Unsigned(
        3, "Bits in learned-relationship confidence counters"
    )
    min_confidence = Param.Unsigned(
        2, "Minimum relationship confidence before issuing prefetches"
    )
    degree = Param.Unsigned(
        2, "Number of table-structure data prefetches generated per access"
    )
    lookahead = Param.Unsigned(
        2, "Number of dependent pointer dereferences to look ahead"
    )
    scan_cacheline_on_fill = Param.Bool(
        True, "Scan filled cache lines for pointer candidates"
    )
    enable_table_detector = Param.Bool(
        True, "Learn constant trigger-address displacement structures"
    )
    enable_linked_list_detector = Param.Bool(
        True, "Learn pointer-inside-current-element linked-list structures"
    )
    enable_pointer_table_detector = Param.Bool(
        True, "Learn arrays of pointers to data elements"
    )


class ARMHintPrefetcher(QueuedPrefetcher):
    type = "ARMHintPrefetcher"
    cxx_class = "gem5::prefetch::ARMHintPrefetcher"
    cxx_header = "mem/cache/prefetch/arm_hint.hh"

    on_inst = False
    prefetch_on_access = True
    prefetch_on_pf_hit = True
    queue_filter = True
    cache_snoop = True
    page_bytes = "256TiB"

    source_table_entries = Param.Unsigned(
        64, "Number of source-stream entries carrying indirect hints"
    )
    recent_source_entries = Param.Unsigned(
        64, "Number of recently observed address-indicating source values"
    )
    pending_entries = Param.Unsigned(
        64, "Number of entries in the indirect prefetch buffer"
    )
    stride_confidence_threshold = Param.Unsigned(
        1, "Source-stream confidence required to prefetch source data"
    )
    indirect_confidence_threshold = Param.Unsigned(
        2, "Observed source-target matches required to learn offset/shift"
    )
    degree = Param.Unsigned(4, "Number of first-level source prefetches")
    lookahead = Param.Unsigned(
        0, "Additional source-stream strides skipped before prefetching"
    )
    address_indicating_bytes = Param.Unsigned(
        4,
        "Bytes to decode from a filled source line"
        "when request size is unknown",
    )
    source_element_bytes = Param.Unsigned(
        0,
        "Bytes per address-indicating element; 0 uses request hint, "
        "address_indicating_bytes, or scalar request size",
    )
    source_element_stride = Param.Unsigned(
        0,
        "Byte distance between vector elements; 0 means packed elements",
    )
    max_source_elements = Param.Unsigned(
        16,
        "Maximum address-indicating elements decoded from one source request",
    )
    signed_index = Param.Bool(
        False,
        "Sign-extend decoded elements before base+index target formation",
    )
    min_candidate_address = Param.Addr(
        4096, "Reject generated targets below this physical address"
    )
    max_candidate_address = Param.Addr(
        0, "Reject generated targets above this address; 0 disables the check"
    )
    target_alignment = Param.Unsigned(
        8, "Required target alignment in bytes; 0 or 1 disables the check"
    )
    shift_values = VectorParam.Int(
        [6, 4, 3, 2, 0],
        "Index-to-address shifts evaluated for target formation",
    )
    require_indirect_hint = Param.Bool(
        True,
        "Only treat explicit request hints or configured hint_pcs as "
        "address-indicating sources",
    )
    hint_pcs = VectorParam.Addr(
        [],
        "Fallback PC list treated as explicit indirect-memory hints when "
        "the CPU model does not attach IndirectMemoryPrefetchHint",
    )
    use_requestor_id = Param.Bool(
        False, "Include requestor id in source stream and hint matching"
    )
    enable_direct_pointer = Param.Bool(
        True, "Treat a loaded value as a direct pointer when it validates"
    )
    enable_static_offset = Param.Bool(
        False, "Use the configured static base and shift without learning"
    )
    static_base = Param.Addr(
        0, "Static target base when static-offset mode is enabled"
    )
    static_shift = Param.Int(
        0, "Static target shift when static-offset mode is enabled"
    )
    enable_processor_hint_target = Param.Bool(
        False,
        "Attach the configured target base and shift to CPU-produced "
        "IndirectMemoryPrefetchHint requests",
    )
    processor_hint_base = Param.Addr(
        0,
        "Target base carried by processor-side indirect-memory hints",
    )
    processor_hint_shift = Param.Int(
        0,
        "Target index shift carried by processor-side indirect-memory hints",
    )
    validate_candidate_addresses = Param.Bool(
        True, "Reject generated targets outside configured physical memory"
    )


class SignaturePathPrefetcher(QueuedPrefetcher):
    type = 'SignaturePathPrefetcher'
    cxx_class = 'gem5::prefetch::SignaturePath'
    cxx_header = "mem/cache/prefetch/signature_path.hh"

    signature_shift = Param.UInt8(3,
        "Number of bits to shift when calculating a new signature");
    signature_bits = Param.UInt16(12,
        "Size of the signature, in bits");
    signature_table_entries = Param.MemorySize("1024",
        "Number of entries of the signature table")
    signature_table_assoc = Param.Unsigned(2,
        "Associativity of the signature table")
    signature_table_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(entry_size = 1, assoc = Parent.signature_table_assoc,
        size = Parent.signature_table_entries),
        "Indexing policy of the signature table")
    signature_table_replacement_policy = Param.BaseReplacementPolicy(LRURP(),
        "Replacement policy of the signature table")

    num_counter_bits = Param.UInt8(3,
        "Number of bits of the saturating counters")
    pattern_table_entries = Param.MemorySize("4096",
        "Number of entries of the pattern table")
    pattern_table_assoc = Param.Unsigned(1,
        "Associativity of the pattern table")
    strides_per_pattern_entry = Param.Unsigned(4,
        "Number of strides stored in each pattern entry")
    pattern_table_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(entry_size = 1, assoc = Parent.pattern_table_assoc,
        size = Parent.pattern_table_entries),
        "Indexing policy of the pattern table")
    pattern_table_replacement_policy = Param.BaseReplacementPolicy(LRURP(),
        "Replacement policy of the pattern table")

    prefetch_confidence_threshold = Param.Float(0.5,
        "Minimum confidence to issue prefetches")
    lookahead_confidence_threshold = Param.Float(0.75,
        "Minimum confidence to continue exploring lookahead entries")

class SignaturePathPrefetcherV2(SignaturePathPrefetcher):
    type = 'SignaturePathPrefetcherV2'
    cxx_class = 'gem5::prefetch::SignaturePathV2'
    cxx_header = "mem/cache/prefetch/signature_path_v2.hh"

    signature_table_entries = "256"
    signature_table_assoc = 1
    pattern_table_entries = "512"
    pattern_table_assoc = 1
    num_counter_bits = 4
    prefetch_confidence_threshold = 0.25
    lookahead_confidence_threshold = 0.25

    global_history_register_entries = Param.MemorySize("8",
        "Number of entries of global history register")
    global_history_register_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(entry_size = 1,
        assoc = Parent.global_history_register_entries,
        size = Parent.global_history_register_entries),
        "Indexing policy of the global history register")
    global_history_register_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the global history register")

class CDP(QueuedPrefetcher):
    type = 'CDP'
    cxx_class = 'gem5::prefetch::CDP'
    cxx_header = "mem/cache/prefetch/cdp.hh"
    use_virtual_addresses = True
    prefetch_on_access = False
    prefetch_on_pf_hit = True
    on_read = True
    on_write = False
    on_data  = True
    on_inst  = False
    enable_coordinate = Param.Bool(False, "enable coordinate throttling or not")
    use_byteorder = Param.Bool(True,"")
    vpn_sub_entries = Param.Unsigned(4,
        "Sub entry number of each of vpnEntry")
    vpn_assoc = Param.Unsigned(4,
        "Ways of vpnTable")
    vpn_entries = Param.MemorySize(
        "16",
        "num of entries in vpnTable"
    )
    vpn_reset_period = Param.Unsigned(128,
        "reset vpn table after how many accesses")
    vpn_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.vpn_assoc,
            size=Parent.vpn_entries),
        "Indexing policy of vpnTable"
    )
    vpn_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(),
        "Replacement policy of vpnTable"
    )
    filter_table_assoc = Param.Unsigned(256,
        "Ways of filterTable")
    filter_table_entries = Param.MemorySize(
        "256",
        "num of entries in filterTable"
    )
    filter_table_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.filter_table_assoc,
            size=Parent.filter_table_entries),
        "Indexing policy of filterTable"
    )
    filter_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(),
        "Replacement policy of filterTable"
    )
    filter_entry_region_blks = Param.Unsigned(64,
        "How many blks a region can track")
    filter_entry_granularity = Param.Unsigned(4096,
        "How many bytes a blk in a region can track")
    throttle_aggressiveness = Param.Float(2.0,
        "A parameter to control the aggressiveness of throttling")

class CompositeWithWorkerPrefetcher(WorkerPrefetcher):
    type = 'CompositeWithWorkerPrefetcher'
    cxx_class = 'gem5::prefetch::CompositeWithWorkerPrefetcher'
    cxx_header = "mem/cache/prefetch/composite_with_worker.hh"
    use_virtual_addresses = True
    prefetch_on_access = False
    prefetch_on_pf_hit = True
    on_read = True
    on_write = False
    on_data  = True
    on_inst  = False

class AccessMapPatternMatching(ClockedObject):
    type = 'AccessMapPatternMatching'
    cxx_class = 'gem5::prefetch::AccessMapPatternMatching'
    cxx_header = "mem/cache/prefetch/access_map_pattern_matching.hh"

    block_size = Param.Unsigned(Parent.block_size,
        "Cacheline size used by the prefetcher using this object")

    limit_stride = Param.Unsigned(0,
        "Limit the strides checked up to -X/X, if 0, disable the limit")
    start_degree = Param.Unsigned(4,
        "Initial degree (Maximum number of prefetches generated")
    hot_zone_size = Param.MemorySize("2KiB", "Memory covered by a hot zone")
    access_map_table_entries = Param.MemorySize("256",
        "Number of entries in the access map table")
    access_map_table_assoc = Param.Unsigned(8,
        "Associativity of the access map table")
    access_map_table_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(entry_size = 1, assoc = Parent.access_map_table_assoc,
        size = Parent.access_map_table_entries),
        "Indexing policy of the access map table")
    access_map_table_replacement_policy = Param.BaseReplacementPolicy(LRURP(),
        "Replacement policy of the access map table")
    high_coverage_threshold = Param.Float(0.25,
        "A prefetch coverage factor bigger than this is considered high")
    low_coverage_threshold = Param.Float(0.125,
        "A prefetch coverage factor smaller than this is considered low")
    high_accuracy_threshold = Param.Float(0.5,
        "A prefetch accuracy factor bigger than this is considered high")
    low_accuracy_threshold = Param.Float(0.25,
        "A prefetch accuracy factor smaller than this is considered low")
    high_cache_hit_threshold = Param.Float(0.875,
        "A cache hit ratio bigger than this is considered high")
    low_cache_hit_threshold = Param.Float(0.75,
        "A cache hit ratio smaller than this is considered low")
    epoch_cycles = Param.Cycles(256000, "Cycles in an epoch period")
    offchip_memory_latency = Param.Latency("30ns",
        "Memory latency used to compute the required memory bandwidth")

class AMPMPrefetcher(QueuedPrefetcher):
    type = 'AMPMPrefetcher'
    cxx_class = 'gem5::prefetch::AMPM'
    cxx_header = "mem/cache/prefetch/access_map_pattern_matching.hh"
    ampm = Param.AccessMapPatternMatching( AccessMapPatternMatching(),
        "Access Map Pattern Matching object")

class DeltaCorrelatingPredictionTables(SimObject):
    type = 'DeltaCorrelatingPredictionTables'
    cxx_class = 'gem5::prefetch::DeltaCorrelatingPredictionTables'
    cxx_header = "mem/cache/prefetch/delta_correlating_prediction_tables.hh"
    deltas_per_entry = Param.Unsigned(20,
        "Number of deltas stored in each table entry")
    delta_bits = Param.Unsigned(12, "Bits per delta")
    delta_mask_bits = Param.Unsigned(8,
        "Lower bits to mask when comparing deltas")
    table_entries = Param.MemorySize("128",
        "Number of entries in the table")
    table_assoc = Param.Unsigned(128,
        "Associativity of the table")
    table_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(entry_size = 1, assoc = Parent.table_assoc,
        size = Parent.table_entries),
        "Indexing policy of the table")
    table_replacement_policy = Param.BaseReplacementPolicy(LRURP(),
        "Replacement policy of the table")

class DCPTPrefetcher(QueuedPrefetcher):
    type = 'DCPTPrefetcher'
    cxx_class = 'gem5::prefetch::DCPT'
    cxx_header = "mem/cache/prefetch/delta_correlating_prediction_tables.hh"
    dcpt = Param.DeltaCorrelatingPredictionTables(
        DeltaCorrelatingPredictionTables(),
        "Delta Correlating Prediction Tables object")

class IrregularStreamBufferPrefetcher(QueuedPrefetcher):
    type = "IrregularStreamBufferPrefetcher"
    cxx_class = 'gem5::prefetch::IrregularStreamBuffer'
    cxx_header = "mem/cache/prefetch/irregular_stream_buffer.hh"

    num_counter_bits = Param.Unsigned(2,
        "Number of bits of the confidence counter")
    chunk_size = Param.Unsigned(256,
        "Maximum number of addresses in a temporal stream")
    degree = Param.Unsigned(4, "Number of prefetches to generate")
    training_unit_assoc = Param.Unsigned(128,
        "Associativity of the training unit")
    training_unit_entries = Param.MemorySize("128",
        "Number of entries of the training unit")
    training_unit_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(entry_size = 1, assoc = Parent.training_unit_assoc,
        size = Parent.training_unit_entries),
        "Indexing policy of the training unit")
    training_unit_replacement_policy = Param.BaseReplacementPolicy(LRURP(),
        "Replacement policy of the training unit")

    prefetch_candidates_per_entry = Param.Unsigned(16,
        "Number of prefetch candidates stored in a SP-AMC entry")
    address_map_cache_assoc = Param.Unsigned(128,
        "Associativity of the PS/SP AMCs")
    address_map_cache_entries = Param.MemorySize("128",
        "Number of entries of the PS/SP AMCs")
    ps_address_map_cache_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(entry_size = 1,
        assoc = Parent.address_map_cache_assoc,
        size = Parent.address_map_cache_entries),
        "Indexing policy of the Physical-to-Structural Address Map Cache")
    ps_address_map_cache_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(),
        "Replacement policy of the Physical-to-Structural Address Map Cache")
    sp_address_map_cache_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(entry_size = 1,
        assoc = Parent.address_map_cache_assoc,
        size = Parent.address_map_cache_entries),
        "Indexing policy of the Structural-to-Physical Address Mao Cache")
    sp_address_map_cache_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(),
        "Replacement policy of the Structural-to-Physical Address Map Cache")

class SlimAccessMapPatternMatching(AccessMapPatternMatching):
    start_degree = 2
    limit_stride = 4

class SlimDeltaCorrelatingPredictionTables(DeltaCorrelatingPredictionTables):
    table_entries = "256"
    table_assoc = 256
    deltas_per_entry = 9

class SlimAMPMPrefetcher(QueuedPrefetcher):
    type = 'SlimAMPMPrefetcher'
    cxx_class = 'gem5::prefetch::SlimAMPM'
    cxx_header = "mem/cache/prefetch/slim_ampm.hh"

    ampm = Param.AccessMapPatternMatching(SlimAccessMapPatternMatching(),
        "Access Map Pattern Matching object")
    dcpt = Param.DeltaCorrelatingPredictionTables(
        SlimDeltaCorrelatingPredictionTables(),
        "Delta Correlating Prediction Tables object")

class BOPPrefetcher(QueuedPrefetcher):
    type = "BOPPrefetcher"
    cxx_class = 'gem5::prefetch::BOP'
    cxx_header = "mem/cache/prefetch/bop.hh"

    use_virtual_addresses = False
    prefetch_on_pf_hit = True
    on_read = True
    on_write = False
    on_data  = True
    on_inst  = False

    score_max = Param.Unsigned(20, "Max. score to update the best offset")
    round_max = Param.Unsigned(50, "Max. round to update the best offset")
    bad_score = Param.Unsigned(12, "Score at which the HWP is disabled")
    rr_size = Param.Unsigned(256, "Number of entries of each RR bank")
    tag_bits = Param.Unsigned(24, "Bits used to store the tag")
    negative_offsets_enable = Param.Bool(False,
                "Initialize the offsets list also with negative values \
                (i.e. the table will have half of the entries with positive \
                offsets and the other half with negative ones)")
    delay_queue_enable = Param.Bool(True, "Enable the delay queue")
    delay_queue_size = Param.Unsigned(64,
                "Number of entries in the delay queue")
    delay_queue_cycles = Param.Cycles(150,
                "Cycles to delay a write in the left RR table from the delay queue")

    autoLearning = Param.Bool(False," auto learn offset")

    offsets = VectorParam.Int([72, 75, 80, 81, 90, 96, 100, 108, 120, 125, 128, 135, 144,
                              150, 160, 162, 180, 192, 200, 216, 225, 240, 243, 250, 256], "Predefined offsets")

    crossPage = Param.Bool(True, "Cross page prefetching")
    enable_adaptoffset = Param.Bool(True, "enable adapt offset")
    victimOffsetsListSize = Param.Int(10, "The size of victimOffsetsList")
    restoreCycle = Param.Int(250000, "Cycles which Restore one offset from victimOffsetsList")


class PatternMergingPrefetcher(QueuedPrefetcher):
    # Paper: Merging Similar Patterns for Hardware Prefetching (MICRO 2022)
    type = "PatternMergingPrefetcher"
    cxx_class = "gem5::prefetch::PatternMerging"
    cxx_header = "mem/cache/prefetch/pattern_merging.hh"

    ft_entries = Param.Unsigned(64, "Number of entries in the Filter Table")
    at_entries = Param.Unsigned(
        32, "Number of entries in the Accumulation Table"
    )
    opt_entries = Param.Unsigned(
        64, "Number of entries in the Offset Pattern Table"
    )
    ppt_entries = Param.Unsigned(
        32, "Number of entries in the PC Pattern Table"
    )
    pb_entries = Param.Unsigned(16, "Number of entries in the Prefetch Buffer")
    ft_assoc = Param.Unsigned(8, "Filter Table associativity")
    at_assoc = Param.Unsigned(2, "Accumulation Table associativity")
    pb_assoc = Param.Unsigned(1, "Prefetch Buffer associativity")
    region_size = Param.MemorySize("4KiB", "Spatial region size")
    pattern_length = Param.Unsigned(
        64, "Number of cache lines tracked in each spatial pattern"
    )
    counter_bits = Param.Unsigned(
        5, "Number of bits in each OPT/PPT saturating counter"
    )
    ppt_monitoring_range = Param.Unsigned(
        2, "Number of adjacent offsets monitored by each PPT counter"
    )
    l1_threshold_percent = Param.Percent(
        50, "AFE threshold for high-priority prefetch candidates"
    )
    l2_threshold_percent = Param.Percent(
        15, "AFE threshold for L2 prefetch candidates"
    )
    l2_prefetch_skip_cache_levels = Param.Unsigned(
        1, "Cache levels above an L2-targeted PMP prefetch that do not fill"
    )
    llc_prefetch_skip_cache_levels = Param.Unsigned(
        2, "Cache levels above an LLC-targeted PMP prefetch that do not fill"
    )
    max_prefetches_per_access = Param.Unsigned(
        64, "Maximum number of PMP candidates emitted per observed access"
    )

    queue_squash = True
    queue_filter = True
    cache_snoop = True
    prefetch_on_access = True
    on_write = False
    on_inst = False



class KairosPrefetcher(QueuedPrefetcher):
    type = "KairosPrefetcher"
    cxx_class = "gem5::prefetch::Kairos"
    cxx_header = "mem/cache/prefetch/kairos.hh"

    degree = Param.Unsigned(4, "Max chain-walk prefetches per access")
    kd_size = Param.Unsigned(32, "Detecting Unit entries")
    tu_size = Param.Unsigned(16, "Training Unit entries")
    ht_sets = Param.Unsigned(4096, "Metadata cache sets")
    ht_ways_init = Param.Unsigned(48, "Initial metadata ways per set")
    ht_ways_min = Param.Unsigned(12, "Minimum metadata ways per set")
    ht_ways_max = Param.Unsigned(96, "Maximum metadata ways per set")
    tracking_window = Param.Unsigned(262144, "Accesses per PID window")
    alpha = Param.Float(0.6, "PID alpha (utility weight)")
    beta = Param.Float(-0.3, "PID beta (delta miss-rate weight)")
    gamma = Param.Float(0.1, "PID gamma (second-derivative weight)")
    theta_plus = Param.Float(0.5, "PID positive threshold")
    theta_minus = Param.Float(-0.25, "PID negative threshold")
    tau = Param.Float(1.2, "Miss-rate explosion threshold")

    enable_llc_metadata = Param.Bool(
        False,
        "Count Kairos LLC metadata accesses without injecting real XS traffic",
    )
    metadata_base_addr = Param.Addr(
        0x8000000000,
        "Base physical address of the shadow region used for metadata traffic",
    )
    llc_metadata_ways_init = Param.Unsigned(
        4, "Initial number of LLC physical ways reserved for metadata"
    )
    llc_metadata_ways_min = Param.Unsigned(
        1, "Min number of LLC ways reserved for metadata"
    )
    llc_metadata_ways_max = Param.Unsigned(
        8, "Max number of LLC ways reserved for metadata"
    )
    llc_metadata_initial_ways = VectorParam.Unsigned(
        [],
        "Initial LLC metadata ways; accepted for config compatibility",
    )


class StreamlinePrefetcher(BasePrefetcher):
    type = "StreamlinePrefetcher"
    cxx_class = "gem5::prefetch::Streamline"
    cxx_header = "mem/cache/prefetch/streamline.hh"

    prefetch_on_access = True

    training_unit_assoc = Param.Int(8, "Associativity of the training unit")
    training_unit_entries = Param.MemorySize(
        "256", "Number of per-PC training-unit entries"
    )
    metadata_store_assoc = Param.Int(
        8, "Number of LLC ways reserved per active metadata set"
    )
    metadata_store_entries = Param.MemorySize(
        "16384", "Maximum number of 64B metadata blocks in the store"
    )
    metadata_base_addr = Param.Addr(
        0x8000000000, "Base address of the Streamline shadow metadata region"
    )
    metadata_line_stride = Param.Unsigned(
        2048,
        "Number of LLC cache lines separating Streamline partial-tag groups",
    )
    metadata_buffer_entries = Param.Int(
        3, "Number of per-PC buffered metadata entries"
    )
    max_degree = Param.Int(4, "Maximum Streamline prefetch degree")
    epoch_size = Param.Int(1024, "Per-PC instability epoch for degree control")
    insertions_low_thresh = Param.Int(
        400, "Insertion threshold for degree four"
    )
    insertions_mid_thresh = Param.Int(
        600, "Insertion threshold for degree three"
    )
    insertions_high_thresh = Param.Int(
        800, "Insertion threshold for degree two"
    )
    metadata_port = RequestPort(
        "Dedicated request port for Streamline LLC metadata traffic"
    )

    @cxxMethod
    def debugMetadataEntryTargetCount(self):
        pass

    @cxxMethod
    def debugTriggerFields(self, trigger_hash):
        pass

    @cxxMethod
    def debugDescribeStreamEntry(self, trigger_hash, targets):
        pass

    @cxxMethod
    def debugAppendTrainingAddress(self, current_stream, address):
        pass

    @cxxMethod
    def debugAlignStreams(self, old_stream, new_stream):
        pass

    @cxxMethod
    def debugDegreeForInsertions(self, insertions, max_degree,
                                 low_insertion_threshold,
                                 mid_insertion_threshold,
                                 high_insertion_threshold):
        pass

    @cxxMethod
    def debugMetadataSetCount(self, metadata_entries, metadata_assoc):
        pass

    @cxxMethod
    def debugActiveMetadataSetCount(self, partition_level, max_metadata_sets,
                                    sample_set_count):
        pass

    @cxxMethod
    def debugIsMetadataSetActive(self, metadata_set, partition_level,
                                 max_metadata_sets, sample_set_count):
        pass

    @cxxMethod
    def debugMetadataLineAddress(self, metadata_base, metadata_line_stride,
                                 max_metadata_sets, metadata_set,
                                 partial_tag):
        pass

    @cxxMethod
    def debugRuntimeMetadataLineAddress(self, address):
        pass

    @cxxMethod
    def debugChooseMetadataVictim(self, etrs, valids):
        pass

    @cxxMethod
    def debugMetadataSamplerCoordinates(self, metadata_set):
        pass

    @cxxMethod
    def debugTrainMetadataSampler(self, metadata_set, stream_entry, pc):
        pass

    @cxxMethod
    def debugPredictMetadataSamplerEtr(self, metadata_set, stream_entry):
        pass

    @cxxMethod
    def debugChooseMetadataVictimForEntries(self, metadata_set,
                                            flattened_entries):
        pass

    @cxxMethod
    def debugMetadataHitScore(self, accuracy):
        pass

    @cxxMethod
    def debugSelectPartitionLevel(self, scores, current_level):
        pass

    @cxxMethod
    def debugSampledPartitionLevel(self, metadata_set, max_metadata_sets,
                                   sample_set_count):
        pass

    @cxxMethod
    def debugPackMetadataBlock(self, entries):
        pass

    @cxxMethod
    def debugUnpackMetadataBlock(self, packed_block):
        pass

    @cxxMethod
    def debugUpdateMetadataBuffer(self, current_buffer, stream_entry,
                                  buffer_entries):
        pass

    @cxxMethod
    def debugPlanBufferedPrefetch(self, current_buffer, address, degree):
        pass

    @cxxMethod
    def debugNeedsMetadataRead(self, current_buffer, address):
        pass

    @cxxMethod
    def debugRecordPartitionSample(self, partition_level, score):
        pass

    @cxxMethod
    def debugCurrentPartitionLevel(self):
        pass

    @cxxMethod
    def debugCurrentPartitionLevelStat(self):
        pass

    @cxxMethod
    def debugPartitionTransitionCount(self):
        pass

    @cxxMethod
    def debugPartitionTransitionsStat(self):
        pass

    @cxxMethod
    def debugPartitionScores(self):
        pass

    @cxxMethod
    def debugPartitionSampledAccesses(self):
        pass

    @cxxMethod
    def debugPartitionUpdateInterval(self):
        pass

    @cxxMethod
    def debugResetRuntimeState(self):
        pass

    @cxxMethod
    def debugObserveAccess(self, pc, address):
        pass

class BingoPrefetcher(QueuedPrefetcher):
    # Paper: Bakhshalipour et al., HPCA 2019
    type = 'BingoPrefetcher'
    cxx_class = 'gem5::prefetch::Bingo'
    cxx_header = 'mem/cache/prefetch/bingo.hh'

    region_size = Param.Unsigned(2048, 'Spatial region (page) size in bytes')
    pattern_len = Param.Unsigned(32,
        'Blocks per region (must equal region_size / blkSize)')
    ft_size = Param.Unsigned(64, 'FilterTable entries (fully-assoc, LRU)')
    at_size = Param.Unsigned(128,
        'AccumulationTable entries (fully-assoc, LRU)')
    pht_size = Param.Unsigned(16384,
        'Total PHT entries (must be a multiple of pht_ways, '
        'pht_size/pht_ways must be power of two)')
    pht_ways = Param.Unsigned(16, 'PHT associativity')
    pc_width = Param.Unsigned(16, 'PC bits used in tag/key')
    min_addr_width = Param.Unsigned(5,
        'Offset width in bits (log2(pattern_len))')
    max_addr_width = Param.Unsigned(16,
        'Address bits used in max (PC+Address) tag')
    thresh = Param.Float(0.20,
        'Voting threshold for PC+Offset min-match candidates')
    rotate_pattern = Param.Bool(True,
        'Rotate pattern by -offset on insert / +offset on find')

    prefetch_on_access = True
    prefetch_on_pf_hit = False
    on_inst = False

class XSPhysicalSmallBOP(BOPPrefetcher):
    score_max = 31
    round_max = 50
    bad_score = 1
    rr_size = 256
    tag_bits = 12
    negative_offsets_enable = False
    delay_queue_enable = True
    delay_queue_size = 16
    delay_queue_cycles = 300
    crossPage = False

    offsets = [x for i in [
        1, 2, 3, 4, 5, 6, 8, 9, 10, 12, 15, 16, 18, 20, 24, 25, 27, 30
    ] for x in (i, -i)] + [-32]

class XSVirtualLargeBOP(BOPPrefetcher):
    score_max = 31
    round_max = 50
    bad_score = 2
    rr_size = 256
    tag_bits = 12
    negative_offsets_enable = False
    delay_queue_enable = True
    delay_queue_size = 16
    delay_queue_cycles = 300

    offsets = [
          -117, -147, -91, 117, 147, 91,
          -256, -250, -243, -240, -225, -216, -200,
          -192, -180, -162, -160, -150, -144, -135, -128,
          -125, -120, -108, -100, -96, -90, -81, -80,
          -75, -72, -64, -60, -54, -50, -48, -45,
          -40, -36, -32, -30, -27, -25, -24, -20,
          -18, -16, -15, -12, -10, -9, -8, -6,
          -5, -4, -3, -2, -1,
          1, 2, 3, 4, 5, 6, 8,
          9, 10, 12, 15, 16, 18, 20, 24,
          25, 27, 30, 32, 36, 40, 45, 48,
          50, 54, 60, 64, 72, 75, 80, 81,
          90, 96, 100, 108, 120, 125, 128, 135,
          144, 150, 160, 162, 180, 192, 200, 216,
          225, 240, 243, 250
    ]

class SmallBOPPrefetcher(BOPPrefetcher):
    score_max = 31
    round_max = 30
    bad_score = 8
    rr_size = 256
    tag_bits = 24
    negative_offsets_enable = True
    delay_queue_enable = False

    offsets = [1, 2, 3, 4, 5, 6, 8, 9, 10, 12, 15, 16, 18, 20,
               24, 25, 27, 30, 32, 36, 40, 45, 48, 50, 54, 60, 64]

class LearnedBOPPrefetcher(BOPPrefetcher):
    score_max = 31
    round_max = 30
    bad_score = 8
    rr_size = 256
    tag_bits = 24
    negative_offsets_enable = True
    delay_queue_enable = True
    delay_queue_size = 16
    delay_queue_cycles = 30

    autoLearning = True
    offsets = [64]

class FallenBOPPrefetcher(BOPPrefetcher):
    score_max = 31
    round_max = 30
    bad_score = 8
    rr_size = 256
    tag_bits = 24
    negative_offsets_enable = False
    delay_queue_enable = True
    delay_queue_size = 16
    delay_queue_cycles = 30

    autoLearning = False
    offsets = [24]

class SBOOEPrefetcher(QueuedPrefetcher):
    type = 'SBOOEPrefetcher'
    cxx_class = 'gem5::prefetch::SBOOE'
    cxx_header = "mem/cache/prefetch/sbooe.hh"
    latency_buffer_size = Param.Int(32, "Entries in the latency buffer")
    sequential_prefetchers = Param.Int(9, "Number of sequential prefetchers")
    sandbox_entries = Param.Int(1024, "Size of the address buffer")
    score_threshold_pct = Param.Percent(25, "Min. threshold to issue a \
        prefetch. The value is the percentage of sandbox entries to use")

class STeMSPrefetcher(QueuedPrefetcher):
    type = "STeMSPrefetcher"
    cxx_class = 'gem5::prefetch::STeMS'
    cxx_header = "mem/cache/prefetch/spatio_temporal_memory_streaming.hh"

    spatial_region_size = Param.MemorySize("2KiB",
        "Memory covered by a hot zone")
    active_generation_table_entries = Param.MemorySize("64",
        "Number of entries in the active generation table")
    active_generation_table_assoc = Param.Unsigned(64,
        "Associativity of the active generation table")
    active_generation_table_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(entry_size = 1,
            assoc = Parent.active_generation_table_assoc,
            size = Parent.active_generation_table_entries),
        "Indexing policy of the active generation table")
    active_generation_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the active generation table")

    pattern_sequence_table_entries = Param.MemorySize("16384",
        "Number of entries in the pattern sequence table")
    pattern_sequence_table_assoc = Param.Unsigned(16384,
        "Associativity of the pattern sequence table")
    pattern_sequence_table_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(entry_size = 1,
            assoc = Parent.pattern_sequence_table_assoc,
            size = Parent.pattern_sequence_table_entries),
        "Indexing policy of the pattern sequence table")
    pattern_sequence_table_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(), "Replacement policy of the pattern sequence table")

    region_miss_order_buffer_entries = Param.Unsigned(131072,
        "Number of entries of the Region Miss Order Buffer")
    add_duplicate_entries_to_rmob = Param.Bool(True,
        "Add duplicate entries to RMOB")
    reconstruction_entries = Param.Unsigned(256,
        "Number of reconstruction entries")

class HWPProbeEventRetiredInsts(HWPProbeEvent):
    def register(self):
        if self.obj:
            for name in self.names:
                self.prefetcher.getCCObject().addEventProbeRetiredInsts(
                    self.obj.getCCObject(), name)

class PIFPrefetcher(QueuedPrefetcher):
    type = 'PIFPrefetcher'
    cxx_class = 'gem5::prefetch::PIF'
    cxx_header = "mem/cache/prefetch/pif.hh"
    cxx_exports = [
        PyBindMethod("addEventProbeRetiredInsts"),
    ]

    prec_spatial_region_bits = Param.Unsigned(2,
        "Number of preceding addresses in the spatial region")
    succ_spatial_region_bits = Param.Unsigned(8,
        "Number of subsequent addresses in the spatial region")
    compactor_entries = Param.Unsigned(2, "Entries in the temp. compactor")
    stream_address_buffer_entries = Param.Unsigned(7, "Entries in the SAB")
    history_buffer_size = Param.Unsigned(16, "Entries in the history buffer")

    index_entries = Param.MemorySize("64",
        "Number of entries in the index")
    index_assoc = Param.Unsigned(64,
        "Associativity of the index")
    index_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(entry_size = 1, assoc = Parent.index_assoc,
        size = Parent.index_entries),
        "Indexing policy of the index")
    index_replacement_policy = Param.BaseReplacementPolicy(LRURP(),
        "Replacement policy of the index")

    def listenFromProbeRetiredInstructions(self, simObj):
        if not isinstance(simObj, SimObject):
            raise TypeError("argument must be of SimObject type")
        self.addEvent(HWPProbeEventRetiredInsts(self, simObj,"RetiredInstsPC"))


class IPCPrefetcher(QueuedPrefetcher):
    type = 'IPCPrefetcher'
    cxx_class = 'gem5::prefetch::IPCP'
    cxx_header = 'mem/cache/prefetch/ipcp.hh'

    use_rrf = Param.Bool(True,"")
    degree = Param.Int(4, "Number of prefetches to generate")
    ipt_size = Param.Int(64, "Size of IP Table")
    cspt_size = Param.Int(256, "Szie of CSP Table")


class CMCPrefetcher(QueuedPrefetcher):
    type = "CMCPrefetcher"
    cxx_class = "gem5::prefetch::CMCPrefetcher"
    cxx_header = "mem/cache/prefetch/cmc.hh"

    use_virtual_addresses = True
    prefetch_on_pf_hit = True
    on_read = True
    on_write = False
    on_data  = True
    on_inst  = False

    storage_entries = Param.MemorySize(
        "16384",
        "Number of CMC storage entries"
    )
    storage_assoc = Param.Int(16, "Associativity of the CMC storage table")
    storage_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.storage_assoc,
            size=Parent.storage_entries),
        "Indexing policy of active generation table"
    )
    # constituency_size = Param.Unsigned(32, "constituency_size")
    # team_size = Param.Unsigned(16, "Associativity of the CMC storage table")
    # storage_replacement_policy = Param.BaseReplacementPolicy(
    #     DRRIPRP(constituency_size = Parent.constituency_size,
    #       team_size = Parent.team_size),
    #     "Replacement policy of active generation table"
    # )
    storage_replacement_policy = Param.BaseReplacementPolicy(
        BRRIPRP(),
        "Replacement policy of active generation table"
    )

    degree = Param.Int(4, "prefetch degree")
    enablePrefetchDB = Param.Bool(
        False,
        "Enable prefetch database"
    )


class DespacitoStreamPrefetcher(QueuedPrefetcher):
    type = "DespacitoStreamPrefetcher"
    cxx_class = "gem5::prefetch::DespacitoStreamPrefetcher"
    cxx_header = "mem/cache/prefetch/despacito_stream.hh"

    use_virtual_addresses = False
    prefetch_on_pf_hit = False
    on_read = True
    on_write = False
    on_data  = True
    on_inst  = False

    sample_rate = Param.Int(256, "Sample rate")
    min_distance = Param.Int(4, "Minimum distance")
    max_distance = Param.Int(8192, "Maximum distance")

    sampler_entries = Param.MemorySize(
        "32",
        "num of pattern history table entries"
    )
    sampler_assoc = Param.Int(4, "Associativity of the pattern history table")
    sampler_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.sampler_assoc,
            size=Parent.sampler_entries),
        "Indexing policy of pattern history table"
    )
    sampler_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(),
        "Replacement policy of pattern history table"
    )

    patterns_entries = Param.MemorySize(
        "64",
        "num of pattern history table entries"
    )
    patterns_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.patterns_entries,
            size=Parent.patterns_entries),
        "Indexing policy of pattern history table"
    )
    patterns_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(),
        "Replacement policy of pattern history table"
    )


class XSCompositePrefetcher(QueuedPrefetcher):
    type = "XSCompositePrefetcher"
    cxx_class = 'gem5::prefetch::XSCompositePrefetcher'
    cxx_header = 'mem/cache/prefetch/sms.hh'

    use_virtual_addresses = True
    prefetch_on_pf_hit = True
    on_read = True
    on_write = False
    on_data  = True
    on_inst  = False

    region_size = Param.Int(1024, "region size")

    # TrainFilter configuration
    enable_train_filter = Param.Bool(True, "Enable TrainFilter for ROB-order training")
    training_buffer_size = 8

    # filter table (full-assoc)
    filter_entries = Param.MemorySize("16", "num of filter table entries")
    filter_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.filter_entries,
            size=Parent.filter_entries),
        "Indexing policy of filter table"
    )
    filter_replacement_policy = Param.BaseReplacementPolicy(
        FIFORP(),
        "Replacement policy of filter table"
    )
    # active generation table (full-assoc)
    act_entries = Param.MemorySize(
        "32",
        "num of active generation table entries"
    )
    act_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.act_entries,
            size=Parent.act_entries),
        "Indexing policy of active generation table"
    )
    act_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(),
        "Replacement policy of active generation table"
    )
    re_act_entries = Param.MemorySize(
        "32",
        "num of recently active generation table entries"
    )
    re_act_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.re_act_entries,
            size=Parent.re_act_entries),
        "Indexing policy of recently active generation table"
    )
    sms_filter_entries = Param.MemorySize(
        "16",
        "num of pattern history table entries"
    )
    sms_filter_assoc = Param.Int(16, "Associativity of the pattern history table")
    sms_filter_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.sms_filter_assoc,
            size=Parent.sms_filter_entries),
        "Indexing policy of filter table"
    )
    sms_filter_replacement_policy = Param.BaseReplacementPolicy(
        TreePLRURP(num_leaves=Parent.sms_filter_entries),
        "Replacement policy of filter table"
    )

    stridestream_L1_filter_entries = Param.MemorySize(
        "16",
        "num of pattern history table entries"
    )
    stridestream_L1_filter_assoc = Param.Int(16, "Associativity of the pattern history table")
    stridestream_L1_filter_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.stridestream_L1_filter_assoc,
            size=Parent.stridestream_L1_filter_entries),
        "Indexing policy of filter table"
    )
    stridestream_L1_filter_replacement_policy = Param.BaseReplacementPolicy(
        TreePLRURP(num_leaves=Parent.stridestream_L1_filter_entries),
        "Replacement policy of filter table"
    )
    
    stridestream_L2L3_filter_entries = Param.MemorySize(
        "16",
        "num of pattern history table entries"
    )
    stridestream_L2L3_filter_assoc = Param.Int(16, "Associativity of the pattern history table")
    stridestream_L2L3_filter_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.stridestream_L2L3_filter_assoc,
            size=Parent.stridestream_L2L3_filter_entries),
        "Indexing policy of filter table"
    )
    stridestream_L2L3_filter_replacement_policy = Param.BaseReplacementPolicy(
        TreePLRURP(num_leaves=Parent.stridestream_L2L3_filter_entries),
        "Replacement policy of filter table"
    )
    vaddr_hash_width = Param.Int(5, "Width of virtual address hash")
    re_act_replacement_policy = Param.BaseReplacementPolicy(
        FIFORP(),
        "Replacement policy of recently active generation table"
    )
    stream_pf_ahead = Param.Bool(True, "Prefetch stream region ahead of current region")
    # stride table (full-assoc)
    stride_dyn_depth = Param.Bool(True, "Dynamic depth of stride table")
    stride_entries = Param.MemorySize("32", "Stride Entries")
    stride_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.stride_entries,
            size=Parent.stride_entries),
        "Indexing policy of stride table"
    )
    stride_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(),
        "Replacement policy of stride table"
    )
    fuzzy_stride_matching = Param.Bool(False, "Match stride with fuzzy condition")

    # stride black list
    enable_non_stride_filter= Param.Bool(False, "Prevent non-stride PCs to touch stride table")
    non_stride_entries = Param.MemorySize("256", "Non-Stride Entries")
    non_stride_assoc = Param.Int(4, "Associativity of the non-stride pc table")
    non_stride_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.non_stride_assoc,
            size=Parent.non_stride_entries),
        "Indexing policy of non-stride PC table"
    )
    non_stride_replacement_policy = Param.BaseReplacementPolicy(
        TreePLRURP(num_leaves=Parent.non_stride_assoc),
        "Replacement policy of non-stride pc table"
    )

    # pht table (set-assoc)
    pht_entries = Param.MemorySize(
        "64",
        "num of pattern history table entries"
    )
    pht_assoc = Param.Int(4, "Associativity of the pattern history table")
    pht_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.pht_assoc,
            size=Parent.pht_entries),
        "Indexing policy of pattern history table"
    )
    pht_replacement_policy = Param.BaseReplacementPolicy(
        # TreePLRURP(num_leaves=Parent.pht_entries),
        LRURP(),
        "Replacement policy of pattern history table"
    )
    pht_pf_ahead = Param.Bool(True, "Prefetch pattern region ahead with stride")
    pht_pf_level = Param.Int(2, "Prefetch target level")
    # pf gen table (full-assoc)
    # not implemented now, because queued prefetcher already had a filter
    pf_gen_entries = Param.MemorySize("16", "num of pf_gen entries")
    pf_gen_indexing_policy = Param.BaseIndexingPolicy(
        SetAssociative(
            entry_size=1,
            assoc=Parent.pf_gen_entries,
            size=Parent.pf_gen_entries),
        "Indexing policy of pf_gen"
    )
    pf_gen_replacement_policy = Param.BaseReplacementPolicy(
        LRURP(),
        "Replacement policy of pf_gen"
    )
    bop_large = Param.BasePrefetcher(BOPPrefetcher(is_sub_prefetcher=True),
                                     "Large BOP used in composite prefetcher ")
    bop_small = Param.BasePrefetcher(SmallBOPPrefetcher(is_sub_prefetcher=True),
                                     "Small BOP used in composite prefetcher ")
    bop_learned = Param.BasePrefetcher(LearnedBOPPrefetcher(is_sub_prefetcher=True),
                                       "Learned BOP used in composite prefetcher ")
    bop_pf_level = Param.Int(2, "L1 BOP prefetch target level")
    spp = Param.BasePrefetcher(SignaturePathPrefetcher(is_sub_prefetcher=True),
                               "SPP used in composite prefetcher")
    ipcp = Param.IPCPrefetcher(IPCPrefetcher(use_rrf = False, is_sub_prefetcher=True), "")
    cmc = Param.CMCPrefetcher(CMCPrefetcher(is_sub_prefetcher=True), "")
    berti = Param.BertiPrefetcher(BertiPrefetcher(is_sub_prefetcher=True), "")
    sstride = Param.XSStridePrefetcher(XSStridePrefetcher(is_sub_prefetcher=True), "")
    opt = Param.OptPrefetcher(OptPrefetcher(is_sub_prefetcher=True), "")
    xsstream = Param.XsStreamPrefetcher(XsStreamPrefetcher(is_sub_prefetcher=True), "")

    enable_activepage = Param.Bool(True,"Enable activepage stream prefetcher")
    enable_pht = Param.Bool(True,"Enable sms pht prefetcher")
    enable_cplx = Param.Bool(False, "Enable CPLX component")
    enable_spp = Param.Bool(False, "Enable SPP component")
    enable_temporal = Param.Bool(False, "Enable temporal component")
    enable_berti = Param.Bool(True,"Enable berti component")
    enable_bop = Param.Bool(True, "Enable BOP")

    enable_sstride = Param.Bool(False,"Enable sms stride component")
    enable_opt = Param.Bool(False,"Enable opt component")
    enable_xsstream = Param.Bool(False,"Enable xs_stream component")
    short_stride_thres = Param.Unsigned(512, "Ignore short strides when there are long strides (Bytes)")
    pht_early_update = Param.Bool(True, "Enable update pht earlier")
    neighbor_pht_update = Param.Bool(True, "Enable use nearby act entry to update pht")


class MultiPrefetcher(BasePrefetcher):
    type = 'MultiPrefetcher'
    cxx_class = 'gem5::prefetch::Multi'
    cxx_header = 'mem/cache/prefetch/multi.hh'

    use_virtual_addresses = True
    prefetch_on_pf_hit = True
    on_read = True
    on_write = False
    on_data  = True
    on_inst  = False

    prefetchers = VectorParam.BasePrefetcher([XSCompositePrefetcher(), BOPPrefetcher()],
        "Array of prefetchers")

class IPOPMultiPrefetcher(MultiPrefetcher):
    type = "IPOPMultiPrefetcher"
    cxx_class = "gem5::prefetch::IPOPMulti"
    cxx_header = "mem/cache/prefetch/ipop_multi.hh"
    record_phase_pe_ipc_csv = Param.Bool(
        False,
        "Record each phase's PE values together with the next phase's IPC",
    )
    phase_pe_ipc_csv_path = Param.String(
        "",
        "CSV output path for phase PE and next-phase IPC logging",
    )
    phase_length = Param.Unsigned(1024, "Demand accesses per I-POP phase")
    pfht_entries = Param.Unsigned(512, "Number of PfHT entries")
    poht_entries = Param.Unsigned(512, "Number of PoHT entries")
    table_tag_bits = Param.Unsigned(6, "Tag bits stored in PfHT/PoHT")
    ipop_on_levels = Param.Unsigned(5, "Number of ON aggressiveness levels")
    ipop_off_levels = Param.Unsigned(3, "Number of OFF cooldown levels")
    ideal_dram_latency = Param.Cycles(
        100, "Ideal DRAM access latency used to derive I-POP thresholds"
    )
    phase_on_miss = Param.Bool(
        False,
        "Advance I-POP phases on completed demand misses instead of all demand accesses",
    )
    warmup_phases = Param.Unsigned(
        0,
        "Completed I-POP phases during which ON-to-OFF transitions are suppressed",
    )
    t_noc = Param.Cycles(0, "I-POP NoC contention penalty")
    t_bus = Param.Cycles(1, "I-POP DRAM bus contention penalty")
    t_bank = Param.Cycles(1, "I-POP DRAM bank contention penalty")
    channel_shift = Param.Unsigned(
        0, "Bit position of the least-significant I-POP channel index bit"
    )
    channel_bits = Param.Unsigned(
        0, "Number of I-POP channel index bits; 0 models a single channel"
    )
    bank_shift = Param.Unsigned(
        10,
        "Bit position of the least-significant I-POP bank index bit",
    )
    bank_bits = Param.Unsigned(5, "Number of I-POP bank index bits")


class BanditPrefetcher(MultiPrefetcher):
    type = "BanditPrefetcher"
    cxx_class = "gem5::prefetch::Bandit"
    cxx_header = "mem/cache/prefetch/bandit.hh"

    arm_masks = VectorParam.UInt64(
        [], "Per-arm bitmask over the sub-prefetcher list"
    )
    gamma = Param.Float(0.999, "DUCB discount factor in (0, 1]")
    c = Param.Float(0.04, "Exploration constant")
    bandit_step = Param.UInt64(
        1000, "Main-loop bandit step duration in demand accesses"
    )
    bandit_step_rr = Param.UInt64(
        1000, "Initial round-robin bandit step duration in demand accesses"
    )
    cpu = Param.BaseCPU(NULL, "CPU used to read committed instruction counts")


class L2CompositeWithWorkerPrefetcher(CompositeWithWorkerPrefetcher):
    type = 'L2CompositeWithWorkerPrefetcher'
    cxx_class = 'gem5::prefetch::L2CompositeWithWorkerPrefetcher'
    cxx_header = "mem/cache/prefetch/l2_composite_with_worker.hh"

    cdp = Param.CDP(CDP(is_sub_prefetcher=True), "")
    cmc = Param.CMCPrefetcher(CMCPrefetcher(is_sub_prefetcher=True), "")
    bop_large = Param.BOPPrefetcher(BOPPrefetcher(is_sub_prefetcher=True),
                                     "Large BOP used in composite prefetcher ")
    bop_small = Param.BOPPrefetcher(SmallBOPPrefetcher(is_sub_prefetcher=True),
                                     "Small BOP used in composite prefetcher ")
    despacito_stream = Param.DespacitoStreamPrefetcher(DespacitoStreamPrefetcher(is_sub_prefetcher=True),
                                                       "DespacitoStream used in composite prefetcher")
    enable_bop = Param.Bool(False, "Enable BOP")
    enable_cdp = Param.Bool(True, "Enable CDP")
    enable_cmc = Param.Bool(False, "Enable CMC")
    enable_despacito_stream = Param.Bool(True, "Enable despacito stream")

class L3CompositeWithWorkerPrefetcher(CompositeWithWorkerPrefetcher):
    type = 'L3CompositeWithWorkerPrefetcher'
    cxx_class = 'gem5::prefetch::L3CompositeWithWorkerPrefetcher'
    cxx_header = "mem/cache/prefetch/l3_composite_with_worker.hh"

    bop = Param.BasePrefetcher(FallenBOPPrefetcher(is_sub_prefetcher=True), "")
