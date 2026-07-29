#pragma once

#include <atomic>
#include <string>
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
        // JSON-RPC id this task answers, "" for a notification. Carried so the worker can
        // report completion for a task it skips (cancelled before it was ever popped) or one
        // that throws -- in both cases run_cancellable_request, the only other thing that
        // reports completion, never gets to run.
        std::string id_key;
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
        // The single worker thread can fall behind a client that sends faster than analysis
        // completes, and nothing outside the diagnostics debounce coalesces anything, so the
        // queue could grow without bound over a long session. The cap is far above any
        // realistic backlog -- a client would have to be malfunctioning to reach it.
        static constexpr size_t MAX_PENDING = 4096;

        // Checked by the caller before building a Task, so a rejected request can be answered
        // with an error instead of silently dropped. Only cancellable REQUESTS are gated this
        // way; notifications (didOpen/didChange/didClose) are always accepted, because
        // refusing a buffer update would desynchronize the server's view of the document from
        // the editor's, which is worse than a long queue.
        [[nodiscard]] auto full() -> bool {
            const std::lock_guard lock(mutex_);
            return queue_.size() >= MAX_PENDING;
        }

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
