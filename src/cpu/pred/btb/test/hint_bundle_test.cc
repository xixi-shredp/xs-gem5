#include "cpu/pred/btb/test/hint_bundle_test.hh"
#include <algorithm>
#include <optional>
#include <map>
#include "base/logging.hh"
#include "cpu/pred/btb/hint_bundle.hh"
#include "cpu/pred/btb/hint_indirect.hh"
#include "cpu/pred/btb/hint_history.hh"
namespace gem5::branch_prediction::btb_pred {
namespace {
void check(bool ok,const char *message) { fatal_if(!ok,"Bundle production helper: %s",message); }
void put(std::vector<uint8_t>& data,unsigned at,uint64_t word,unsigned bytes) {
    for(unsigned i=0;i<bytes;++i)data.at(at+i)=word>>(8*i);
}
uint32_t hash(Addr p,Addr t) {
    __uint128_t bits=(__uint128_t((p>>1)&((1ULL<<47)-1))<<47)|((t>>1)&((1ULL<<47)-1));
    uint32_t value=0;for(unsigned bit=0;bit<94;++bit)if((bits>>bit)&1)value^=1U<<(bit%24);
    return value;
}
struct Harness {
    using Controller=HintBundleController;
    using Token=Controller::DmaToken;
    static constexpr uint64_t Valid=1ULL<<63,Enabled=1ULL<<62,Disc=1ULL<<61,
        Detail=1ULL<<60,Load=1ULL<<59,Bundle=1ULL<<51,Busy=1ULL<<50;
    static constexpr uint64_t Base=Valid|Bundle|(1ULL<<15)|1;
    struct Transfer { Token token;Addr addr;bool write;unsigned size;Controller::DmaDone done; };
    struct PoolSlot {
        Controller::DmaPayload data{};uint64_t generation=0;
        bool leased=false,submitted=false,responded=false;
    };
    struct Attempt { Addr addr;bool write;std::vector<uint8_t> bytes;unsigned size; };
    std::array<PoolSlot,64> pool{};
    std::array<std::optional<Transfer>,64> transfers{};
    std::vector<Attempt> attempts;
    std::vector<Controller::TraceAudit> events;
    HintIndirect::Config cfg;
    std::unique_ptr<HintIndirect> hint;
    HintFiniteHistory history;
    std::vector<uint8_t> memory=std::vector<uint8_t>(0x600000);
    std::unique_ptr<Controller> ctl;
    bool drained=true,admission=false,capture=false;
    unsigned rejectDma=0;bool syncFailure=false;
    std::optional<unsigned> failInstallAt;
    uint64_t cycle=1,reads=0,writes=0,installs=0,lastInstallCycle=0,lastSubmitCycle=0;
    unsigned peakLeases=0,peakTransfers=0;
    explicit Harness(unsigned kind=2) {
        cfg.bundle=true;cfg.confQueueDepth=32;
        hint=std::make_unique<HintIndirect>(cfg,nullptr);hint->resetStats(0);
        history.configure(hint->historyPCs());
        Controller::Hooks hooks;
        hooks.quiescent=[this]{return drained&&hint->bundleQuiescent(cycle);};
        hooks.clearLocal=[this]{hint->clearRuntimeTable();history.clearAndRebindLocals(hint->historyPCs());};
        hooks.beginInstall=[this](unsigned k){return hint->beginBundleInstall(k);};
        hooks.installRow=[this](unsigned row,uint64_t value){
            check(lastInstallCycle!=cycle,"more than one physical row installed/tick");
            lastInstallCycle=cycle;
            if(failInstallAt&&row==*failInstallAt)return false;
            ++installs;return hint->installBundleRow(row,value);
        };
        hooks.finishInstall=[this](unsigned k){
            const bool ok=hint->finishBundleInstall(k);
            if(ok) {
                history.clearAndRebindLocals(hint->historyPCs());
            }
            return ok;
        };
        hooks.admission=[this](bool value){admission=value;hint->setRuntimeAdmission(value);};
        hooks.profileStage=[this](unsigned k,bool run){hint->setBundleStage(k,run);};
        hooks.traceAudit=[this](const auto& event){if(capture)events.push_back(event);};
        hooks.dmaAcquire=[this]()->std::optional<Token>{
            for(unsigned i=0;i<pool.size();++i)if(!pool[i].leased) {
                auto& slot=pool[i];slot.leased=true;slot.submitted=slot.responded=false;
                ++slot.generation;peakLeases=std::max(peakLeases,leases());
                return Token{uint8_t(i),slot.generation};
            }
            return std::nullopt;
        };
        hooks.dmaPayload=[this](Token token)->Controller::DmaPayload&{
            auto& slot=pool.at(token.slot);
            check(slot.leased&&slot.generation==token.generation,"payload lease identity/ABA mismatch");
            return slot.data;
        };
        hooks.dmaSubmit=[this](Token token,Addr addr,bool write,unsigned size,Controller::DmaDone done){
            auto& slot=pool.at(token.slot);
            check(slot.leased&&!slot.submitted&&!slot.responded&&slot.generation==token.generation,
                  "duplicate/stale lease submission");
            check(size&&size<=64&&(addr/64)==((addr+size-1)/64),"DMA crossed64B or invalid size");
            check(lastSubmitCycle!=cycle,"more than one DMA submit/tick");lastSubmitCycle=cycle;
            if(capture)attempts.push_back({addr,write,write?std::vector<uint8_t>(slot.data.begin(),slot.data.begin()+size):std::vector<uint8_t>{},size});
            if(rejectDma){--rejectDma;return false;}
            slot.submitted=true;
            if(syncFailure){syncFailure=false;slot.responded=true;done(token,false,cycle);return true;}
            transfers[token.slot]=Transfer{token,addr,write,size,std::move(done)};
            peakTransfers=std::max(peakTransfers,pending());return true;
        };
        hooks.dmaRelease=[this](Token token){
            auto& slot=pool.at(token.slot);
            check(slot.leased&&slot.generation==token.generation&&(!slot.submitted||slot.responded)&&
                  !transfers[token.slot],"lease reused before true response/consume/retire");
            slot.leased=slot.submitted=slot.responded=false;
        };
        ctl=std::make_unique<Controller>(std::move(hooks));
        std::vector<uint8_t> container(0x108000);
        const char magic[]="H2PBND01";std::copy(magic,magic+8,container.begin());
        container[8]=kind;container[9]=32;put(container,12,3,4);
        put(container,16,(0x2000>>1)*2+1,8);
        for(const auto pair:{std::pair<Addr,unsigned>{0x4000,1},{0x6000,0}}) {
            const auto h=hash(0x2000,pair.first);
            unsigned at=0x110+(h&255)*8;
            if(container[at]&1)at+=4;
            put(container,at,1|((h>>8)<<1)|(pair.second<<17),4);
        }
        if(kind)for(unsigned id=0;id<2;++id) {
            const unsigned at=0x8000+id*0x8000;
            const char *inner=kind==1?"H2PCAM58":"H2PLD058";
            std::copy(inner,inner+8,container.begin()+at);
            put(container,at+16,(0x1000>>1)|(1ULL<<51)|(8ULL<<52),8);
            if(kind==2) {
                const auto key=hintHashV3(0x1000,HintV3Contexts{},0);
                put(container,at+0x90+(key&2047)*8,(key>>11)|(3U<<22)|(1U<<24),4);
                put(container,at+0x4090,(0x2000+id*0x1000)>>1,8);
            }
        }
        std::copy(container.begin(),container.end(),memory.begin()+0x100000);
        ctl->writeCsr(0x5c1,4);ctl->writeCsr(0x5c2,1);
        ctl->writeCsr(0x5c0,Base|Load);
        settle();
        check(!(ctl->readCsr(0x5c0)&Load),"explicit LOAD did not complete");
        check(((ctl->readCsr(0x5c0)>>12)&7)==0,"Buf_SZ must encode16 as000");
        check(!((ctl->readCsr(0x5c0)>>52)&7),"valid container loader error");
    }
    unsigned leases() const {unsigned n=0;for(const auto& s:pool)n+=s.leased;return n;}
    unsigned pending() const {unsigned n=0;for(const auto& t:transfers)n+=bool(t);return n;}
    int find(bool write,bool newest=false) const {
        int found=-1;
        for(unsigned i=0;i<transfers.size();++i)if(transfers[i]&&transfers[i]->write==write&&
            (found<0||(newest?transfers[i]->addr>transfers[found]->addr:transfers[i]->addr<transfers[found]->addr)))found=i;
        return found;
    }
    void respond(unsigned index,bool success=true) {
        check(bool(transfers.at(index)),"response has no accepted operation");
        auto transfer=std::move(*transfers[index]);transfers[index].reset();
        auto& slot=pool[index];check(slot.leased&&slot.submitted&&!slot.responded,"response lost lease");
        check(transfer.addr+transfer.size<=memory.size(),"test transfer bounds");
        if(success) {
            if(transfer.write){++writes;std::copy_n(slot.data.begin(),transfer.size,memory.begin()+transfer.addr);}
            else {++reads;std::copy_n(memory.begin()+transfer.addr,transfer.size,slot.data.begin());}
        }
        slot.responded=true;transfer.done(transfer.token,success,cycle);
        check(slot.leased,"Done incorrectly returned credit before controller consumption");
    }
    void step(bool responses=true) {
        ctl->tick(cycle++);
        check(ctl->leasedSlots()==leases()&&leases()<=64,"controller/CPU shared-credit conservation");
        check(ctl->bufferedLoadBytes()<=4112&&ctl->bufferedTraceBytes()<=4096,"hidden extra payload buffer");
        const auto stats=ctl->bufferStats();
        check(stats.maxRowBytesPerTick<=8&&stats.maxPackBytesPerTick<=8&&
              stats.maxWriteRetireSlotsPerTick<=1&&stats.maxWriteRetireRecordsPerTick<=3&&
              stats.maxSubmitPerTick<=1&&stats.completionDescriptorPeak<=192,"bounded per-tick controller lanes");
        if(responses) {
            int index=find(false);if(index<0)index=find(true);
            if(index>=0)respond(index);
        }
    }
    void settle() {
        for(unsigned i=0;i<50000;++i){if(!ctl->busy())return;step();}
        check(false,"finite controller failed to drain");
    }
    void returnAll(bool success=true) {
        for(int index=find(false,true);index>=0;index=find(false,true))respond(index,success);
        for(int index=find(true,true);index>=0;index=find(true,true))respond(index,success);
    }
    void fillReadWindow() {
        // First packet/header validation precedes body prefetch.
        for(unsigned i=0;i<100&&find(false)<0;++i)step(false);
        const int first=find(false);check(first>=0&&transfers[first]->size==64,"missing real header64");
        respond(first);
        for(unsigned i=0;i<100;++i)step(false);
        check(pending()==64&&leases()==64,"loader did not expose all64 shared credits");
    }
    void stopBudget() {for(unsigned i=0;i<1000000;++i)ctl->retiredInstruction();}
};
}
void verifyHintBundleController() {
    {
        Harness h;
        check(h.reads==300&&h.installs==4144&&h.ctl->bufferStats().maxRowBytesPerTick==8,
              "exact route/image DMA or row count changed");
        check(!h.leases()&&!h.ctl->busy(),"initial load retained lease");
    }
    {
        Harness h;
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Enabled);
        h.ctl->committedBoundary(0x2000,0x4000,1,4);h.fillReadWindow();
        const auto installs=h.installs;
        const int earliest=h.find(false);
        for(int index=h.find(false,true);index>=0&&index!=earliest;index=h.find(false,true)) {
            h.respond(index);h.step(false);
        }
        check(h.installs==installs&&h.leases()==64,"OOO read installed past held earliest or freed ready payload");
        h.respond(earliest);h.settle();
        check(h.ctl->activeMetadata()==1&&h.installs==4144*2&&h.peakTransfers==64,
              "ordered read consumption lost/reordered rows");
    }
    {
        Harness h;h.capture=true;
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Enabled);
        h.ctl->committedBoundary(0x2000,0x4000,1,4);h.fillReadWindow();
        const auto before=h.installs,attempts=h.attempts.size();
        h.ctl->committedBoundary(0x2000,0x6000,2,4);h.step(false);
        for(unsigned i=0;i<10;++i)h.step(false);
        check(h.attempts.size()==attempts,"abort reused epoch before old accepted reads returned");
        h.returnAll();
        for(unsigned i=0;i<63;++i){h.step(false);check(h.installs==before,"aborted read installed old row");}
        h.settle();
        check(h.ctl->activeMetadata()==0&&h.installs==before+4144&&!h.leases(),"abort failed complete epoch drain/restart");
    }
    {
        Harness h;
        h.memory[0x100000]='X';const auto reads=h.reads;
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Load);h.settle();
        check(h.reads==reads+1&&((h.ctl->readCsr(0x5c0)>>52)&7)==1&&!h.leases(),
              "invalid header issued body reads or leaked epoch");
    }
    {
        Harness h;
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Enabled);
        h.ctl->committedBoundary(0x2000,0x4000,1,4);h.fillReadWindow();
        const auto before=h.installs;const int first=h.find(false);
        h.respond(first,false);h.step(false);
        check(h.leases()>0&&h.ctl->busy()&&!h.admission,"read error silently canceled other accepted requests");
        h.returnAll();h.settle();
        check(h.installs==before&&!h.leases()&&((h.ctl->readCsr(0x5c0)>>52)&7)==4,
              "read error installed later data or did not drain");
    }
    {
        Harness h;h.syncFailure=true;
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Load);h.settle();
        check(((h.ctl->readCsr(0x5c0)>>52)&7)==4&&!h.leases()&&!h.pending(),
              "synchronous accepted permission failure reused/lost its lease");
    }
    {
        Harness h;h.failInstallAt=8;
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Enabled);
        h.ctl->committedBoundary(0x2000,0x4000,1,4);h.fillReadWindow();
        h.respond(h.find(false));
        for(unsigned i=0;i<10;++i)h.step(false);
        check(((h.ctl->readCsr(0x5c0)>>52)&7)==1&&h.ctl->busy(),
              "invalid body failed to preserve accepted prefetch drain");
        h.returnAll();h.settle();check(!h.leases()&&!h.admission,"invalid body reenabled partial image");
    }
    {
        Harness h(0);h.capture=true;h.rejectDma=1;
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Disc);
        check(h.ctl->recordDiscovery(0x1000,8,false,1),"Discovery fixture rejected record");
        check(h.ctl->busy(),"queued record invisible to drain");h.step(false);
        check(h.ctl->busy()&&h.ctl->bufferedTraceBytes()==0,"fold-before-slot invisible to drain");
        for(unsigned i=1;i<10;++i)h.ctl->recordDiscovery(0x1000+i*2,8,false,i+1);
        for(unsigned i=0;i<100;++i)h.step(false);
        check(h.pending()==4&&h.leases()==4&&h.ctl->writtenRecords()==0,"write packer did not retain four unacknowledged packets");
        check(h.attempts.size()==5&&h.attempts[0].bytes==h.attempts[1].bytes&&
              h.attempts[0].addr==h.attempts[1].addr,"false-submit retry mutated packet");
        const int first=h.find(true);
        for(int index=h.find(true,true);index!=first;index=h.find(true,true)){h.respond(index);h.step(false);}
        check(h.ctl->writtenRecords()==0&&h.leases()==4,"later WriteResp crossed held prefix or freed payload");
        h.respond(first);check(h.ctl->writtenRecords()==0,"response bypassed tick retirement");
        h.step(false);check(h.ctl->writtenRecords()==2,"first64B response completed split third record");
        h.step(false);check(h.ctl->writtenRecords()==5,"second64B response lost endings");
        h.settle();check(h.ctl->writtenRecords()==10,"OOO completion lost tail records");
        h.stopBudget();
        for(unsigned i=0;i<100&&h.find(true)<0;++i)h.step(false);
        check(h.find(true)>=0&&(h.ctl->readCsr(0x5c0)&Harness::Disc)&&h.ctl->writtenRecords()==10,
              "final completed without true response");
        h.respond(h.find(true));check(h.ctl->writtenRecords()==10,"final response bypassed retire tick");
        h.settle();
        check(h.ctl->writtenRecords()==11&&h.memory[0x400000+10*22+6]==0x40&&
              !(h.ctl->readCsr(0x5c0)&Harness::Disc),"final last byte did not govern Started");
        for(const auto& event:h.events)if(event.event=="write")
            check(event.retireCycle==event.cycle&&event.responseCycle<=event.retireCycle,
                  "real response time replaced by retirement time");
    }
    {
        Harness h(0);h.capture=true;h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Disc);
        for(unsigned i=0;i<10;++i)h.ctl->recordDiscovery(0x1000+i*2,8,false,i+1);
        for(unsigned i=0;i<100;++i)h.step(false);
        const int first=h.find(true),last=h.find(true,true);
        check(h.pending()==4&&first!=last,"write fault fixture missing actual packets");
        int failed=-1;
        for(unsigned i=0;i<h.transfers.size();++i)if(h.transfers[i]&&h.transfers[i]->addr==0x400040)failed=i;
        check(failed>=0,"write fault fixture missing second packet");h.respond(failed,false);
        for(int index=h.find(true,true);index>=0&&index!=first;index=h.find(true,true))h.respond(index);
        for(unsigned i=0;i<10;++i)h.step(false);
        check(h.ctl->writtenRecords()==0&&h.ctl->busy(),"failed prefix hole fabricated success");
        h.respond(first);h.settle();
        check(h.ctl->writtenRecords()==2&&((h.ctl->readCsr(0x5c0)>>56)&7)==4&&
              ((h.ctl->readCsr(0x5c0)>>52)&7)==4&&!h.leases(),"P4 lost true prefix or published later success");
        check(std::count_if(h.events.begin(),h.events.end(),[](const auto& e){return e.event=="write";})==2,
              "P4 emitted audit for failed/later records or final");
    }
    {
        Harness h(0);h.capture=true;h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Disc);
        uint64_t seq=1;
        // Slow arrivals fill64 payload slots while routing and packing keep up.
        while(h.leases()<64&&seq<1000) {
            h.ctl->committedBoundary(0x2000,0x4000,seq++,4);
            for(unsigned i=0;i<8;++i)h.step(false);
        }
        check(h.leases()==64&&h.pending()==64&&h.ctl->bufferStats().sharedSlotPeak==64,
              "shared pool did not enforce/expose all64 write credits");
        for(unsigned i=0;i<100&&h.ctl->counters().boundaryLost==0;++i) {
            h.ctl->committedBoundary(0x2000,0x4000,seq++,4);h.step(false);
        }
        check(h.ctl->counters().boundaryLost==1&&h.ctl->bufferStats().boundaryOwnerPeak==64,
              "full pool silently lost Boundary or exceeded true owner64");
        h.returnAll();h.settle();
        std::vector<uint64_t> accepted,written;unsigned pending=0,peak=0;
        for(const auto& event:h.events)if(event.boundary) {
            if(event.event=="accept"){accepted.push_back(event.reservationIndex);++pending;peak=std::max(peak,pending);}
            if(event.event=="write"){written.push_back(event.reservationIndex);check(pending>0,"Boundary write lacks acceptance");--pending;}
            check(event.event!="overwrite","Boundary overwritten");
        }
        check(accepted==written&&!pending&&peak<=258&&h.ctl->bufferStats().completionDescriptorPeak<=192&&
              ((h.ctl->readCsr(0x5c0)>>56)&7)==3&&!h.leases(),"P3 failed accepted-order drain/physical258 bound");
    }
    {
        Harness h;
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Enabled);
        h.ctl->committedBoundary(0x2000,0x4000,1,4);h.fillReadWindow();
        h.ctl->writeCsr(0x5c0,Harness::Base);
        check(!h.admission&&h.ctl->busy(),"disable bypassed pending read ownership");
        h.returnAll();h.settle();
        check(!h.leases()&&!h.admission&&!(h.ctl->readCsr(0x5c0)&Harness::Enabled),
              "disable failed to drain all accepted old-epoch reads");
    }
    {
        Harness h(1);h.capture=true;
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Detail);
        const std::array<uint32_t,4> contexts{1,2,3,4};
        h.ctl->committedBoundary(0x2000,0x4000,1,4);
        for(unsigned i=0;i<100;++i){h.ctl->recordDetail(0,0x3000,contexts,true,true,0,i+2,0x1010);h.step();}
        h.stopBudget();h.settle();
        check(h.ctl->bufferStats().readSlotPeak&&h.ctl->bufferStats().writeSlotPeak&&
              h.ctl->bufferStats().sharedSlotPeak<=64&&!h.leases(),"read/write did not share one finite pool");
        std::map<uint64_t,uint64_t> latest;
        unsigned overwrites=0;
        for(const auto& event:h.events) {
            if(event.event=="accept")check(latest.emplace(event.reservationIndex,event.sequence).second,
                                           "ordinary overwrite allocated duplicate reservation");
            else if(event.event=="overwrite") {
                const auto found=latest.find(event.reservationIndex);
                check(found!=latest.end(),"overwrite lacks accepted pending reservation");
                found->second=event.sequence;++overwrites;
            } else if(event.event=="write") {
                const auto found=latest.find(event.reservationIndex);
                if(found!=latest.end()) {
                    check(found->second==event.sequence,"ordinary write lost most recent overwrite identity");
                    latest.erase(found);
                } else check(event.recordIndex+1==h.ctl->writtenRecords()&&!event.sequence,
                             "unaccepted write is not internal final");
            }
        }
        check(latest.empty()&&overwrites==h.ctl->counters().ordinaryLost,
              "P1 ordinary overwrite/reservation drain mismatch");
        const auto last=h.ctl->writtenRecords()-1;
        check(h.memory[0x400000+last*22+20]==0x40&&!(h.ctl->readCsr(0x5c0)&Harness::Detail),
              "Detail final bit166/drain mismatch");
    }
    // Retained baseline CSR/table/history/measurement/capacity regressions.
    {
        HintIndirect::Config cfg;
        cfg.bundle=true;
        HintIndirect hint(cfg,nullptr);
        check(!hint.complete(nullptr,0,0,false,false,false,false,0),
              "null fall-through query must not produce an override");
        auto q=std::make_shared<HintQuery>();
        // Full-CPU empty-BTB startup: fall-through target is meaningful even
        // though selected_pc/selected_size describe no control instruction.
        check(!hint.complete(q,0,0x80000040,false,false,false,false,0)&&
              q->completed&&!q->baselineValid&&!q->eligible&&q->baselinePC==0,
              "empty-table fall-through must not require a control size");
        check(!hint.tableReferenced(nullptr)&&!hint.tableReferenced(q),
              "non-candidate query must not hold a table binding");
        q->hasCandidate=true;
        check(hint.tableReferenced(q),"live candidate must hold a table binding");
        q->canceled=true;
        check(hint.tableReferenced(q),
              "canceled FTQ candidate must retain its old local binding");
    }
    {
        Harness h;
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Enabled);
        h.hint->setMeasurementActive(true,h.cycle);
        auto q=h.hint->query(std::make_shared<const HintSnapshot>(h.history),
                             0x1000,0x1040,h.cycle);
        check(q->tagHit&&q->conf==3,"runtime reset regression requires real Context hit");
        h.hint->complete(q,0x1010,0x3000,true,true,false,true);
        h.hint->reserve(q,0x1010,h.history.localPhase(0),&h.history);
        h.hint->bind(q,1);
        h.hint->resolve(q,0x100e,0x4000,1,h.cycle,true,true,true,false,4);
        h.hint->commit(q,1,0x100e,0x4000,true,0x2000,h.cycle,
                       nullptr,0,0x1000,4);
        check(h.hint->pendingUpdates()==1&&h.hint->totals().executions==1,
              "runtime reset fixture did not retain an accepted confidence request");
        h.hint->resetStats(h.cycle);
        check(h.hint->totals().executions==0&&h.hint->pendingUpdates()==1&&
              h.hint->queueCounts().carriedIn==1&&h.hint->inFlightHints()==1&&
              h.hint->confidenceAtEntry(q->entryId)==3&&h.hint->canUseTable(),
              "runtime stats reset dropped counters, FIFO carry-in, metadata or active table");
        // Prediction owns Context read atC+1; the accepted write follows atC+2.
        h.hint->tick(h.cycle+3);
        check(h.hint->confidenceAtEntry(q->entryId)==2&&
              h.hint->queueCounts().applied==1,
              "runtime reset lost accepted request's subsequent SRAM write");
        h.hint->finish();
        check(h.hint->inFlightHints()==1,
              "runtime finish incorrectly drained architectural metadata");
    }
    {
        Harness h(1);
        h.ctl->writeCsr(0x5c0,Harness::Base|(1ULL<<60));
        bool emitted=false,observedMiss=true;
        h.hint->setBundleDetailSink([&](uint8_t,Addr,const auto&,bool miss,bool valid){
            emitted=valid;observedMiss=miss;
        });
        h.hint->setMeasurementActive(true,h.cycle);
        auto q=h.hint->query(std::make_shared<const HintSnapshot>(h.history,true),
                             0x1000,0x1040,h.cycle);
        h.hint->complete(q,0x100e,0x3000,true,true,false,true,4);
        check(q->baselinePC==0x1010,"Bundle Detail baseline did not retain native C coordinate");
        h.hint->commit(q,1,0x100e,0x3000,true,0x3000,h.cycle,
                       nullptr,0,0x1000,4);
        check(emitted&&!observedMiss,"correct32-bit Detail target was counted as baseline miss");
        check(h.hint->totals().executions==1&&h.hint->totals().baselineMisses==0&&
              h.hint->totals().finalMisses==0,"Bundle Detail skipped measurement commit totals");
        h.hint->setMeasurementActive(false,h.cycle);
        emitted=false;
        h.hint->commit(q,2,0x100e,0x4000,true,0x3000,h.cycle,
                       nullptr,0,0x1000,4);
        check(emitted&&observedMiss&&h.hint->totals().executions==1,
              "CSR Detail output or measure-only counters depended on the other window");
    }
    {
        Harness h;
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Enabled);
        h.hint->setMeasurementActive(true,h.cycle);
        auto q=h.hint->query(std::make_shared<const HintSnapshot>(h.history),
                             0x1000,0x1040,h.cycle);
        h.hint->complete(q,0x1010,0x3000,true,true,false,true);
        h.hint->commit(q,1,0x100e,0x4000,true,0x3000,h.cycle,
                       nullptr,0,0x1000,4);
        check(h.hint->totals().executions==1&&h.hint->totals().baselineMisses==1&&
              h.hint->totals().finalMisses==1,"Bundle Hint On bypassed measured miss counters");
    }
    {
        Harness h;
        check(h.ctl->activeMetadata()==0,"initial complete image not activated");
        check(h.installs==4144,"full image skipped finite4144-row installation");
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Enabled);
        check(h.ctl->hintEnabled(),"complete kind2 image not enabled");
        const auto before=h.reads;
        h.ctl->committedBoundary(0x2000,0x6000,1,4);h.step();
        check(h.ctl->activeMetadata()==0&&h.reads==before,"same id unnecessarily reloaded image");
        h.drained=false;h.ctl->committedBoundary(0x2000,0x4000,2,4);h.step();
        check(!h.admission&&h.ctl->busy()&&h.reads==before,"new id bypassed old-reference drain");
        h.drained=true;h.step(false);
        check(h.find(false)>=0,"new image did not issue real DMA");
        h.ctl->committedBoundary(0x2000,0x6000,3,4);
        h.step(false);
        check(!h.admission,"aborted image enabled before old response drained");
        h.step();h.settle();
        check(h.ctl->activeMetadata()==0&&h.ctl->hintEnabled(),"latest desired did not win after late response");
        check(h.ctl->counters().aborted==1,"aborted loader statistic");
        h.ctl->committedBoundary(0x2000,0x4000,4,4);h.step(false);
        h.ctl->writeCsr(0x5c0,h.ctl->readCsr(0x5c0)&~Harness::Enabled);
        h.step();h.settle();
        check(!h.ctl->hintEnabled(),"disable during load automatically reenabled");
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Enabled);
        check(!(h.ctl->readCsr(0x5c0)&(Harness::Enabled|Harness::Valid)),"partial invalid image could reenable without LOAD");
    }
    {
        Harness h(0);
        const auto loaded=h.reads;
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Disc);
        check(h.ctl->recordDiscovery(0x1000,8,true),"Discovery sample not accepted");
        h.ctl->committedBoundary(0x2000,0x4000,1,4);
        for(unsigned i=0;i<40;++i)h.step();
        check(h.reads==loaded,"Discovery boundary incorrectly read metadata slot");
        check(h.writes>0,"Bundle Trace did not use real DMA write interface");
        // Boundary priority is an allowed finite reordering; records remain
        // tightly22B and the Boundary's real P/T has no per-record metadata id.
        check(h.memory[0x400000]&1,"Boundary did not occupy first output record");
        check(!(h.memory[0x400000+22]&1),"Discovery record has wrong format or padding");
    }
    {
        Harness h;
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Enabled);
        h.ctl->committedBoundary(0x2000,0x4000,1,4);
        const auto before=h.installs;
        for(unsigned i=0;i<4000 && h.installs==before;++i)h.step();
        check(h.installs==before+1&&!h.admission,"partially installed image became active");
        h.ctl->writeCsr(0x5c0,h.ctl->readCsr(0x5c0)&~Harness::Enabled);
        h.settle();
        check(h.installs==before+1,"disable issued more old table writes");
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Enabled);
        check(!(h.ctl->readCsr(0x5c0)&Harness::Enabled),"partial Install could reenable");
    }
    {
        Harness h;
        h.memory[0x100000+0x10000+16+7]|=4; // CAM bit58, selected slot1
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Enabled);
        h.ctl->committedBoundary(0x2000,0x4000,1,4);h.settle();
        check(((h.ctl->readCsr(0x5c0)>>52)&7)==1&&!h.admission,
              "row-format failure enabled partial table or omitted InvalidImage");
    }
    {
        Harness h(0);
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Disc);
        for(unsigned i=0;i<17;++i)h.ctl->recordDiscovery(0x1000+i*2,8,false,i);
        check(h.ctl->reservedRecords()==16&&((h.ctl->readCsr(0x5c0)>>56)&7)==1,
              "ordinary overwrite consumed new capacity instead of reusing oldest slot");
        h.ctl->committedBoundary(0x2000,0x4000,20,4);
        for(unsigned i=0;i<100;++i)h.step();
        check(h.memory[0x400000]&1,"ordinary overflow overwrote Boundary record");
    }
    {
        Harness h(0);
        h.ctl->writeCsr(0x5c0,Harness::Base|Harness::Disc);
        const uint64_t slots=(1ULL<<20)/22;
        for(uint64_t i=0;i<slots;++i) {
            check(h.ctl->recordDiscovery(0x1000,8,false,i),"capacity accounting stopped before final full22B slot");
            // Real DMA responses own the reservation until every fragment
            // completes; four ticks allow the naturally aligned22B beats.
            for(unsigned j=0;j<5;++j)h.step();
        }
        h.ctl->committedBoundary(0x2000,0x4000,slots,4);
        h.settle();
        check(h.ctl->reservedRecords()==slots&&h.ctl->writtenRecords()==slots&&
              ((h.ctl->readCsr(0x5c0)>>56)&7)==2&&
              !((h.ctl->readCsr(0x5c0)>>52)&7),
              "normal capacity cutoff lost a reserved record or misreported excluded boundary");
        check(!(h.ctl->readCsr(0x5c0)&Harness::Disc),"capacity stop clearedStarted before all writes drained");
    }

}
}
