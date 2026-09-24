#include "AsyncTaskRunner.h"

namespace GGPKExp {

AsyncTaskRunner::AsyncTaskRunner(size_t threadCount) {
    for (size_t i = 0; i < threadCount; ++i)
        m_workers.emplace_back(&AsyncTaskRunner::WorkerLoop, this);
}

AsyncTaskRunner::~AsyncTaskRunner() {
    CancelAll();
    {
        std::lock_guard lock(m_mutex);
        m_shutdown = true;
    }
    m_cv.notify_all();
    for (auto& w : m_workers) {
        if (w.joinable()) w.join();
    }
}

std::future<Result<void>> AsyncTaskRunner::Submit(
    std::function<Result<void>(std::atomic<bool>& cancelFlag, ProgressCallback progress)> task
) {
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    auto promise = std::make_shared<std::promise<Result<void>>>();
    auto future = promise->get_future();

    {
        std::lock_guard lock(m_mutex);
        m_cancelFlags.push_back(cancelFlag);
    }

    auto work = [task = std::move(task), promise, cancelFlag]() {
        try {
            auto result = task(*cancelFlag, [](float, const std::string&) {});
            promise->set_value(std::move(result));
        } catch (const std::exception& e) {
            promise->set_value(Result<void>::Fail(std::string("Exception: ") + e.what()));
        } catch (...) {
            promise->set_value(Result<void>::Fail("Unknown exception"));
        }
    };

    {
        std::lock_guard lock(m_mutex);
        m_tasks.push(std::move(work));
    }
    m_cv.notify_one();

    return future;
}

void AsyncTaskRunner::CancelAll() {
    std::lock_guard lock(m_mutex);
    for (auto& flag : m_cancelFlags)
        flag->store(true);
}

void AsyncTaskRunner::WorkerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return m_shutdown || !m_tasks.empty(); });
            if (m_shutdown && m_tasks.empty()) return;
            task = std::move(m_tasks.front());
            m_tasks.pop();
        }
        task();
    }
}

} // namespace GGPKExp
