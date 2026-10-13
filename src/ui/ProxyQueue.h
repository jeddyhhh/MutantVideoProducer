#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// Makes proxies (core/Proxy.h) one after another on a background thread,
// reporting progress for the status bar.
class ProxyQueue : public QObject {
    Q_OBJECT
public:
    explicit ProxyQueue(QObject* parent = nullptr);
    ~ProxyQueue() override;

    // Queues a stream (nothing if it already has a proxy or is queued).
    void add(int mediaId, const QString& path, int stream, const QString& name);
    // Stops the one being made and drops the rest.
    void cancelAll();
    bool busy() const;
    int pending() const; // queued plus the one being made
    // Proxy height in lines (360, 540, 720); QSettings "proxy/height".
    static int height();
    static void setHeight(int lines);

signals:
    // While making one: its name, how far (0..1), how many are left after it.
    void progress(const QString& name, double fraction, int left);
    void finished(int mediaId, bool ok, const QString& error);
    // Nothing left to do.
    void idle();

private:
    struct Job {
        int mediaId;
        QString path;
        int stream;
        QString name;
    };
    void run();

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Job> m_jobs;
    bool m_working = false;
    QString m_current; // path#stream being made
    std::atomic<bool> m_cancel{false};
    bool m_quit = false;
    std::thread m_thread;
};
