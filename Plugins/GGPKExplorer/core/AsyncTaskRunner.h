#pragma once
#include "Result.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace GGPKExp {

class AsyncTaskRunner {
public:
    using ProgressCallback = std::function<void(float percent, const std::string& status)>;

    explicit AsyncTaskRunner(size_t threadCount = 2);
    ~AsyncTaskRunner();

    AsyncTaskRunner(const AsyncTaskRunner&) = delete;
    AsyncTaskRunner& operator=(const AsyncTaskRunner&) = delete;

    std::future<Result<void>> Submit(
        std::function<Result<void>(std::atomic<bool>& cancelFlag, ProgressCallback progress)> task
    );

    template<typename T>
    std::future<Result<T>> Submit(
        std::function<Result<T>(std::atomic<bool>& cancelFlag, ProgressCallback progress)> task
    ) {
        auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
        auto promise = std::make_shared<std::promise<Result<T>>>();
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
                promise->set_value(Result<T>::Fail(std::string("Exception: ") + e.what()));
            } catch (...) {
                promise->set_value(Result<T>::Fail("Unknown exception"));
            }
        };

        {
            std::lock_guard lock(m_mutex);
            m_tasks.push(std::move(work));
        }
        m_cv.notify_one();

        return future;
    }

    void CancelAll();

private:
    void WorkerLoop();

    std::vector<std::thread> m_workers;
    std::queue<std::function<void()>> m_tasks;
    std::vector<std::shared_ptr<std::atomic<bool>>> m_cancelFlags;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_shutdown{false};
};

} // namespace GGPKExp
