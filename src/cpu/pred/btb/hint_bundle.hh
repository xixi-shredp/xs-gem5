#ifndef __CPU_PRED_BTB_HINT_BUNDLE_HH__
#define __CPU_PRED_BTB_HINT_BUNDLE_HH__
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <variant>
#include <utility>
#include "base/types.hh"
namespace gem5::branch_prediction::btb_pred {
class HintBundleController {
  public:
    struct DmaToken { uint8_t slot=0; uint64_t generation=0; };
    using DmaPayload = std::array<uint8_t,64>;
    using DmaDone = std::function<void(DmaToken,bool,uint64_t)>;
    struct TraceAudit {
        std::string event;
        uint64_t cycle=0,responseCycle=0,retireCycle=0,recordIndex=0,reservationIndex=0,sequence=0;
        Addr sourcePC=0,target=0;
        int logicalMetadata=-1,snapshotMetadata=-1;
        unsigned pcId=0;
        bool boundary=false,queryValid=false;
    };
    struct Hooks {
        using DmaAcquire = std::function<std::optional<DmaToken>()>;
        using DmaPayloadAccess = std::function<DmaPayload &(DmaToken)>;
        using DmaSubmit = std::function<bool(DmaToken,Addr,bool,unsigned,DmaDone)>;
        using DmaRelease = std::function<void(DmaToken)>;
        DmaAcquire dmaAcquire;
        DmaPayloadAccess dmaPayload;
        DmaSubmit dmaSubmit;
        DmaRelease dmaRelease;
        std::function<bool()> quiescent;
        std::function<void()> clearLocal;
        std::function<bool(const std::vector<uint8_t>&,unsigned)> loadImage;
        std::function<bool(unsigned)> beginInstall;
        std::function<bool(unsigned,uint64_t)> installRow;
        std::function<bool(unsigned)> finishInstall;
        std::function<void(bool)> admission;
        std::function<void(unsigned,bool)> profileStage;
        // Simulation audit only; no extra identity in production Trace/table.
        std::function<void(const std::string&,uint64_t,Addr,Addr,int,int)> audit;
        std::function<void(const TraceAudit&)> traceAudit;
    };
    explicit HintBundleController(Hooks hooks);
    void setDmaPool(Hooks::DmaAcquire acquire, Hooks::DmaPayloadAccess payload,
                    Hooks::DmaSubmit submit, Hooks::DmaRelease release) {
        hooks.dmaAcquire=std::move(acquire);hooks.dmaPayload=std::move(payload);
        hooks.dmaSubmit=std::move(submit);hooks.dmaRelease=std::move(release);
    }
    bool activeOperation() const { return busy() || (status & (Started|Enabled)); }
    uint64_t readCsr(unsigned csr) const;
    void writeCsr(unsigned csr,uint64_t value);
    void tick(uint64_t cycle);
    void committedBoundary(Addr pc,Addr target,uint64_t seq=0,unsigned size=2);
    void retiredInstruction();
    bool recordDiscovery(Addr window,unsigned position,bool call,uint64_t seq=0,Addr source_pc=0);
    bool recordDetail(unsigned pc_id,Addr target,const std::array<uint32_t,4>& contexts,
                      bool baseline_miss,bool query_valid,int snapshot_metadata=-1,
                      uint64_t seq=0,Addr source_pc=0);
    bool bundleEnabled() const { return status & Bundle; }
    bool admissionEnabled() const { return admitted; }
    bool hintEnabled() const { return admitted && (status & Enabled); }
    bool detailEnabled() const { return admitted && (status & Detail); }
    std::optional<unsigned> activeMetadata() const { return active; }
    std::optional<unsigned> desiredMetadata() const { return desired; }
    std::optional<unsigned> logicalMetadata() const { return logicalOwner; }
    bool busy() const;
    uint64_t reservedRecords() const { return reserved; }
    uint64_t writtenRecords() const { return written; }
    size_t bufferedLoadBytes() const;
    size_t bufferedTraceBytes() const;
    unsigned leasedSlots() const;
    struct BufferStats {
        unsigned boundaryOwnerPeak=0,readBufferPeakBytes=0;
        unsigned sharedSlotPeak=0,readSlotPeak=0,writeSlotPeak=0;
        unsigned sharedPayloadPeakBytes=0,writeBufferPeakBytes=0;
        unsigned completionDescriptorPeak=0,maxRowBytesPerTick=0,maxPackBytesPerTick=0;
        unsigned maxWriteRetireSlotsPerTick=0,maxWriteRetireRecordsPerTick=0,maxSubmitPerTick=0;
    };
    BufferStats bufferStats() const { return bufferPeaks; }
    unsigned occupiedBoundarySlots() const {
        unsigned count=0;for(const auto& slot:boundaryOwners)count+=slot.used;
        return count;
    }
    struct Counters { uint64_t switches,aborted,unavailable,routeMisses,ordinaryLost,boundaryLost; };
    Counters counters() const { return {switches,aborted,unavailable,routeMisses,ordinaryLost,boundaryLost}; }
  private:
    static constexpr uint64_t Valid=1ULL<<63, Enabled=1ULL<<62,
        Discovery=1ULL<<61, Detail=1ULL<<60, Load=1ULL<<59,
        Ignore=1ULL<<55, Bundle=1ULL<<51, Busy=1ULL<<50;
    static constexpr uint64_t Started=Discovery|Detail|Load;
    enum class State { Idle, Route, Drain, Image, Install, Abort, ErrorDrain };
    struct Boundary { Addr pc,target; uint64_t sequence=0; unsigned size=2; int owner=-1; };
    using Record=std::array<uint8_t,22>;
    struct Trace { Record data{}; uint64_t slot=0; bool boundary=false; TraceAudit audit{}; int owner=-1; };
    // At most three 22B record endings fit in a <=64B contiguous packet.
    struct TraceEnd { TraceAudit audit{}; uint64_t reservation=0; bool boundary=false; unsigned byteEnd=0; };
    enum class SlotState { Free, Reserved, Filling, Ready, Submitted, Responded };
    struct ReadMeta { bool epoch=false; unsigned consumed=0; };
    struct WriteMeta { std::array<TraceEnd,3> ends{}; unsigned endCount=0; };
    struct DmaSlot {
        DmaToken token{};
        uint64_t at=0,responseCycle=0;
        unsigned bytes=0;
        SlotState state=SlotState::Free;
        bool success=false;
        std::variant<ReadMeta,WriteMeta> meta{ReadMeta{false,0}};
        bool write() const { return std::holds_alternative<WriteMeta>(meta); }
    };
    Hooks hooks;
    uint64_t status=0,paddr=0,laddr=0,now=0,budget=0,reserved=0,written=0;
    unsigned kind=0,lerror=0,perror=0;
    bool admitted=false,prepared=false,partial=false,stopping=false,finalQueued=false;
    bool initialLoad=false,tableReady=false;
    bool initialValid=false; unsigned initialId=0; uint32_t validMask=0;
    std::optional<unsigned> desired,active,loading,logicalOwner;
    State state=State::Idle;
    Boundary latestBoundary{};
    std::array<uint64_t,32> boundaryTable{};
    std::array<uint32_t,512> bundleTable{};
    static constexpr unsigned BoundaryCapacity=64;
    // The two host queues are dual consumers of the SAME64 physical slots.
    // A slot is reusable only after route consumption AND transfer to fold.
    struct BoundaryOwner { bool used=false,routeDone=false,traceDone=false; };
    std::array<BoundaryOwner,BoundaryCapacity> boundaryOwners{};
    std::deque<Boundary> boundaries;
    std::deque<Trace> boundaryTrace,ordinaryTrace;
    std::optional<Trace> fold,writing;
    unsigned writeOffset=0;
    std::array<DmaSlot,64> dmaSlots{};
    uint64_t packedBytes=0,acknowledgedBytes=0;
    std::optional<uint64_t> failedWriteAt;
    bool traceFault=false,loadEpoch=false,preferRead=false;
    BufferStats bufferPeaks{};
    uint64_t offset=0,nextIssueOffset=0,total=0;
    unsigned installRow=0;
    // Only a16B header copy; all read/Trace bytes live in CPU's shared pool.
    std::vector<uint8_t> image;
    bool innerStarted=false;
    uint64_t switches=0,aborted=0,unavailable=0,routeMisses=0,ordinaryLost=0,boundaryLost=0;
    void setAdmission(bool value);
    void fail(unsigned load_error,unsigned profile_error=0);
    void beginLoad();
    void beginSwitch();
    bool validateRoutes();
    bool consumeRouteBeat();
    bool consumeImageBeat();
    void activateImage();
    void serviceTrace();
    void retireTrace();
    void submitDma();
    void receiveDma(DmaToken token,bool success,uint64_t responseCycle);
    int acquireSlot(bool write);
    void releaseSlot(unsigned index);
    bool readsEmpty() const;
    bool traceBuffersEmpty() const;
    int currentRead() const;
    void finishReadRow(unsigned index,unsigned bytes);
    void discardRead();
    void observeBuffers();
    void processBoundary();
    void releaseBoundaryOwner(int owner,bool route);
    void discardBoundaryControls();
    std::optional<unsigned> route(Addr pc,Addr target) const;
    bool profiling() const { return status & (Discovery|Detail); }
    bool acceptRecord(const Record&,bool boundary,TraceAudit audit);
    void stopProfile(unsigned error);
    uint64_t capacity() const { return ((status>>15)&4095)*(1ULL<<20)/22; }
    static uint64_t word(const std::vector<uint8_t>& bytes,unsigned at,unsigned count);
    static uint64_t payloadWord(const DmaPayload&,unsigned at,unsigned count);
    static void field(Record&,unsigned low,unsigned width,uint64_t value);
};
}
#endif
