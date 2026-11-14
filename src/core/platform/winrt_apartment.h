// Running C++/WinRT code where it wants to run: in the multithreaded apartment (MTA), on a thread of
// its own, so the caller's COM state is left alone (the GUI thread is a single-threaded apartment,
// where a blocking .get() on an async operation isn't allowed).
#pragma once

#include <winrt/base.h>

#include <thread>
#include <utility>

namespace avel::platform {

// Runs `work` on a fresh MTA thread and returns its result, which must be default-constructible.
// The thread leaves the apartment only after `work` has returned, so every WinRT object `work`
// made is released while the apartment is alive: when the last thread leaves the MTA, COM tears it
// down and may unload the DLLs behind live objects. `work` must not let an exception escape.
template <typename Work>
auto run_in_mta(Work&& work) {
    decltype(work()) result{};
    std::thread worker([&] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        result = work();
        winrt::uninit_apartment();
    });
    worker.join();
    return result;
}

}  // namespace avel::platform
