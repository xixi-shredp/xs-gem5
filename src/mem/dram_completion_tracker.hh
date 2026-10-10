// Bounded by the controller's accepted-transaction limit; no response can be
// released before its DRAM callback plus the configured completion padding.
#ifndef __MEM_DRAM_COMPLETION_TRACKER_HH__
#define __MEM_DRAM_COMPLETION_TRACKER_HH__
#include <cassert>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

namespace gem5 { namespace memory {
template<class PacketHandle> class DramCompletionTracker
{
  public:
    struct Transaction {
        PacketHandle packet;
        bool write;
        bool needsResponse;
        uint64_t ready = 0;
        uint64_t sequence = 0;
    };
  private:
    struct Later {
        bool operator()(const Transaction &a, const Transaction &b) const {
            return a.ready != b.ready ? a.ready > b.ready :
                a.sequence > b.sequence;
        }
    };
    using AddressQueue = std::unordered_map<uint64_t, std::queue<Transaction>>;
    AddressQueue reads, writes;
    std::priority_queue<Transaction, std::vector<Transaction>, Later> pending;
    unsigned readCount = 0, writeCount = 0;
    uint64_t sequence = 0;

  public:
    void accept(uint64_t address, PacketHandle packet, bool write,
                bool needsResponse) {
        (write ? writes : reads)[address].push(
            {packet, write, needsResponse});
        ++(write ? writeCount : readCount);
    }
    void callback(uint64_t address, bool write, uint64_t now,
                  uint64_t padding) {
        auto &addresses = write ? writes : reads;
        const auto it = addresses.find(address);
        assert(it != addresses.end() && !it->second.empty());
        auto transaction = it->second.front();
        it->second.pop();
        if (it->second.empty())
            addresses.erase(it);
        transaction.ready = now + padding;
        assert(transaction.ready >= now);
        transaction.sequence = sequence++;
        pending.push(transaction);
    }
    bool ready(uint64_t now) const {
        return !pending.empty() && pending.top().ready <= now;
    }
    uint64_t nextReady() const {
        assert(!pending.empty());
        return pending.top().ready;
    }
    bool hasPending() const { return !pending.empty(); }
    Transaction takeReady(uint64_t now) {
        assert(ready(now));
        const auto transaction = pending.top();
        pending.pop();
        auto &count = transaction.write ? writeCount : readCount;
        assert(count);
        --count;
        return transaction;
    }
    unsigned outstandingReads() const { return readCount; }
    unsigned outstandingWrites() const { return writeCount; }
    unsigned outstanding() const { return readCount + writeCount; }
    template<class Visitor> void forEachPacket(Visitor visitor) const {
        for (const auto *addresses : {&reads, &writes}) {
            for (const auto &entry : *addresses) {
                auto queue = entry.second;
                while (!queue.empty()) {
                    if (queue.front().packet) visitor(queue.front());
                    queue.pop();
                }
            }
        }
        auto queue = pending;
        while (!queue.empty()) {
            if (queue.top().packet) visitor(queue.top());
            queue.pop();
        }
    }
};
} }
#endif
