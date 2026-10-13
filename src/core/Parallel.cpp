#include "core/Parallel.h"

#include <QThread>
#include <QThreadPool>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace {

QThreadPool* workerPool()
{
    // Intentionally leaked: avoids shutdown-order problems with static pools.
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool;
        p->setMaxThreadCount(std::max(1, QThread::idealThreadCount() - 1));
        return p;
    }();
    return pool;
}

struct Job {
    std::atomic<int> next{0};
    std::atomic<int> done{0};
    int chunks = 0;
    int n = 0;
    const std::function<void(int, int)>* fn = nullptr;
    std::mutex mutex;
    std::condition_variable cv;

    void work()
    {
        for (;;) {
            const int c = next.fetch_add(1);
            if (c >= chunks)
                return;
            const int b = int(int64_t(c) * n / chunks);
            const int e = int(int64_t(c + 1) * n / chunks);
            (*fn)(b, e);
            if (done.fetch_add(1) + 1 == chunks) {
                std::lock_guard lock(mutex);
                cv.notify_all();
            }
        }
    }
};

} // namespace

void parallelFor(int n, const std::function<void(int, int)>& fn, int minChunk)
{
    if (n <= 0)
        return;
    QThreadPool* pool = workerPool();
    const int threads = pool->maxThreadCount() + 1;
    const int chunks = std::min(threads * 4, (n + std::max(1, minChunk) - 1) / std::max(1, minChunk));
    if (chunks <= 1) {
        fn(0, n);
        return;
    }
    auto job = std::make_shared<Job>();
    job->chunks = chunks;
    job->n = n;
    job->fn = &fn; // only dereferenced while chunks remain, i.e. before we return
    const int helpers = std::min(threads - 1, chunks - 1);
    for (int i = 0; i < helpers; ++i)
        pool->start([job] { job->work(); });
    job->work();
    std::unique_lock lock(job->mutex);
    job->cv.wait(lock, [&] { return job->done.load() == job->chunks; });
}
