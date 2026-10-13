#include "ui/ProxyQueue.h"

#include "core/Proxy.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QSettings>

ProxyQueue::ProxyQueue(QObject* parent) : QObject(parent)
{
    m_thread = std::thread([this] { run(); });
}

ProxyQueue::~ProxyQueue()
{
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
        m_jobs.clear();
    }
    m_cancel = true;
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

int ProxyQueue::height()
{
    const int h = QSettings().value(QStringLiteral("proxy/height"), 540).toInt();
    return h == 360 || h == 720 ? h : 540;
}

void ProxyQueue::setHeight(int lines)
{
    QSettings().setValue(QStringLiteral("proxy/height"), lines);
}

void ProxyQueue::add(int mediaId, const QString& path, int stream, const QString& name)
{
    if (proxy::exists(path, stream))
        return;
    const QString key = path + QLatin1Char('#') + QString::number(stream);
    {
        std::lock_guard lock(m_mutex);
        if (m_current == key)
            return;
        for (const Job& j : m_jobs)
            if (j.path == path && j.stream == stream)
                return;
        m_jobs.push_back({mediaId, path, stream, name});
    }
    m_cv.notify_all();
}

void ProxyQueue::cancelAll()
{
    {
        std::lock_guard lock(m_mutex);
        m_jobs.clear();
    }
    m_cancel = true;
}

bool ProxyQueue::busy() const
{
    std::lock_guard lock(m_mutex);
    return m_working || !m_jobs.empty();
}

int ProxyQueue::pending() const
{
    std::lock_guard lock(m_mutex);
    return int(m_jobs.size()) + (m_working ? 1 : 0);
}

void ProxyQueue::run()
{
    std::unique_lock lock(m_mutex);
    for (;;) {
        m_cv.wait(lock, [&] { return m_quit || !m_jobs.empty(); });
        if (m_quit)
            return;
        const Job job = m_jobs.front();
        m_jobs.pop_front();
        m_working = true;
        m_current = job.path + QLatin1Char('#') + QString::number(job.stream);
        m_cancel = false;
        const int lines = height();
        lock.unlock();

        QElapsedTimer sinceReport;
        sinceReport.start();
        auto report = [&](double f) {
            int left = 0;
            {
                std::lock_guard l(m_mutex);
                left = int(m_jobs.size());
            }
            QMetaObject::invokeMethod(this, [this, name = job.name, f, left] { emit progress(name, f, left); }, Qt::QueuedConnection);
        };
        report(0);
        QString error;
        const bool ok = proxy::make(job.path, job.stream, lines, [&](double f) {
            if (sinceReport.elapsed() > 150) {
                sinceReport.restart();
                report(f);
            }
            return !m_cancel.load();
        }, &error);

        lock.lock();
        m_working = false;
        m_current.clear();
        const bool none = m_jobs.empty();
        lock.unlock();
        QMetaObject::invokeMethod(this, [this, id = job.mediaId, ok, error, none] {
            emit finished(id, ok, error);
            if (none)
                emit idle();
        }, Qt::QueuedConnection);
        lock.lock();
    }
}
