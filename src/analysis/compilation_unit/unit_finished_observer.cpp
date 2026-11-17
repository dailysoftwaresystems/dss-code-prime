#include "analysis/compilation_unit/unit_finished_observer.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>

namespace dss {

namespace {

struct Entry {
    std::uint64_t                     id;
    UnitFinishedObservers::Callback   callback;
};

// The process's registrations. A function-local static, so a registration made by
// another translation unit's static initializer finds the list constructed.
struct Registry {
    std::mutex         mutex;
    std::vector<Entry> entries;
    std::uint64_t      nextId = 1;   // 0 is the empty Registration
};

Registry& registry() {
    static Registry r;
    return r;
}

void removeRegistration(std::uint64_t id) noexcept {
    if (id == 0) return;
    Registry& r = registry();
    std::lock_guard<std::mutex> lock{r.mutex};
    std::erase_if(r.entries, [id](Entry const& e) { return e.id == id; });
}

} // namespace

UnitFinishedObservers::Registration::Registration(Registration&& other) noexcept
    : id_{std::exchange(other.id_, 0)} {}

UnitFinishedObservers::Registration&
UnitFinishedObservers::Registration::operator=(Registration&& other) noexcept {
    if (this != &other) {
        removeRegistration(id_);
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

UnitFinishedObservers::Registration::~Registration() { removeRegistration(id_); }

UnitFinishedObservers::Registration UnitFinishedObservers::add(Callback callback) {
    if (!callback) {
        std::fputs("dss::UnitFinishedObservers fatal: an empty callback was registered; "
                   "it would throw inside an unrelated builder's finish()\n",
                   stderr);
        std::abort();
    }
    Registry& r = registry();
    std::lock_guard<std::mutex> lock{r.mutex};
    std::uint64_t const id = r.nextId++;
    r.entries.push_back(Entry{id, std::move(callback)});
    return Registration{id};
}

void UnitFinishedObservers::notify(CompilationUnit const& unit) {
    std::vector<Entry> live;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lock{r.mutex};
        if (r.entries.empty()) return;
        live = r.entries;   // the walk below holds no lock: see the header's contract
    }
    for (Entry const& e : live) e.callback(unit);
}

} // namespace dss
