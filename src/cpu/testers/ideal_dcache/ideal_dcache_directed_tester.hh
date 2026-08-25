#ifndef __CPU_TESTERS_IDEAL_DCACHE_DIRECTED_TESTER_HH__
#define __CPU_TESTERS_IDEAL_DCACHE_DIRECTED_TESTER_HH__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "base/statistics.hh"
#include "mem/port.hh"
#include "params/IdealDCacheDirectedTester.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5
{

class System;
namespace memory
{
class AbstractMemory;
}

class IdealDCacheDirectedTester : public ClockedObject
{
  public:
    using Params = IdealDCacheDirectedTesterParams;

    IdealDCacheDirectedTester(const Params &p);

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;
    void startup() override;

  private:
    enum class Mode
    {
        FunctionalIsolation,
        QueuedStateCarrier,
        GenericFunctionalReconciliation,
        OverlapFunctionalWriteback
    };

    enum class PortKind
    {
        External,
        Normal,
        Ideal
    };

    class TestPort : public RequestPort
    {
      public:
        TestPort(const std::string &name, IdealDCacheDirectedTester &owner,
                 PortKind kind);

        void sendPacket(PacketPtr pkt);
        void sendFunctionalPacket(PacketPtr pkt) const;

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;

      private:
        IdealDCacheDirectedTester &owner;
        const PortKind kind;
        PacketPtr retryPkt = nullptr;
    };

    class VisitorProbeCpuSidePort : public ResponsePort
    {
      public:
        VisitorProbeCpuSidePort(
            const std::string &name, IdealDCacheDirectedTester &owner);

      protected:
        Tick recvAtomic(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;
        bool recvTimingReq(PacketPtr pkt) override;
        bool tryTiming(PacketPtr pkt) override;
        bool recvTimingSnoopResp(PacketPtr pkt) override;
        void recvRespRetry() override;
        AddrRangeList getAddrRanges() const override;

      private:
        IdealDCacheDirectedTester &owner;
    };

    class VisitorProbeMemSidePort : public RequestPort
    {
      public:
        VisitorProbeMemSidePort(
            const std::string &name, IdealDCacheDirectedTester &owner);

        bool isSnooping() const override;

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
        void recvRangeChange() override;
        Tick recvAtomicSnoop(PacketPtr pkt) override;
        void recvFunctionalSnoop(PacketPtr pkt) override;
        void recvTimingSnoopReq(PacketPtr pkt) override;
        void recvRetrySnoopResp() override;

      private:
        IdealDCacheDirectedTester &owner;
    };

    struct TesterStats : public statistics::Group
    {
        explicit TesterStats(statistics::Group *parent);

        statistics::Scalar maskedWritesWhilePending;
        statistics::Scalar preservedQueuedResponses;
        statistics::Scalar maskedMergeChecks;
        statistics::Scalar sameLineMshrTargetChecks;
        statistics::Scalar queuedStateCarriers;
        statistics::Scalar stateCarrierDataChecks;
        statistics::Scalar genericTimingWriteResponses;
        statistics::Scalar genericQueuedResponseUpdates;
        statistics::Scalar genericOracleFlagChecks;
        statistics::Scalar overlapNormalBlockChecks;
        statistics::Scalar overlapVisitorDataChecks;
        statistics::Scalar overlapVisitorFlagChecks;
        statistics::Scalar overlapRawMemoryChecks;
        statistics::Scalar overlapOracleChecks;
    };

    static constexpr size_t AccessSize = 8;
    static constexpr size_t CarrierUpdateOffset = 16;

    static Mode parseMode(const std::string &mode);

    void start();
    void startFunctionalIsolation();
    void startQueuedStateCarrier();
    void startGenericFunctionalReconciliation();
    void startOverlapFunctionalWriteback();
    void performAction();
    void performMaskedFunctionalWrite();
    void performIdealCarrierUpdate();
    void performGenericFunctionalWrite();
    void verifyQueuedStateCarrier();
    void verifyOverlapFunctionalWriteback();
    void testTimeout();
    void forwardVisitorFunctional(PacketPtr pkt);

    void packetAccepted(PortKind kind, const RequestPtr &request);
    void recvTimingResp(PortKind kind, PacketPtr pkt);
    void maybeScheduleFunctionalAction();
    void maybeIssueFunctionalFinalRead();
    void maybePassOverlapFunctionalWriteback();
    void pass();

    PacketPtr makeRead(Addr addr, size_t size, RequestPtr &request);
    PacketPtr makeWrite(Addr addr, const std::vector<uint8_t> &data,
                        RequestPtr &request,
                        const std::vector<bool> &byte_enable = {});
    PacketPtr makeWriteAs(Addr addr, const std::vector<uint8_t> &data,
                          RequestorID requestor_id, RequestPtr &request);
    PacketPtr makeReadAs(Addr addr, size_t size,
                         RequestorID requestor_id, RequestPtr &request);
    PacketPtr makeStateCarrier();
    bool packetDataEquals(PacketPtr pkt,
                          const std::vector<uint8_t> &expected) const;

    System *const system;
    const Mode mode;
    const Addr testAddr;
    const Addr mshrAddr;
    const Addr genericAddr;
    memory::AbstractMemory *const deviceMemory;
    const Tick actionDelay;
    const Tick verifyDelay;
    const Tick timeoutDelay;
    const RequestorID requestorId;
    const RequestorID oracleRequestorId;
    const size_t lineSize;

    EventFunctionWrapper startEvent;
    EventFunctionWrapper actionEvent;
    EventFunctionWrapper verifyEvent;
    EventFunctionWrapper postWritebackEvent;
    EventFunctionWrapper timeoutEvent;

    TestPort externalPort;
    TestPort normalPort;
    TestPort idealPort;
    VisitorProbeCpuSidePort visitorProbeCpuSidePort;
    VisitorProbeMemSidePort visitorProbeMemSidePort;

    std::vector<uint8_t> functionalInitial;
    std::vector<uint8_t> functionalPayload;
    std::vector<uint8_t> functionalExpected;
    std::vector<bool> functionalMask;
    std::vector<uint8_t> carrierInitial;
    std::vector<uint8_t> carrierUpdate;
    std::vector<uint8_t> carrierExpected;
    std::vector<uint8_t> genericInitial;
    std::vector<uint8_t> genericPayload;
    std::vector<uint8_t> overlapOracleLine;
    std::vector<uint8_t> overlapDeviceLine;
    std::vector<uint8_t> overlapDeviceUpdate;
    std::vector<uint8_t> overlapExpectedLine;

    RequestPtr normalWriteRequest;
    RequestPtr canaryReadRequest;
    RequestPtr functionalFinalReadRequest;
    RequestPtr carrierRequest;
    RequestPtr idealWriteRequest;
    RequestPtr carrierFinalReadRequest;
    RequestPtr genericWriteRequest;
    RequestPtr genericReadRequest;
    RequestPtr overlapOracleSeedRequest;
    RequestPtr overlapNormalWriteRequest;
    RequestPtr overlapNormalReadRequest;
    RequestPtr overlapRawReadRequest;
    RequestPtr overlapOracleReadRequest;

    bool normalWriteAccepted = false;
    bool canaryReadAccepted = false;
    bool normalWriteReceived = false;
    bool canaryReadReceived = false;
    bool actionPerformed = false;
    bool carrierAccepted = false;
    bool idealWriteReceived = false;
    bool genericWriteAccepted = false;
    bool genericWriteReceived = false;
    bool genericReadAccepted = false;
    bool genericReadReceived = false;
    bool overlapOracleSeedReceived = false;
    bool overlapNormalWriteReceived = false;
    bool overlapNormalReadReceived = false;
    bool overlapVisitorSeen = false;
    bool overlapRawReadReceived = false;
    bool overlapOracleReadReceived = false;

    TesterStats stats;
};

} // namespace gem5

#endif // __CPU_TESTERS_IDEAL_DCACHE_DIRECTED_TESTER_HH__
