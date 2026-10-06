#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "sparsepp.h"

struct Marker {
    unsigned long value;
};

int main() {
    constexpr int worker_count = 12;
    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;

    // Each thread owns its map and pointee. Only Sparsepp's allocation table
    // is shared. Start together before this map specialization is first used.
    for (int i = 0; i < worker_count; ++i) {
        workers.emplace_back([&] {
            Marker marker{42};
            ready.fetch_add(1);
            while (!start.load()) {
                std::this_thread::yield();
            }
            for (int iteration = 0; iteration < 100; ++iteration) {
                spp::sparse_hash_map<std::string, Marker*> index;
                index.emplace("isImportant", &marker);
                index.emplace("isHidden", &marker);
                index.emplace("referenceId", &marker);
                if (index.size() != 3) {
                    failed.store(true);
                }
                for (const auto* field : {"isImportant", "isHidden", "referenceId"}) {
                    auto entry = index.find(field);
                    if (entry == index.end() || entry->second != &marker) {
                        failed.store(true);
                    }
                }
            }
        });
    }
    while (ready.load() != worker_count) {
        std::this_thread::yield();
    }
    start.store(true);
    for (auto& worker : workers) {
        worker.join();
    }
    return failed.load() ? 1 : 0;
}
