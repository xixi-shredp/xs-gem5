#include "cpu/pred/btb/hint_bundle.hh"
#include <algorithm>
#include "base/logging.hh"
#include <limits>
#include <set>
#include <string>
#include <utility>
namespace gem5::branch_prediction::btb_pred {
HintBundleController::HintBundleController(Hooks h):hooks(std::move(h)) {}
uint64_t HintBundleController::word(const std::vector<uint8_t>& b,unsigned at,unsigned n) {
    uint64_t v=0; for(unsigned i=0;i<n;++i)v|=uint64_t(b.at(at+i))<<(8*i); return v;
}
void HintBundleController::field(Record& r,unsigned lo,unsigned n,uint64_t v) {
    for(unsigned i=0;i<n;++i) if((v>>i)&1)r[(lo+i)/8]|=1U<<((lo+i)%8);
}
bool HintBundleController::busy() const {
    return state!=State::Idle || leasedSlots() || !boundaries.empty() || stopping ||
        !boundaryTrace.empty() || !ordinaryTrace.empty() || fold || writing ||
        !traceBuffersEmpty();
}
uint64_t HintBundleController::readCsr(unsigned csr) const {
    if(csr==0x5c1)return paddr;
    if(csr==0x5c2)return laddr;
    if(csr!=0x5c0)return 0;
    return (status & ~((7ULL<<56)|(7ULL<<52)|Busy|(7ULL<<12))) |
        (uint64_t(perror)<<56)|(uint64_t(lerror)<<52)|(busy()?Busy:0);
}
void HintBundleController::setAdmission(bool value) {
    if(admitted==value)return;
    admitted=value; if(hooks.admission)hooks.admission(value);
}
void HintBundleController::writeCsr(unsigned csr,uint64_t value) {
    if(csr==0x5c1 || csr==0x5c2) {
        if(status&(Started|Enabled) || busy())return;
        if(value>>44){status&=~Valid;return;}
        if(csr==0x5c1)paddr=value; else {laddr=value;prepared=false;}
        return;
    }
    if(csr!=0x5c0)return;
    const uint64_t writable=Valid|Enabled|Started|Ignore|Bundle|(4095ULL<<15)|4095;
    const uint64_t ro=(7ULL<<56)|(7ULL<<52)|Busy|(7ULL<<12);
    if(status&Started)return;
    if(busy()) {
        if((status&Enabled) && !(value&Enabled) &&
           ((value^status)&(writable&~Enabled))==0) {
            status&=~Enabled;setAdmission(false);prepared=false;
            if(state==State::Image || state==State::Install){state=State::Abort;partial=true;}
            else if(state==State::Idle)state=State::Drain;
        }
        return;
    }
    if(!(value&Valid) || (value&~(writable|ro)) ||
       __builtin_popcountll(value&(Enabled|Started))>1) {status&=~Valid;return;}
    if((status&Enabled) && ((status^value)&(writable&~Enabled)))return;
    const bool changed=((status^value)&Bundle)!=0;
    if(changed)prepared=false;
    status=value&writable;
    if(!(status&Bundle)) {setAdmission(false);return;}
    if(status&Load) {lerror=0;beginLoad();return;}
    if(status&(Enabled|Discovery|Detail)) {
        unsigned expected=(status&Discovery)?0:(status&Detail)?1:2;
        if(!prepared || partial || lerror || kind!=expected ||
           ((status&(Discovery|Detail)) && (!(status&4095)||!capacity()))) {
            status&=~(Valid|Enabled|Discovery|Detail);return;
        }
        if(status&(Discovery|Detail)) {
            const uint64_t bytes=((status>>15)&4095)*(1ULL<<20);
            const Addr out=paddr<<20,base=laddr<<20;
            if(out>std::numeric_limits<Addr>::max()-bytes ||
               base>std::numeric_limits<Addr>::max()-0x108000 ||
               (out<base+0x108000 && base<out+bytes)) {
                status&=~(Valid|Discovery|Detail);return;
            }
            perror=0;reserved=written=0;budget=(status&4095)*1000000ULL;
            stopping=false;finalQueued=false;
            packedBytes=acknowledgedBytes=0;traceFault=false;failedWriteAt.reset();
            if(hooks.profileStage)hooks.profileStage(kind,true);
        }
        if((status&Enabled) && hooks.profileStage)hooks.profileStage(kind,true);
        setAdmission(active.has_value() && kind!=0);
    } else {
        setAdmission(false);
        state=State::Drain;
    }
}
void HintBundleController::fail(unsigned le,unsigned pe) {
    lerror=std::max(lerror,le);if(profiling())perror=std::max(perror,pe?pe:3U);
    status&=~Enabled;setAdmission(false);prepared=false;partial=true;
    discardBoundaryControls();desired.reset();state=State::ErrorDrain;
    if(profiling())stopping=true;
    if(profiling() && (le==4 || pe==4))traceFault=true;
}
void HintBundleController::beginLoad() {
    if ((laddr<<20)>std::numeric_limits<Addr>::max()-0x108000) {
        fail(1);return;
    }
    setAdmission(false);prepared=false;initialLoad=true;active.reset();desired.reset();
    discardBoundaryControls();state=State::Drain;tableReady=false;
}
void HintBundleController::beginSwitch() {
    setAdmission(false);state=State::Drain;
}
bool HintBundleController::validateRoutes() {
    if(image.size()!=16 || std::string(image.begin(),image.begin()+8)!="H2PBND01")return false;
    kind=image[8];initialValid=image[9]&32;initialId=image[9]&31;
    if(kind>2 || image[9]&192 || (!initialValid&&initialId) || image[10]||image[11])return false;
    validMask=word(image,12,4);
    if(initialValid && !(validMask&(1U<<initialId)))return false;
    return true;
}
bool HintBundleController::consumeRouteBeat() {
    const int index=currentRead();if(index<0)return true;
    auto& slot=dmaSlots[index];auto& read=std::get<ReadMeta>(slot.meta);
    const auto& payload=hooks.dmaPayload(slot.token);
    const unsigned beatConsumed=read.consumed;
    const uint64_t at=slot.at+beatConsumed;
    const unsigned bytes=at<272?8:4;
    if(beatConsumed+bytes>slot.bytes)return false;
    bufferPeaks.maxRowBytesPerTick=std::max(bufferPeaks.maxRowBytesPerTick,bytes);
    const uint64_t value=payloadWord(payload,beatConsumed,bytes);
    if(at<16) {
        image.insert(image.end(),payload.begin()+beatConsumed,
                     payload.begin()+beatConsumed+bytes);
        if(at==8 && !validateRoutes())return false;
    } else if(at<272) {
        const unsigned row=(at-16)/8;
        if(value>>48 || (!(value&1)&&value))return false;
        for(unsigned i=0;i<row;++i)
            if((value&1)&&(boundaryTable[i]&1)&&(boundaryTable[i]>>1)==(value>>1))return false;
        boundaryTable[row]=value;
    } else {
        const unsigned row=(at-272)/4;
        if(value>>22 || (!(value&1)&&value) ||
           ((value&1)&&!(validMask&(1U<<(value>>17)))))return false;
        if((row&1)&&(value&1)&&(bundleTable[row-1]&1)&&
           ((value>>1)&65535)==((bundleTable[row-1]>>1)&65535)&&
           (value>>17)!=(bundleTable[row-1]>>17))return false;
        bundleTable[row]=value;
    }
    finishReadRow(index,bytes);
    return true;
}
bool HintBundleController::consumeImageBeat() {
    const int index=currentRead();if(index<0)return true;
    auto& slot=dmaSlots[index];auto& read=std::get<ReadMeta>(slot.meta);
    const auto& payload=hooks.dmaPayload(slot.token);
    const unsigned beatConsumed=read.consumed;
    const uint64_t at=slot.at+beatConsumed;
    const unsigned bytes=at>=0x90 && at<0x4090?4:8;
    if(beatConsumed+bytes>slot.bytes)return false;
    bufferPeaks.maxRowBytesPerTick=std::max(bufferPeaks.maxRowBytesPerTick,bytes);
    if(at<16) {
        image.insert(image.end(),payload.begin()+beatConsumed,
                     payload.begin()+beatConsumed+bytes);
        if(at==8) {
            const std::string magic(image.begin(),image.begin()+8);
            if(magic!=(kind==1?"H2PCAM58":"H2PLD058") ||
               !std::all_of(image.begin()+8,image.end(),[](uint8_t v){return v==0;}) ||
               !hooks.beginInstall || !hooks.installRow || !hooks.finishInstall ||
               !hooks.beginInstall(kind))return false;
            innerStarted=true;
        }
    } else {
        if(!innerStarted || !hooks.installRow(installRow,payloadWord(payload,beatConsumed,bytes)))return false;
        ++installRow;
    }
    finishReadRow(index,bytes);
    return true;
}
void HintBundleController::activateImage() {
    if(!hooks.finishInstall || !hooks.finishInstall(kind)){fail(1);return;}
    if(hooks.audit)hooks.audit("activate",latestBoundary.sequence,latestBoundary.pc,
        latestBoundary.target,active?int(*active):-1,loading?int(*loading):-1);
    active=loading;partial=false;prepared=true;++switches;state=State::Idle;
    if(initialLoad){status&=~Load;initialLoad=false;}
    setAdmission((status&(Enabled|Detail)) && active.has_value());
}

uint64_t HintBundleController::payloadWord(const DmaPayload& data,unsigned at,unsigned bytes) {
    uint64_t value=0;for(unsigned i=0;i<bytes;++i)value|=uint64_t(data.at(at+i))<<(8*i);
    return value;
}
unsigned HintBundleController::leasedSlots() const {
    unsigned count=0;for(const auto& slot:dmaSlots)count+=slot.state!=SlotState::Free;
    return count;
}
size_t HintBundleController::bufferedLoadBytes() const {
    size_t bytes=image.size();
    for(const auto& slot:dmaSlots)if(slot.state!=SlotState::Free&&!slot.write())bytes+=slot.bytes;
    return bytes;
}
size_t HintBundleController::bufferedTraceBytes() const {
    size_t bytes=0;
    for(const auto& slot:dmaSlots)if(slot.state!=SlotState::Free&&slot.write())bytes+=slot.bytes;
    return bytes;
}
void HintBundleController::observeBuffers() {
    unsigned reads=0,writes=0,readBytes=0,writeBytes=0,ends=0;
    for(const auto& slot:dmaSlots)if(slot.state!=SlotState::Free) {
        if(slot.write()){++writes;writeBytes+=slot.bytes;ends+=std::get<WriteMeta>(slot.meta).endCount;}
        else {++reads;readBytes+=slot.bytes;}
    }
    bufferPeaks.sharedSlotPeak=std::max(bufferPeaks.sharedSlotPeak,reads+writes);
    bufferPeaks.readSlotPeak=std::max(bufferPeaks.readSlotPeak,reads);
    bufferPeaks.writeSlotPeak=std::max(bufferPeaks.writeSlotPeak,writes);
    bufferPeaks.sharedPayloadPeakBytes=std::max(bufferPeaks.sharedPayloadPeakBytes,64*(reads+writes));
    bufferPeaks.readBufferPeakBytes=std::max(bufferPeaks.readBufferPeakBytes,readBytes);
    bufferPeaks.writeBufferPeakBytes=std::max(bufferPeaks.writeBufferPeakBytes,writeBytes);
    bufferPeaks.completionDescriptorPeak=std::max(bufferPeaks.completionDescriptorPeak,ends);
}
int HintBundleController::acquireSlot(bool write) {
    if(!hooks.dmaAcquire||!hooks.dmaPayload||!hooks.dmaRelease)return -1;
    const auto token=hooks.dmaAcquire();if(!token)return -1;
    fatal_if(token->slot>=dmaSlots.size() || dmaSlots[token->slot].state!=SlotState::Free,
             "Hint DMA acquired duplicate/invalid shared slot");
    auto& slot=dmaSlots[token->slot];slot={};slot.token=*token;
    slot.state=write?SlotState::Filling:SlotState::Reserved;
    if(write)slot.meta=WriteMeta{};else slot.meta=ReadMeta{loadEpoch,0};
    observeBuffers();return token->slot;
}
void HintBundleController::releaseSlot(unsigned index) {
    auto& slot=dmaSlots.at(index);
    fatal_if(slot.state==SlotState::Free||slot.state==SlotState::Submitted,
             "Hint DMA released unresolved/free slot");
    const auto token=slot.token;slot={};hooks.dmaRelease(token);
}
bool HintBundleController::readsEmpty() const {
    for(const auto& slot:dmaSlots)if(slot.state!=SlotState::Free&&!slot.write())return false;
    return true;
}
int HintBundleController::currentRead() const {
    for(unsigned i=0;i<dmaSlots.size();++i) {
        const auto& slot=dmaSlots[i];
        if(slot.state!=SlotState::Responded||slot.write()||!slot.success)continue;
        const auto& read=std::get<ReadMeta>(slot.meta);
        if(read.epoch==loadEpoch&&slot.at+read.consumed==offset)return i;
    }
    return -1;
}
void HintBundleController::finishReadRow(unsigned index,unsigned bytes) {
    auto& slot=dmaSlots[index];auto& read=std::get<ReadMeta>(slot.meta);
    read.consumed+=bytes;offset+=bytes;
    if(read.consumed==slot.bytes)releaseSlot(index);
}
void HintBundleController::discardRead() {
    // One completed or unsubmitted canceled lease per tick. Accepted requests
    // retain their payload and epoch until the transport actually returns.
    for(unsigned i=0;i<dmaSlots.size();++i) {
        const auto& slot=dmaSlots[i];
        if(slot.state!=SlotState::Free&&!slot.write()&&slot.state!=SlotState::Submitted) {
            releaseSlot(i);return;
        }
    }
}
void HintBundleController::receiveDma(DmaToken token,bool ok,uint64_t responseCycle) {
    fatal_if(token.slot>=dmaSlots.size(),"Invalid Hint DMA completion slot");
    auto& slot=dmaSlots[token.slot];
    fatal_if(slot.state!=SlotState::Submitted||slot.token.generation!=token.generation,
             "Stale or duplicate Hint DMA completion");
    slot.state=SlotState::Responded;slot.success=ok;slot.responseCycle=responseCycle;
    if(!ok) {
        if(slot.write())failedWriteAt=failedWriteAt?std::min(*failedWriteAt,slot.at):slot.at;
        fail(4,4);
    } else if(!slot.write()&&state==State::Image)state=State::Install;
    observeBuffers();
}

std::optional<unsigned> HintBundleController::route(Addr pc,Addr target) const {
    bool match=false;
    for(auto row:boundaryTable)if((row&1)&&(row>>1)==((pc>>1)&((1ULL<<47)-1)))match=true;
    if(!match)return desired;
    // F24((P[47:1]<<47)|T[47:1]), bitwise without host-width truncation.
    uint32_t hash=0;
    for(unsigned bit=0;bit<47;++bit) {
        if((target>>(bit+1))&1)hash^=1U<<(bit%24);
        if((pc>>(bit+1))&1)hash^=1U<<((bit+47)%24);
    }
    for(unsigned way=0;way<2;++way) {
        const auto row=bundleTable[(hash&255)*2+way];
        if((row&1)&&((row>>1)&65535)==(hash>>8))return row>>17;
    }
    return std::nullopt;
}
void HintBundleController::releaseBoundaryOwner(int owner,bool routeDone) {
    if(owner<0)return;
    auto &slot=boundaryOwners.at(owner);
    if(routeDone)slot.routeDone=true;else slot.traceDone=true;
    if(slot.routeDone&&slot.traceDone)slot={};
}
void HintBundleController::discardBoundaryControls() {
    for(const auto& event:boundaries)releaseBoundaryOwner(event.owner,true);
    boundaries.clear();
}
void HintBundleController::committedBoundary(Addr pc,Addr target,uint64_t seq,unsigned size) {
    if(!bundleEnabled() || !(status&(Enabled|Discovery|Detail)) || stopping || lerror)return;
    bool hit=false;
    for(auto row:boundaryTable)if((row&1)&&(row>>1)==((pc>>1)&((1ULL<<47)-1)))hit=true;
    if(!hit)return;
    int owner=-1;
    for(unsigned i=0;i<boundaryOwners.size();++i)
        if(!boundaryOwners[i].used){owner=i;break;}
    if(owner<0){++boundaryLost;fail(2,3);return;}
    const bool trace=profiling();
    boundaryOwners[owner]={true,false,!trace};
    bufferPeaks.boundaryOwnerPeak=std::max(bufferPeaks.boundaryOwnerPeak,occupiedBoundarySlots());
    Record record{};field(record,0,1,1);field(record,1,47,pc>>1);field(record,48,47,target>>1);
    TraceAudit audit; audit.sequence=seq;audit.sourcePC=pc;audit.target=target;
    audit.boundary=true;
    if(trace) {
        if(!acceptRecord(record,true,audit)){boundaryOwners[owner]={};return;}
        boundaryTrace.back().owner=owner;
    }
    logicalOwner=route(pc,target);
    if(stopping){releaseBoundaryOwner(owner,true);return;}
    boundaries.push_back({pc,target,seq,size,owner});
    // Stop old-table admission at the architectural boundary, before the
    // finite table pipeline computes its route result. Never redirect Core.
    setAdmission(false);
}

void HintBundleController::processBoundary() {
    if(boundaries.empty())return;
    const auto event=boundaries.front();boundaries.pop_front();
    releaseBoundaryOwner(event.owner,true);
    auto next=route(event.pc,event.target);if(!next)++routeMisses;
    if(hooks.audit)hooks.audit("boundary",event.sequence,event.pc,event.target,
                               desired?int(*desired):-1,next?int(*next):-1);
    latestBoundary=event;
    desired=next;
    if(kind==0)return;
    if((state==State::Image || state==State::Install) && loading!=desired){
        if(hooks.audit)hooks.audit("abort",event.sequence,event.pc,event.target,
            loading?int(*loading):-1,desired?int(*desired):-1);
        ++aborted;partial=true;state=State::Abort;
    }
    else if(state==State::Idle) {
        if(active==desired && !partial)setAdmission(active.has_value()&&(status&(Enabled|Detail)));
        else beginSwitch();
    }
}
bool HintBundleController::acceptRecord(const Record& data,bool boundary,TraceAudit audit) {
    if(!profiling() || stopping)return false;
    if(reserved>=capacity()){stopProfile(2);return false;}
    auto &queue=boundary?boundaryTrace:ordinaryTrace;
    const size_t limit=boundary?BoundaryCapacity:16;
    if(queue.size()>=limit) {
        if(boundary){++boundaryLost;fail(2,3);return false;}
        ++ordinaryLost;perror=std::max(perror,1U);
        if(status&Ignore)return false;
        Trace *oldest=&queue.front();
        if(fold && !fold->boundary && fold->slot<oldest->slot)oldest=&*fold;
        if(writing && !writing->boundary && writeOffset==0 &&
           writing->slot<oldest->slot)oldest=&*writing;
        oldest->data=data;
        audit.event="overwrite";audit.cycle=now;audit.recordIndex=oldest->slot;
        audit.reservationIndex=oldest->slot;
        audit.logicalMetadata=logicalOwner?int(*logicalOwner):-1;
        oldest->audit=audit;
        if(hooks.traceAudit)hooks.traceAudit(audit);
        return true; // overwrite oldest unissued ordinary slot
    }
    audit.event="accept";audit.cycle=now;audit.recordIndex=reserved;
    audit.reservationIndex=reserved;
    audit.logicalMetadata=logicalOwner?int(*logicalOwner):-1;
    if(hooks.traceAudit)hooks.traceAudit(audit);
    queue.push_back({data,reserved++,boundary,audit});
    if(reserved==capacity())stopProfile(2);
    return true;
}
bool HintBundleController::recordDiscovery(Addr window,unsigned pos,bool call,uint64_t seq,Addr source_pc) {
    if(kind!=0 || !(status&Discovery))return false;
    Record r{};field(r,1,47,window>>1);field(r,48,5,pos);field(r,53,1,call);
    TraceAudit audit;audit.sequence=seq;audit.sourcePC=source_pc;audit.pcId=pos;
    return acceptRecord(r,false,audit);
}
bool HintBundleController::recordDetail(unsigned id,Addr target,
    const std::array<uint32_t,4>& c,bool miss,bool valid,int snapshot_metadata,uint64_t seq,Addr source_pc) {
    if(kind!=1 || !(status&Detail))return false;
    Record r{};field(r,1,4,id);field(r,5,47,target>>1);
    if(valid){field(r,52,28,c[0]);field(r,80,28,c[3]);field(r,108,28,c[1]);field(r,136,28,c[2]);}
    field(r,164,1,miss);field(r,165,1,valid);
    TraceAudit audit;audit.sequence=seq;audit.sourcePC=source_pc;audit.target=target;
    audit.pcId=id;audit.queryValid=valid;audit.snapshotMetadata=snapshot_metadata;
    return acceptRecord(r,false,audit);
}
void HintBundleController::stopProfile(unsigned error) {
    perror=std::max(perror,error);stopping=true;setAdmission(false);
    discardBoundaryControls();
    if(state==State::Image || state==State::Install){partial=true;state=State::Abort;}
    else if(state==State::Idle)state=State::Drain;
}
void HintBundleController::retiredInstruction() {
    if(!profiling()||stopping)return;
    if(budget && --budget==0)stopProfile(0);
}
void HintBundleController::serviceTrace() {
    if(traceFault) {
        fold.reset();writing.reset();writeOffset=0;
        if(!boundaryTrace.empty()) {
            releaseBoundaryOwner(boundaryTrace.front().owner,false);boundaryTrace.pop_front();
        } else if(!ordinaryTrace.empty())ordinaryTrace.pop_front();
        // Keep every successful prefix before a failed write until ordered
        // retirement. Discard only returned later work or never-submitted work.
        for(unsigned i=0;i<dmaSlots.size();++i) {
            const auto& slot=dmaSlots[i];
            if(slot.state==SlotState::Free||!slot.write()||slot.state==SlotState::Submitted)continue;
            if(slot.state!=SlotState::Responded||(failedWriteAt&&slot.at>=*failedWriteAt)) {
                releaseSlot(i);break;
            }
        }
        return;
    }
    if(!writing&&fold){writing=std::move(fold);fold.reset();writeOffset=0;}
    if(!fold) {
        if(!boundaryTrace.empty()) {
            fold=boundaryTrace.front();boundaryTrace.pop_front();
            releaseBoundaryOwner(fold->owner,false);fold->owner=-1;
        } else if(!ordinaryTrace.empty()){fold=ordinaryTrace.front();ordinaryTrace.pop_front();}
    }
    int filling=-1;
    for(unsigned i=0;i<dmaSlots.size();++i)
        if(dmaSlots[i].state==SlotState::Filling)filling=i;
    if(writing&&filling<0) {
        filling=acquireSlot(true);
        if(filling>=0)dmaSlots[filling].at=packedBytes;
    }
    if(writing&&filling>=0) {
        auto& slot=dmaSlots[filling];auto& write=std::get<WriteMeta>(slot.meta);
        auto& payload=hooks.dmaPayload(slot.token);
        const unsigned limit=64-(((paddr<<20)+slot.at)&63);
        const unsigned bytes=std::min({8U,22-writeOffset,limit-slot.bytes});
        std::copy_n(writing->data.begin()+writeOffset,bytes,payload.begin()+slot.bytes);
        slot.bytes+=bytes;writeOffset+=bytes;packedBytes+=bytes;
        bufferPeaks.maxPackBytesPerTick=std::max(bufferPeaks.maxPackBytesPerTick,bytes);
        if(writeOffset==22) {
            fatal_if(write.endCount>=write.ends.size(),"Hint DMA record-end overflow");
            write.ends[write.endCount++]={writing->audit,writing->slot,writing->boundary,slot.bytes};
            writing.reset();writeOffset=0;
        }
        if(slot.bytes==limit)slot.state=SlotState::Ready;
    }
    if(!writing&&!fold&&boundaryTrace.empty()&&ordinaryTrace.empty())
        for(auto& slot:dmaSlots)if(slot.state==SlotState::Filling&&slot.bytes)slot.state=SlotState::Ready;
    observeBuffers();
}
bool HintBundleController::traceBuffersEmpty() const {
    for(const auto& slot:dmaSlots)if(slot.state!=SlotState::Free&&slot.write())return false;
    return true;
}
void HintBundleController::retireTrace() {
    for(unsigned index=0;index<dmaSlots.size();++index) {
        auto& slot=dmaSlots[index];
        if(slot.state!=SlotState::Responded||!slot.write()||!slot.success||
           slot.at!=acknowledgedBytes||(failedWriteAt&&slot.at>=*failedWriteAt))continue;
        const auto& write=std::get<WriteMeta>(slot.meta);
        unsigned completed=0;
        for(unsigned i=0;i<write.endCount;++i) {
            const auto& end=write.ends[i];
            fatal_if(!end.byteEnd||end.byteEnd>slot.bytes||slot.at+end.byteEnd!=(written+1)*22,
                     "Hint DMA complete-record frontier mismatch");
            if(hooks.traceAudit) {
                TraceAudit audit=end.audit;audit.event="write";audit.cycle=now;
                audit.responseCycle=slot.responseCycle;audit.retireCycle=now;
                audit.recordIndex=written;audit.reservationIndex=end.reservation;
                audit.boundary=end.boundary;hooks.traceAudit(audit);
            }
            ++written;++completed;
        }
        acknowledgedBytes+=slot.bytes;releaseSlot(index);
        bufferPeaks.maxWriteRetireSlotsPerTick=1;
        bufferPeaks.maxWriteRetireRecordsPerTick=std::max(bufferPeaks.maxWriteRetireRecordsPerTick,completed);
        return;
    }
}
void HintBundleController::submitDma() {
    if(!hooks.dmaSubmit)return;
    int read=-1,write=-1;
    const bool canRead=(state==State::Route||state==State::Image||state==State::Install)&&
        nextIssueOffset<total&&(nextIssueOffset==0||image.size()==16)&&
        (state==State::Route||innerStarted||nextIssueOffset==0);
    if(canRead) {
        for(unsigned i=0;i<dmaSlots.size();++i)
            if(dmaSlots[i].state==SlotState::Reserved&&!dmaSlots[i].write())read=i;
        if(read<0) {
            read=acquireSlot(false);
            if(read>=0) {
                auto& slot=dmaSlots[read];slot.at=nextIssueOffset;
                const Addr address=(laddr<<20)+(state==State::Route?0:0x8000+*loading*0x8000)+slot.at;
                slot.bytes=std::min<uint64_t>({64,64-(address&63),total-slot.at});
            }
        }
    }
    if(!traceFault)
        for(unsigned i=0;i<dmaSlots.size();++i)
            if(dmaSlots[i].state==SlotState::Ready&&dmaSlots[i].write()&&
               (write<0||dmaSlots[i].at<dmaSlots[write].at))write=i;
    const int index=(read>=0&&(write<0||preferRead))?read:write;
    if(index<0){observeBuffers();return;}
    auto& slot=dmaSlots[index];const bool isWrite=slot.write();
    const Addr address=(isWrite?paddr<<20:(laddr<<20)+(state==State::Route?0:0x8000+*loading*0x8000))+slot.at;
    const SlotState previous=slot.state;slot.state=SlotState::Submitted;
    // Latch the frontier before Submit: Done(false) may run synchronously.
    const uint64_t next=slot.at+slot.bytes;
    if(!hooks.dmaSubmit(slot.token,address,isWrite,slot.bytes,
        [this](DmaToken token,bool ok,uint64_t cycle){receiveDma(token,ok,cycle);})) {
        fatal_if(slot.state!=SlotState::Submitted,"Rejected Hint DMA illegally called Done");
        slot.state=previous;
    } else {
        if(!isWrite)nextIssueOffset=next;
        preferRead=isWrite;bufferPeaks.maxSubmitPerTick=1;
    }
    observeBuffers();
}
void HintBundleController::tick(uint64_t cycle) {
    now=cycle;
    if(!bundleEnabled())return;
    if(!admitted && (status&(Enabled|Detail)))++unavailable;
    if(!boundaries.empty() && !stopping && !lerror)processBoundary();
    const bool drained=(!hooks.quiescent || hooks.quiescent());
    retireTrace();
    if(state==State::Abort||state==State::ErrorDrain)discardRead();
    if(state==State::Abort && readsEmpty()) {
        // Accepted reads have now completed; local recovery still owns its
        // heads until the full quiescence predicate is satisfied below.
        image.clear();innerStarted=false;
        state=State::Drain;
    }
    if(state==State::ErrorDrain && readsEmpty()) {
        if(drained){active.reset();status&=~Load;initialLoad=false;state=State::Idle;}
    }
    if(state==State::Drain && drained && readsEmpty()) {
        if(initialLoad && !tableReady) {
            image.clear();boundaryTable={};bundleTable={};
            offset=nextIssueOffset=0;total=2320;loadEpoch=!loadEpoch;state=State::Route;
        } else {
            // A clean disable drains holders but preserves a complete image;
            // a partial aborted image remains invalid until explicit LOAD.
            if (!initialLoad && !(status&(Load|Enabled|Detail)) && !partial && !stopping) {
                state=State::Idle;
                return;
            }
            if(hooks.clearLocal)hooks.clearLocal();
            active.reset();
            if(stopping || !(status&(Load|Enabled|Detail)) || !desired || kind==0) {
                state=State::Idle;
                if(initialLoad){prepared=true;partial=false;status&=~Load;initialLoad=false;}
            } else {
                loading=desired;partial=true;image.clear();
                innerStarted=false;installRow=0;offset=nextIssueOffset=0;
                total=kind==1?144:16784;loadEpoch=!loadEpoch;state=State::Image;
            }
        }
    }
    if(state==State::Route) {
        if(!consumeRouteBeat()){fail(1);return;}
        if(offset==total && readsEmpty()) {
            desired=initialValid?std::optional<unsigned>(initialId):std::nullopt;
            logicalOwner=desired;tableReady=true;state=State::Drain;
        }
    } else if(state==State::Install) {
        if(loading!=desired || !(status&(Load|Enabled|Detail))) {
            partial=true;state=State::Abort;
        } else if(readsEmpty() && offset==total) {
            // The last physical row write completed on the previous tick.
            activateImage();
        } else if(!consumeImageBeat()){fail(1);return;}
    }
    // Trace uses the same real transport. A pending sample never disappears
    // merely because loader ownership changed; table traffic may be delayed.
    serviceTrace();
    submitDma();
    if(stopping && state==State::Idle && drained && !leasedSlots() &&
       !fold && !writing && traceBuffersEmpty() && boundaryTrace.empty() && ordinaryTrace.empty()) {
        if(!traceFault && !finalQueued && reserved<capacity()) {
            Record r{};field(r,kind==0?54:166,1,1);
            ordinaryTrace.push_back({r,reserved++});finalQueued=true;return;
        }
        if(hooks.profileStage)hooks.profileStage(kind,false);
        status&=~(Discovery|Detail);stopping=false;
    }
}
}
