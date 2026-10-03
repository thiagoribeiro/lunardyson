// Zero-overhead deadlines. Instead of running an interrupt callback at every
// safepoint, a single watchdog thread arms the runtime's interrupt only when a
// VM slice passes its deadline (Luau allows setting `interrupt` from another
// thread — the REPL's Ctrl-C handler works the same way). ld_interrupt then
// checks the exact VM time and raises the timeout.
#include "internal.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace
{

struct Armed
{
    ld_runtime* rt;
    uint64_t generation;
};

class Watchdog
{
public:
    ~Watchdog()
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }

    void arm(ld_runtime* rt, uint64_t generation, std::chrono::steady_clock::time_point deadline)
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!thread_.joinable())
            thread_ = std::thread([this] { loop(); });
        slices_.emplace(deadline, Armed{rt, generation});
        // Wake the thread only if it would otherwise sleep past this deadline; in steady
        // state it wakes about once per budget window, not once per slice.
        if (deadline < wake_at_)
            cv_.notify_all();
    }

    // Removes the slice and clears the interrupt. Holding the lock guarantees the
    // watchdog cannot arm this runtime after we return.
    void disarm(ld_runtime* rt, uint64_t generation)
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = slices_.begin(); it != slices_.end(); ++it)
        {
            if (it->second.rt == rt && it->second.generation == generation)
            {
                slices_.erase(it);
                break;
            }
        }
        lua_callbacks(rt->L)->interrupt = nullptr;
    }

private:
    void loop()
    {
        std::unique_lock<std::mutex> lock(mu_);
        while (!stop_)
        {
            if (slices_.empty())
            {
                wake_at_ = std::chrono::steady_clock::time_point::max();
                cv_.wait(lock);
                continue;
            }
            auto next = slices_.begin();
            // Copy the deadline by value: cv_.wait_until releases the lock, during which
            // disarm() can erase this slice and free the map node. Waiting on a reference
            // into the node (next->first) would then be a use-after-free (caught by ASan).
            const auto deadline = next->first;
            if (std::chrono::steady_clock::now() < deadline)
            {
                wake_at_ = deadline;
                cv_.wait_until(lock, deadline);
                continue; // re-fetch begin() fresh; `next` may be dangling after the wait
            }
            lua_callbacks(next->second.rt->L)->interrupt = ld_interrupt;
            slices_.erase(next);
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::multimap<std::chrono::steady_clock::time_point, Armed> slices_;
    std::thread thread_;
    std::chrono::steady_clock::time_point wake_at_ = std::chrono::steady_clock::time_point::max();
    bool stop_ = false;
};

Watchdog& watchdog()
{
    static Watchdog instance;
    return instance;
}

} // namespace

void ld_watchdog_arm(ld_runtime* rt, uint64_t generation, std::chrono::steady_clock::time_point deadline)
{
    watchdog().arm(rt, generation, deadline);
}

void ld_watchdog_disarm(ld_runtime* rt, uint64_t generation)
{
    watchdog().disarm(rt, generation);
}
