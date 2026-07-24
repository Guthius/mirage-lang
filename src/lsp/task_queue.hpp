#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace lsp {
    // A unit of work handed from the main (stdin-reading) thread to the single
    // analysis worker thread. `cancelled` is shared with the main thread's
    // in-flight-request table (see Server) so a `$/cancelRequest` can mark it
    // without the worker needing to look anything up; it's null for tasks with
    // no request id (notifications - didOpen/didChange/didClose), which can't
    // be cancelled.
    struct Task {
        std::shared_ptr<std::atomic<bool>> cancelled;
        std::function<void()> run;
    };

    // FIFO handoff queue between the stdin-reading thread and the analysis
    // worker thread. The stdin thread only ever pushes; the worker only ever
    // pops - this is the only synchronization point between them besides
    // transport::OutputChannel, which is what lets DocumentStore itself stay
    // single-owner (worker-thread-only) with no locking of its own.
    //
    // FIFO ordering is what keeps a didChange's buffer update and a
    // subsequent hover/definition for the same document correctly sequenced:
    // both go through this one queue in arrival order, so by the time the
    // worker reaches the read request, the write has already been applied.
    class TaskQueue {
      public:
        void push(Task task) {
            {
                const std::lock_guard lock(mutex_);
                queue_.push_back(std::move(task));
            }
            cv_.notify_one();
        }

        // Blocks up to `timeout` for a task to become available. Returns
        // nullopt on timeout - the worker uses this to periodically service
        // debounced diagnostics even while otherwise idle.
        template <typename Rep, typename Period>
        auto pop(const std::chrono::duration<Rep, Period> timeout) -> std::optional<Task> {
            std::unique_lock lock(mutex_);
            if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) {
                return std::nullopt;
            }
            auto task = std::move(queue_.front());
            queue_.pop_front();
            return task;
        }

      private:
        std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<Task> queue_;
    };
}
