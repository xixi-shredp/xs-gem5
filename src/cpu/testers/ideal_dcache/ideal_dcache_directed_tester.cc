#include "cpu/testers/ideal_dcache/ideal_dcache_directed_tester.hh"

#include <algorithm>
#include <cassert>

#include "base/logging.hh"
#include "base/statistics.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

namespace gem5
{

IdealDCacheDirectedTester::Mode
IdealDCacheDirectedTester::parseMode(const std::string &mode)
{
    if (mode == "functional-isolation") {
        return Mode::FunctionalIsolation;
    }
    if (mode == "queued-state-carrier") {
        return Mode::QueuedStateCarrier;
    }
    if (mode == "generic-functional-reconciliation") {
        return Mode::GenericFunctionalReconciliation;
    }
    if (mode == "overlap-functional-reconciliation") {
        return Mode::GenericFunctionalReconciliation;
    }
    if (mode == "overlap-functional-writeback") {
        return Mode::OverlapFunctionalWriteback;
    }
    fatal("Unknown ideal-DCache directed tester mode: %s", mode);
}

IdealDCacheDirectedTester::TestPort::TestPort(
    const std::string &name, IdealDCacheDirectedTester &owner, PortKind kind)
    : RequestPort(name, &owner), owner(owner), kind(kind)
{
}

void
IdealDCacheDirectedTester::TestPort::sendPacket(PacketPtr pkt)
{
    panic_if(retryPkt, "%s already has a packet waiting for retry", name());
    const RequestPtr request = pkt->req;
    if (sendTimingReq(pkt)) {
        owner.packetAccepted(kind, request);
    } else {
        retryPkt = pkt;
    }
}

void
IdealDCacheDirectedTester::TestPort::sendFunctionalPacket(PacketPtr pkt) const
{
    sendFunctional(pkt);
}

bool
IdealDCacheDirectedTester::TestPort::recvTimingResp(PacketPtr pkt)
{
    owner.recvTimingResp(kind, pkt);
    return true;
}

void
IdealDCacheDirectedTester::TestPort::recvReqRetry()
{
    assert(retryPkt);
    PacketPtr pkt = retryPkt;
    const RequestPtr request = pkt->req;
    if (sendTimingReq(pkt)) {
        retryPkt = nullptr;
        owner.packetAccepted(kind, request);
    }
}

IdealDCacheDirectedTester::VisitorProbeCpuSidePort::
VisitorProbeCpuSidePort(
    const std::string &name, IdealDCacheDirectedTester &owner)
    : ResponsePort(name, &owner), owner(owner)
{
}

Tick
IdealDCacheDirectedTester::VisitorProbeCpuSidePort::recvAtomic(PacketPtr pkt)
{
    return owner.visitorProbeMemSidePort.sendAtomic(pkt);
}

void
IdealDCacheDirectedTester::VisitorProbeCpuSidePort::recvFunctional(
    PacketPtr pkt)
{
    owner.forwardVisitorFunctional(pkt);
}

bool
IdealDCacheDirectedTester::VisitorProbeCpuSidePort::recvTimingReq(
    PacketPtr pkt)
{
    return owner.visitorProbeMemSidePort.sendTimingReq(pkt);
}

bool
IdealDCacheDirectedTester::VisitorProbeCpuSidePort::tryTiming(PacketPtr pkt)
{
    return owner.visitorProbeMemSidePort.tryTiming(pkt);
}

bool
IdealDCacheDirectedTester::VisitorProbeCpuSidePort::recvTimingSnoopResp(
    PacketPtr pkt)
{
    return owner.visitorProbeMemSidePort.sendTimingSnoopResp(pkt);
}

void
IdealDCacheDirectedTester::VisitorProbeCpuSidePort::recvRespRetry()
{
    owner.visitorProbeMemSidePort.sendRetryResp();
}

AddrRangeList
IdealDCacheDirectedTester::VisitorProbeCpuSidePort::getAddrRanges() const
{
    return owner.visitorProbeMemSidePort.getAddrRanges();
}

IdealDCacheDirectedTester::VisitorProbeMemSidePort::
VisitorProbeMemSidePort(
    const std::string &name, IdealDCacheDirectedTester &owner)
    : RequestPort(name, &owner), owner(owner)
{
}

bool
IdealDCacheDirectedTester::VisitorProbeMemSidePort::isSnooping() const
{
    return owner.visitorProbeCpuSidePort.isSnooping();
}

bool
IdealDCacheDirectedTester::VisitorProbeMemSidePort::recvTimingResp(
    PacketPtr pkt)
{
    return owner.visitorProbeCpuSidePort.sendTimingResp(pkt);
}

void
IdealDCacheDirectedTester::VisitorProbeMemSidePort::recvReqRetry()
{
    owner.visitorProbeCpuSidePort.sendRetryReq();
}

void
IdealDCacheDirectedTester::VisitorProbeMemSidePort::recvRangeChange()
{
    owner.visitorProbeCpuSidePort.sendRangeChange();
}

Tick
IdealDCacheDirectedTester::VisitorProbeMemSidePort::recvAtomicSnoop(
    PacketPtr pkt)
{
    return owner.visitorProbeCpuSidePort.sendAtomicSnoop(pkt);
}

void
IdealDCacheDirectedTester::VisitorProbeMemSidePort::recvFunctionalSnoop(
    PacketPtr pkt)
{
    owner.visitorProbeCpuSidePort.sendFunctionalSnoop(pkt);
}

void
IdealDCacheDirectedTester::VisitorProbeMemSidePort::recvTimingSnoopReq(
    PacketPtr pkt)
{
    owner.visitorProbeCpuSidePort.sendTimingSnoopReq(pkt);
}

void
IdealDCacheDirectedTester::VisitorProbeMemSidePort::recvRetrySnoopResp()
{
    owner.visitorProbeCpuSidePort.sendRetrySnoopResp();
}

IdealDCacheDirectedTester::TesterStats::TesterStats(
    statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(maskedWritesWhilePending,
               statistics::units::Count::get(),
               "Masked functional writes issued while timing responses wait"),
      ADD_STAT(preservedQueuedResponses,
               statistics::units::Count::get(),
               "Queued timing responses retaining their pre-write data"),
      ADD_STAT(maskedMergeChecks,
               statistics::units::Count::get(),
               "Post-functional reads matching the byte-enable merge"),
      ADD_STAT(sameLineMshrTargetChecks,
               statistics::units::Count::get(),
               "Final reads validating a same-line pending MSHR target"),
      ADD_STAT(queuedStateCarriers,
               statistics::units::Count::get(),
               "Stale WriteClean carriers admitted to the delay queue"),
      ADD_STAT(stateCarrierDataChecks,
               statistics::units::Count::get(),
               "Raw-memory reads matching the rebased state carrier"),
      ADD_STAT(genericTimingWriteResponses,
               statistics::units::Count::get(),
               "Non-oracle timing writes completed without oracle commits"),
      ADD_STAT(genericQueuedResponseUpdates,
               statistics::units::Count::get(),
               "Non-oracle queued responses updated by functional writes"),
      ADD_STAT(genericOracleFlagChecks,
               statistics::units::Count::get(),
               "Non-oracle functional writes without oracle request flags"),
      ADD_STAT(overlapNormalBlockChecks,
               statistics::units::Count::get(),
               "Device-provenance blocks read back through a normal L1D"),
      ADD_STAT(overlapVisitorDataChecks,
               statistics::units::Count::get(),
               "Functional writeback visitors carrying device block data"),
      ADD_STAT(overlapVisitorFlagChecks,
               statistics::units::Count::get(),
               "Functional writeback visitors retaining non-oracle flags"),
      ADD_STAT(overlapRawMemoryChecks,
               statistics::units::Count::get(),
               "Raw RAM reads matching device-provenance writeback data"),
      ADD_STAT(overlapOracleChecks,
               statistics::units::Count::get(),
               "Canonical reads unchanged by device-provenance writeback")
{
}

IdealDCacheDirectedTester::IdealDCacheDirectedTester(const Params &p)
    : ClockedObject(p),
      system(p.system),
      mode(parseMode(p.mode)),
      testAddr(p.test_addr),
      mshrAddr(p.mshr_addr),
      genericAddr(p.generic_addr),
      deviceMemory(p.device_memory),
      actionDelay(p.action_delay),
      verifyDelay(p.verify_delay),
      timeoutDelay(p.timeout),
      requestorId(p.system->getRequestorId(this)),
      oracleRequestorId(mode == Mode::OverlapFunctionalWriteback ?
          p.system->getRequestorId(this, "oracle") : requestorId),
      lineSize(p.system->cacheLineSize()),
      startEvent([this] { start(); }, name() + ".start"),
      actionEvent([this] { performAction(); }, name() + ".action"),
      verifyEvent([this] { verifyQueuedStateCarrier(); },
                  name() + ".verify"),
      postWritebackEvent([this] { verifyOverlapFunctionalWriteback(); },
                         name() + ".post_writeback"),
      timeoutEvent([this] { testTimeout(); }, name() + ".timeout"),
      externalPort(name() + ".external_port", *this, PortKind::External),
      normalPort(name() + ".normal_port", *this, PortKind::Normal),
      idealPort(name() + ".ideal_port", *this, PortKind::Ideal),
      visitorProbeCpuSidePort(name() + ".visitor_probe_cpu_side_port", *this),
      visitorProbeMemSidePort(name() + ".visitor_probe_mem_side_port", *this),
      stats(this)
{
    fatal_if(lineSize < CarrierUpdateOffset + AccessSize,
             "Cache line is too small for the state-carrier test");
    fatal_if(testAddr % lineSize != 0,
             "Test address %#x is not cache-line aligned", testAddr);
    fatal_if(mshrAddr % lineSize != 0,
             "MSHR address %#x is not cache-line aligned", mshrAddr);
    fatal_if(mode == Mode::FunctionalIsolation && testAddr != mshrAddr,
             "Functional-isolation test and MSHR addresses must share a line");
    fatal_if(actionDelay == 0 || verifyDelay <= actionDelay,
             "Directed tester delays are not ordered");
    fatal_if(timeoutDelay <= verifyDelay,
             "Directed tester timeout must follow its verification delay");
    fatal_if(mode == Mode::OverlapFunctionalWriteback && !deviceMemory,
             "Overlap functional-writeback mode requires device_memory");
    if (mode == Mode::OverlapFunctionalWriteback) {
        system->addDeviceMemory(requestorId, deviceMemory);
    }

    functionalInitial = {0x10, 0x11, 0x12, 0x13,
                         0x14, 0x15, 0x16, 0x17};
    functionalPayload = {0xa0, 0xa1, 0xa2, 0xa3,
                         0xa4, 0xa5, 0xa6, 0xa7};
    functionalMask = {true, false, true, false,
                      false, true, false, true};
    functionalExpected = functionalInitial;
    for (size_t i = 0; i < AccessSize; ++i) {
        if (functionalMask[i]) {
            functionalExpected[i] = functionalPayload[i];
        }
    }
    // The pending normal write is submitted after the initial line value and
    // is the last target merged into the final line.
    functionalExpected[1] = 0xb1;

    carrierInitial.resize(lineSize);
    for (size_t i = 0; i < lineSize; ++i) {
        carrierInitial[i] = 0x40 + (i % 0x20);
    }
    carrierUpdate = {0xd0, 0xd1, 0xd2, 0xd3,
                     0xd4, 0xd5, 0xd6, 0xd7};
    carrierExpected = carrierInitial;
    std::copy(carrierUpdate.begin(), carrierUpdate.end(),
              carrierExpected.begin() + CarrierUpdateOffset);

    genericInitial = {0x20, 0x21, 0x22, 0x23,
                      0x24, 0x25, 0x26, 0x27};
    genericPayload = {0xe0, 0xe1, 0xe2, 0xe3,
                      0xe4, 0xe5, 0xe6, 0xe7};

    overlapOracleLine.resize(lineSize);
    overlapDeviceLine.resize(lineSize);
    for (size_t i = 0; i < lineSize; ++i) {
        overlapOracleLine[i] = 0x80 + (i % 0x20);
        overlapDeviceLine[i] = 0x30 + (i % 0x20);
    }
    overlapDeviceUpdate = {0xf0, 0xf1, 0xf2, 0xf3,
                           0xf4, 0xf5, 0xf6, 0xf7};
    overlapExpectedLine = overlapDeviceLine;
    std::copy(overlapDeviceUpdate.begin(), overlapDeviceUpdate.end(),
              overlapExpectedLine.begin());
}

Port &
IdealDCacheDirectedTester::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "external_port") {
        return externalPort;
    }
    if (if_name == "normal_port") {
        return normalPort;
    }
    if (if_name == "ideal_port") {
        return idealPort;
    }
    if (if_name == "visitor_probe_cpu_side_port") {
        return visitorProbeCpuSidePort;
    }
    if (if_name == "visitor_probe_mem_side_port") {
        return visitorProbeMemSidePort;
    }
    return ClockedObject::getPort(if_name, idx);
}

void
IdealDCacheDirectedTester::startup()
{
    schedule(startEvent, nextCycle());
}

PacketPtr
IdealDCacheDirectedTester::makeRead(
    Addr addr, size_t size, RequestPtr &request)
{
    return makeReadAs(addr, size, requestorId, request);
}

PacketPtr
IdealDCacheDirectedTester::makeReadAs(
    Addr addr, size_t size, RequestorID requestor_id, RequestPtr &request)
{
    request = std::make_shared<Request>(addr, size, 0, requestor_id);
    PacketPtr pkt = new Packet(request, MemCmd::ReadReq);
    pkt->allocate();
    return pkt;
}

PacketPtr
IdealDCacheDirectedTester::makeWrite(
    Addr addr, const std::vector<uint8_t> &data, RequestPtr &request,
    const std::vector<bool> &byte_enable)
{
    PacketPtr pkt = makeWriteAs(addr, data, requestorId, request);
    if (!byte_enable.empty()) {
        request->setByteEnable(byte_enable);
    }
    return pkt;
}

PacketPtr
IdealDCacheDirectedTester::makeWriteAs(
    Addr addr, const std::vector<uint8_t> &data,
    RequestorID requestor_id, RequestPtr &request)
{
    request = std::make_shared<Request>(
        addr, data.size(), 0, requestor_id);
    PacketPtr pkt = new Packet(request, MemCmd::WriteReq);
    auto *pkt_data = new uint8_t[data.size()];
    std::copy(data.begin(), data.end(), pkt_data);
    pkt->dataDynamic(pkt_data);
    return pkt;
}

PacketPtr
IdealDCacheDirectedTester::makeStateCarrier()
{
    carrierRequest = std::make_shared<Request>(
        testAddr, lineSize, 0, Request::wbRequestorId);
    PacketPtr pkt = new Packet(carrierRequest, MemCmd::WriteClean);
    auto *pkt_data = new uint8_t[lineSize];
    std::copy(carrierInitial.begin(), carrierInitial.end(), pkt_data);
    pkt->dataDynamic(pkt_data);
    return pkt;
}

bool
IdealDCacheDirectedTester::packetDataEquals(
    PacketPtr pkt, const std::vector<uint8_t> &expected) const
{
    return pkt->hasData() && pkt->getSize() == expected.size() &&
        std::equal(expected.begin(), expected.end(),
                   pkt->getConstPtr<uint8_t>());
}

void
IdealDCacheDirectedTester::start()
{
    schedule(timeoutEvent, curTick() + timeoutDelay);
    if (mode == Mode::FunctionalIsolation) {
        startFunctionalIsolation();
    } else if (mode == Mode::QueuedStateCarrier) {
        startQueuedStateCarrier();
    } else if (mode == Mode::OverlapFunctionalWriteback) {
        startOverlapFunctionalWriteback();
    } else {
        startGenericFunctionalReconciliation();
    }
}

void
IdealDCacheDirectedTester::startFunctionalIsolation()
{
    RequestPtr init_request;
    PacketPtr init_pkt = makeWrite(
        testAddr, functionalInitial, init_request);
    externalPort.sendFunctionalPacket(init_pkt);
    fatal_if(!init_pkt->isResponse() || init_pkt->isError(),
             "Initial functional write failed: %s", init_pkt->print());
    delete init_pkt;

    normalPort.sendPacket(
        makeWrite(testAddr + 1, {0xb1}, normalWriteRequest));
    externalPort.sendPacket(
        makeRead(testAddr, AccessSize, canaryReadRequest));
}

void
IdealDCacheDirectedTester::startQueuedStateCarrier()
{
    RequestPtr init_request;
    PacketPtr init_pkt = makeWrite(
        testAddr, carrierInitial, init_request);
    externalPort.sendFunctionalPacket(init_pkt);
    fatal_if(!init_pkt->isResponse() || init_pkt->isError(),
             "Initial carrier-line functional write failed: %s",
             init_pkt->print());
    delete init_pkt;

    externalPort.sendPacket(makeStateCarrier());
}

void
IdealDCacheDirectedTester::startGenericFunctionalReconciliation()
{
    RequestPtr init_request;
    PacketPtr init_pkt = makeWrite(
        genericAddr, genericInitial, init_request);
    externalPort.sendFunctionalPacket(init_pkt);
    fatal_if(!init_pkt->isResponse() || init_pkt->isError(),
             "Initial generic functional write failed: %s",
             init_pkt->print());
    delete init_pkt;

    externalPort.sendPacket(makeWrite(
        genericAddr + lineSize, genericInitial, genericWriteRequest));
}

void
IdealDCacheDirectedTester::startOverlapFunctionalWriteback()
{
    idealPort.sendPacket(makeWriteAs(
        genericAddr, overlapOracleLine, oracleRequestorId,
        overlapOracleSeedRequest));
}

void
IdealDCacheDirectedTester::packetAccepted(
    PortKind kind, const RequestPtr &request)
{
    if (mode == Mode::FunctionalIsolation) {
        if (kind == PortKind::Normal && request == normalWriteRequest) {
            normalWriteAccepted = true;
        } else if (kind == PortKind::External &&
                   request == canaryReadRequest) {
            canaryReadAccepted = true;
        } else if (kind == PortKind::External &&
                   request == functionalFinalReadRequest) {
            return;
        } else {
            fatal("Unexpected admitted packet in functional-isolation mode");
        }
        maybeScheduleFunctionalAction();
        return;
    }

    if (mode == Mode::GenericFunctionalReconciliation) {
        if (kind == PortKind::External && request == genericWriteRequest) {
            fatal_if(genericWriteAccepted,
                     "Generic timing write admitted more than once");
            genericWriteAccepted = true;
            return;
        }
        if (kind == PortKind::External && request == genericReadRequest) {
            fatal_if(genericReadAccepted,
                     "Generic timing read admitted more than once");
            genericReadAccepted = true;
            schedule(actionEvent, curTick() + actionDelay);
            return;
        }
        fatal("Unexpected admitted packet in generic reconciliation mode");
    }

    if (mode == Mode::OverlapFunctionalWriteback) {
        const bool expected =
            (kind == PortKind::Ideal &&
             (request == overlapOracleSeedRequest ||
              request == overlapOracleReadRequest)) ||
            (kind == PortKind::Normal &&
             (request == overlapNormalWriteRequest ||
              request == overlapNormalReadRequest)) ||
            (kind == PortKind::External &&
             request == overlapRawReadRequest);
        fatal_if(!expected,
                 "Unexpected admitted packet in overlap writeback mode");
        return;
    }

    if (kind == PortKind::External && request == carrierRequest) {
        fatal_if(carrierAccepted, "State carrier admitted more than once");
        carrierAccepted = true;
        stats.queuedStateCarriers++;
        schedule(actionEvent, curTick() + actionDelay);
        schedule(verifyEvent, curTick() + verifyDelay);
    } else if (kind == PortKind::Ideal && request == idealWriteRequest) {
        return;
    } else if (kind == PortKind::External &&
               request == carrierFinalReadRequest) {
        return;
    } else {
        fatal("Unexpected admitted packet in queued-state-carrier mode");
    }
}

void
IdealDCacheDirectedTester::maybeScheduleFunctionalAction()
{
    if (normalWriteAccepted && canaryReadAccepted && !actionEvent.scheduled()) {
        schedule(actionEvent, curTick() + actionDelay);
    }
}

void
IdealDCacheDirectedTester::performAction()
{
    if (mode == Mode::FunctionalIsolation) {
        performMaskedFunctionalWrite();
    } else if (mode == Mode::QueuedStateCarrier) {
        performIdealCarrierUpdate();
    } else {
        performGenericFunctionalWrite();
    }
}

void
IdealDCacheDirectedTester::performMaskedFunctionalWrite()
{
    fatal_if(normalWriteReceived || canaryReadReceived,
             "Timing response escaped the delay before the masked write");

    RequestPtr write_request;
    PacketPtr write_pkt = makeWrite(
        testAddr, functionalPayload, write_request, functionalMask);
    externalPort.sendFunctionalPacket(write_pkt);
    fatal_if(!write_pkt->isResponse() || write_pkt->isError(),
             "Masked functional write failed: %s", write_pkt->print());
    delete write_pkt;

    actionPerformed = true;
    stats.maskedWritesWhilePending++;
    maybeIssueFunctionalFinalRead();
}

void
IdealDCacheDirectedTester::performIdealCarrierUpdate()
{
    fatal_if(!carrierAccepted, "Carrier update ran before carrier admission");
    idealPort.sendPacket(makeWrite(
        testAddr + CarrierUpdateOffset, carrierUpdate, idealWriteRequest));
}

void
IdealDCacheDirectedTester::performGenericFunctionalWrite()
{
    fatal_if(!genericReadAccepted,
             "Generic functional write ran before read admission");
    fatal_if(genericReadReceived,
             "Generic timing response returned before the functional write");

    RequestPtr write_request;
    PacketPtr write_pkt = makeWrite(
        genericAddr, genericPayload, write_request);
    externalPort.sendFunctionalPacket(write_pkt);
    fatal_if(!write_pkt->isResponse() || write_pkt->isError(),
             "Generic functional write failed: %s", write_pkt->print());
    fatal_if(write_request->isIdealDCacheInternal() ||
             write_request->isIdealDCacheFunctionalObserved() ||
             write_request->isIdealDCacheFunctionalIsolated(),
             "Generic functional write acquired an oracle request flag");
    stats.genericOracleFlagChecks++;
    delete write_pkt;
    actionPerformed = true;
}

void
IdealDCacheDirectedTester::verifyQueuedStateCarrier()
{
    fatal_if(mode != Mode::QueuedStateCarrier,
             "Carrier verification event used in the wrong mode");
    fatal_if(!carrierAccepted || !idealWriteReceived,
             "Carrier verification ran before the ideal update completed");
    externalPort.sendPacket(makeRead(
        testAddr, lineSize, carrierFinalReadRequest));
}

void
IdealDCacheDirectedTester::verifyOverlapFunctionalWriteback()
{
    fatal_if(mode != Mode::OverlapFunctionalWriteback,
             "Overlap writeback verification used in the wrong mode");
    fatal_if(!overlapNormalReadReceived || !overlapVisitorSeen,
             "Overlap verification ran before the functional writeback");

    externalPort.sendPacket(makeRead(
        genericAddr, lineSize, overlapRawReadRequest));
    idealPort.sendPacket(makeReadAs(
        genericAddr, lineSize, oracleRequestorId,
        overlapOracleReadRequest));
}

void
IdealDCacheDirectedTester::forwardVisitorFunctional(PacketPtr pkt)
{
    fatal_if(mode != Mode::OverlapFunctionalWriteback,
             "Visitor probe received functional traffic in the wrong mode");
    fatal_if(overlapVisitorSeen,
             "Overlap functional writeback visitor observed more than once");
    fatal_if(!pkt->isRequest() || !pkt->isWrite() || pkt->isRead() ||
             pkt->getAddr() != genericAddr || pkt->getSize() != lineSize,
             "Unexpected overlap functional visitor packet: %s",
             pkt->print());
    fatal_if(pkt->requestorId() != requestorId,
             "Functional visitor lost device requestor provenance");
    fatal_if(!packetDataEquals(pkt, overlapExpectedLine),
             "Functional visitor did not carry the dirty device block");
    stats.overlapVisitorDataChecks++;

    visitorProbeMemSidePort.sendFunctional(pkt);

    fatal_if(pkt->req->isIdealDCacheInternal() ||
             pkt->req->isIdealDCacheFunctionalObserved() ||
             pkt->req->isIdealDCacheFunctionalIsolated(),
             "Functional visitor acquired an oracle request flag");
    fatal_if(pkt->requestorId() != requestorId,
             "Functional visitor requestor changed downstream");
    stats.overlapVisitorFlagChecks++;
    overlapVisitorSeen = true;
}

void
IdealDCacheDirectedTester::testTimeout()
{
    const char *mode_name = "generic-functional-reconciliation";
    if (mode == Mode::FunctionalIsolation) {
        mode_name = "functional-isolation";
    } else if (mode == Mode::QueuedStateCarrier) {
        mode_name = "queued-state-carrier";
    } else if (mode == Mode::OverlapFunctionalWriteback) {
        mode_name = "overlap-functional-writeback";
    }
    fatal("Ideal-DCache directed tester timed out in %s mode",
          mode_name);
}

void
IdealDCacheDirectedTester::recvTimingResp(PortKind kind, PacketPtr pkt)
{
    const RequestPtr &request = pkt->req;
    fatal_if(pkt->isError(), "Directed tester received an error: %s",
             pkt->print());

    if (mode == Mode::FunctionalIsolation) {
        if (kind == PortKind::Normal && request == normalWriteRequest) {
            normalWriteReceived = true;
            delete pkt;
            maybeIssueFunctionalFinalRead();
            return;
        }
        if (kind == PortKind::External && request == canaryReadRequest) {
            fatal_if(!packetDataEquals(pkt, functionalInitial),
                     "Queued timing response was modified by the later "
                     "functional write");
            canaryReadReceived = true;
            stats.preservedQueuedResponses++;
            delete pkt;
            maybeIssueFunctionalFinalRead();
            return;
        }
        if (kind == PortKind::External &&
            request == functionalFinalReadRequest) {
            fatal_if(!packetDataEquals(pkt, functionalExpected),
                     "Masked functional write did not merge byte enables");
            stats.maskedMergeChecks++;
            stats.sameLineMshrTargetChecks++;
            delete pkt;
            pass();
            return;
        }
    } else if (mode == Mode::QueuedStateCarrier) {
        if (kind == PortKind::Ideal && request == idealWriteRequest) {
            idealWriteReceived = true;
            delete pkt;
            return;
        }
        if (kind == PortKind::External &&
            request == carrierFinalReadRequest) {
            fatal_if(!packetDataEquals(pkt, carrierExpected),
                     "Delayed state carrier overwrote the newer ideal value");
            stats.stateCarrierDataChecks++;
            delete pkt;
            pass();
            return;
        }
    } else if (mode == Mode::OverlapFunctionalWriteback) {
        if (kind == PortKind::Ideal &&
            request == overlapOracleSeedRequest) {
            fatal_if(overlapOracleSeedReceived,
                     "Overlap oracle seed completed more than once");
            overlapOracleSeedReceived = true;
            delete pkt;

            RequestPtr raw_init_request;
            PacketPtr raw_init_pkt = makeWrite(
                genericAddr, overlapDeviceLine, raw_init_request);
            externalPort.sendFunctionalPacket(raw_init_pkt);
            fatal_if(!raw_init_pkt->isResponse() || raw_init_pkt->isError(),
                     "Overlap device backing initialization failed: %s",
                     raw_init_pkt->print());
            fatal_if(raw_init_request->isIdealDCacheInternal() ||
                     raw_init_request->isIdealDCacheFunctionalObserved() ||
                     raw_init_request->isIdealDCacheFunctionalIsolated(),
                     "Overlap device initialization acquired oracle flags");
            delete raw_init_pkt;

            normalPort.sendPacket(makeWrite(
                genericAddr, overlapDeviceUpdate,
                overlapNormalWriteRequest));
            return;
        }
        if (kind == PortKind::Normal &&
            request == overlapNormalWriteRequest) {
            fatal_if(overlapNormalWriteReceived,
                     "Overlap normal write completed more than once");
            fatal_if(request->isIdealDCacheInternal() ||
                     request->isIdealDCacheFunctionalObserved() ||
                     request->isIdealDCacheFunctionalIsolated(),
                     "Overlap normal write acquired oracle flags");
            overlapNormalWriteReceived = true;
            delete pkt;
            normalPort.sendPacket(makeRead(
                genericAddr, lineSize, overlapNormalReadRequest));
            return;
        }
        if (kind == PortKind::Normal &&
            request == overlapNormalReadRequest) {
            fatal_if(!overlapNormalWriteReceived ||
                     overlapNormalReadReceived,
                     "Unexpected overlap normal read completion");
            fatal_if(!packetDataEquals(pkt, overlapExpectedLine),
                     "Normal L1D block lost device-provenance data");
            fatal_if(request->isIdealDCacheInternal() ||
                     request->isIdealDCacheFunctionalObserved() ||
                     request->isIdealDCacheFunctionalIsolated(),
                     "Overlap normal read acquired oracle flags");
            overlapNormalReadReceived = true;
            stats.overlapNormalBlockChecks++;
            delete pkt;
            schedule(postWritebackEvent, curTick() + actionDelay);
            exitSimLoop("ideal dcache overlap functional writeback ready");
            return;
        }
        if (kind == PortKind::External &&
            request == overlapRawReadRequest) {
            fatal_if(!packetDataEquals(pkt, overlapExpectedLine),
                     "Functional visitor did not update raw RAM data");
            fatal_if(request->isIdealDCacheInternal() ||
                     request->isIdealDCacheFunctionalObserved() ||
                     request->isIdealDCacheFunctionalIsolated(),
                     "Overlap raw read acquired oracle flags");
            overlapRawReadReceived = true;
            stats.overlapRawMemoryChecks++;
            delete pkt;
            maybePassOverlapFunctionalWriteback();
            return;
        }
        if (kind == PortKind::Ideal &&
            request == overlapOracleReadRequest) {
            fatal_if(!packetDataEquals(pkt, overlapOracleLine),
                     "Device functional visitor modified canonical RAM data");
            overlapOracleReadReceived = true;
            stats.overlapOracleChecks++;
            delete pkt;
            maybePassOverlapFunctionalWriteback();
            return;
        }
    } else if (kind == PortKind::External &&
               request == genericWriteRequest) {
        fatal_if(!genericWriteAccepted || genericWriteReceived,
                 "Unexpected generic timing write response");
        fatal_if(request->isIdealDCacheInternal() ||
                 request->isIdealDCacheFunctionalObserved() ||
                 request->isIdealDCacheFunctionalIsolated(),
                 "Generic timing write acquired an oracle request flag");
        genericWriteReceived = true;
        stats.genericTimingWriteResponses++;
        delete pkt;
        externalPort.sendPacket(makeRead(
            genericAddr, AccessSize, genericReadRequest));
        return;
    } else if (kind == PortKind::External &&
               request == genericReadRequest) {
        fatal_if(!actionPerformed,
                 "Generic timing response returned before reconciliation");
        fatal_if(!packetDataEquals(pkt, genericPayload),
                 "Generic queued response did not observe the later "
                 "functional write");
        genericReadReceived = true;
        stats.genericQueuedResponseUpdates++;
        delete pkt;
        pass();
        return;
    }

    fatal("Unexpected timing response: %s", pkt->print());
}

void
IdealDCacheDirectedTester::maybeIssueFunctionalFinalRead()
{
    if (actionPerformed && normalWriteReceived && canaryReadReceived &&
        !functionalFinalReadRequest) {
        externalPort.sendPacket(makeRead(
            testAddr, AccessSize, functionalFinalReadRequest));
    }
}

void
IdealDCacheDirectedTester::maybePassOverlapFunctionalWriteback()
{
    if (overlapRawReadReceived && overlapOracleReadReceived) {
        pass();
    }
}

void
IdealDCacheDirectedTester::pass()
{
    if (mode == Mode::FunctionalIsolation) {
        inform("IDEAL_DCACHE_FUNCTIONAL_ISOLATION: PASS");
        exitSimLoop("ideal dcache functional isolation passed");
    } else if (mode == Mode::QueuedStateCarrier) {
        inform("IDEAL_DCACHE_QUEUED_STATE_CARRIER: PASS");
        exitSimLoop("ideal dcache queued state carrier passed");
    } else if (mode == Mode::OverlapFunctionalWriteback) {
        inform("IDEAL_DCACHE_OVERLAP_FUNCTIONAL_WRITEBACK: PASS");
        exitSimLoop("ideal dcache overlap functional writeback passed");
    } else {
        inform("IDEAL_DCACHE_GENERIC_FUNCTIONAL_RECONCILIATION: PASS");
        exitSimLoop("ideal dcache generic functional reconciliation passed");
    }
}

} // namespace gem5
