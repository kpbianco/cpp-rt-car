#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <mutex>
#include <chrono>
#include <thread>
#include "backpressure.hpp"
#include "debug.hpp"
#include "kernel_bypass.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <atomic>
#include <deque>
#include <condition_variable>
#include <utility>
#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <io.h>
#include <fcntl.h>
#include <share.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef LOG_DEFAULT_LEVEL
#define LOG_DEFAULT_LEVEL 2   // Info
#endif

class Logger {
public:
    enum class Level : int { Trace=0, Debug=1, Info=2, Warn=3, Error=4, None=5 };

    struct Record {
        Level level;
        std::string msg;
        std::uint64_t seq;
        std::chrono::steady_clock::time_point tp;
        std::thread::id tid;
    };

    struct Sink {
        virtual ~Sink() = default;
        virtual void write(const Record& r) = 0;
        virtual std::size_t dropped() const { return 0; }
    };

    class StdoutSink : public Sink {
    public:
        void write(const Record& r) override {
            std::lock_guard<std::mutex> lk(m_);
            std::fwrite(r.msg.data(), 1, r.msg.size(), stdout);
            std::fwrite("\n", 1, 1, stdout);
        }
    private: std::mutex m_;
    };

    class FileSink : public Sink {
    public:
        explicit FileSink(const std::string& path) : f_(path, std::ios::app) {}
        void write(const Record& r) override {
            if (!f_) return;
            std::lock_guard<std::mutex> lk(m_);
            f_ << r.msg << '\n';
        }
    private:
        std::ofstream f_;
        std::mutex m_;
    };

    class AsyncRingFileSink : public Sink {
    public:
        explicit AsyncRingFileSink(const std::string& path,
                                   std::size_t cap = 1024,
                                   std::chrono::milliseconds syncEvery = std::chrono::milliseconds(100),
                                   bool directSync = false,
                                   std::size_t preallocate = 0)
            : fd_(-1), cap_(cap),
              backpressure_(cap, refillRate(cap, syncEvery)),
              syncEvery_(syncEvery) {
#ifdef _WIN32
            int tmp = -1;
            int oflag = _O_CREAT | _O_APPEND | _O_WRONLY;
#ifdef _O_DSYNC
            if (directSync) oflag |= _O_DSYNC;
#else
            (void)directSync;
#endif
            if (_sopen_s(&tmp, path.c_str(), oflag, _SH_DENYNO,
                          _S_IREAD | _S_IWRITE) == 0) {
                fd_ = tmp;
                if (fd_ >= 0 && preallocate > 0) {
#ifdef _WIN32
                    _chsize_s(fd_, static_cast<long long>(preallocate));
#endif
                }
            }
#else
            int flags = O_CREAT | O_APPEND | O_WRONLY;
            if (directSync) {
                flags |= O_DSYNC;
#ifdef O_DIRECT
                flags |= O_DIRECT;
#endif
            }
            fd_ = ::open(path.c_str(), flags, 0644);
            if (fd_ >= 0 && preallocate > 0) {
#if defined(__linux__)
                ::posix_fallocate(fd_, 0, static_cast<off_t>(preallocate));
#else
                ::ftruncate(fd_, static_cast<off_t>(preallocate));
#endif
            }
#endif
            if (fd_ >= 0) {
                simcore::debug::assert_thread_creation_allowed();
                worker_ = std::thread([this]{ run(); });
            }
        }
        ~AsyncRingFileSink() override {
            {
                std::lock_guard<std::mutex> lk(m_);
                stop_ = true;
            }
            cv_.notify_all();
            if (worker_.joinable()) worker_.join();
            flush();
            if (fd_ >= 0) {
#ifdef _WIN32
                _close(fd_);
#else
                ::close(fd_);
#endif
            }
        }

        void write(const Record& r) override {
            if (fd_ < 0) return;
            if (!backpressure_.try_acquire()) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            std::lock_guard<std::mutex> lk(m_);
            if (queue_.size() >= cap_) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            queue_.push_back(r.msg);
            cv_.notify_one();
        }

        std::size_t dropped() const override { return dropped_.load(); }

    private:
        void flush() {
            if (fd_ >= 0) {
#ifdef _WIN32
                _commit(fd_);
#else
                ::fsync(fd_);
#endif
            }
        }

        void run() {
            std::unique_lock<std::mutex> lk(m_);
            lastSync_ = std::chrono::steady_clock::now();
            while (!stop_ || !queue_.empty()) {
                if (queue_.empty()) {
                    cv_.wait(lk, [this]{ return stop_ || !queue_.empty(); });
                    if (queue_.empty()) continue;
                }
                auto msg = std::move(queue_.front());
                queue_.pop_front();
                lk.unlock();
#ifdef _WIN32
                int wr = _write(fd_, msg.data(), static_cast<unsigned int>(msg.size()));
                (void)wr;
                int wr2 = _write(fd_, "\n", 1);
                (void)wr2;
#else
                ssize_t wr = ::write(fd_, msg.data(), msg.size());
                (void)wr;
                ssize_t wr2 = ::write(fd_, "\n", 1);
                (void)wr2;
#endif
                auto now = std::chrono::steady_clock::now();
                if (now - lastSync_ >= syncEvery_) {
                    flush();
                    lastSync_ = now;
                }
                lk.lock();
            }
            flush();
        }

        int fd_;
        std::deque<std::string> queue_;
        std::size_t cap_;
        simcore::TokenBucket backpressure_;
        std::atomic<std::size_t> dropped_{0};
        std::chrono::milliseconds syncEvery_;
        std::chrono::steady_clock::time_point lastSync_{};
        std::mutex m_;
        std::condition_variable cv_;
        bool stop_ = false;
        std::thread worker_;

        static double refillRate(std::size_t cap,
                                 std::chrono::milliseconds interval) {
            if (cap == 0)
                return 1.0;
            const double seconds =
                static_cast<double>(interval.count()) / 1000.0;
            if (seconds <= 0.0)
                return static_cast<double>(cap);
            const double rate = static_cast<double>(cap) / seconds;
            return rate < 1.0 ? 1.0 : rate;
        }
    };

    class AsyncKernelBypassSink : public Sink {
    public:
        explicit AsyncKernelBypassSink(int fd,
                                       std::size_t cap = 1024,
                                       std::chrono::milliseconds pollEvery = std::chrono::milliseconds(10),
                                       unsigned queueDepth = 64,
                                       int sendFlags = simcore::KernelBypassSocket::default_send_flags())
            : fd_(fd),
              cap_(cap),
              backpressure_(cap, computeRefillRate(cap, pollEvery)),
              pollEvery_(pollEvery),
              sendFlags_(sendFlags),
              bypass_(queueDepth) {
            if (fd_ >= 0) {
                simcore::debug::assert_thread_creation_allowed();
                worker_ = std::thread([this]{ run(); });
            }
        }

        ~AsyncKernelBypassSink() override {
            {
                std::lock_guard<std::mutex> lk(m_);
                stop_ = true;
            }
            cv_.notify_all();
            if (worker_.joinable()) worker_.join();
            bypass_.drain();
        }

        void write(const Record& r) override {
            if (fd_ < 0) return;
            if (!backpressure_.try_acquire()) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            std::lock_guard<std::mutex> lk(m_);
            if (queue_.size() >= cap_) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            queue_.push_back(r.msg);
            cv_.notify_one();
        }

        std::size_t dropped() const override { return dropped_.load(); }

    private:
        static double computeRefillRate(std::size_t cap,
                                        std::chrono::milliseconds interval) {
            if (cap == 0) return 1.0;
            const double seconds = static_cast<double>(interval.count()) / 1000.0;
            if (seconds <= 0.0) return static_cast<double>(cap);
            const double rate = static_cast<double>(cap) / seconds;
            return rate < 1.0 ? 1.0 : rate;
        }

        void run() {
            std::unique_lock<std::mutex> lk(m_);
            while (true) {
                if (queue_.empty()) {
                    if (stop_) break;
                    cv_.wait_for(lk, pollEvery_, [this]{ return stop_ || !queue_.empty(); });
                    if (queue_.empty()) {
                        lk.unlock();
                        bypass_.poll();
                        lk.lock();
                        continue;
                    }
                }
                auto msg = std::move(queue_.front());
                queue_.pop_front();
                lk.unlock();
                if (!bypass_.submit(fd_, msg, sendFlags_)) {
                    bypass_.poll();
                    if (!bypass_.submit(fd_, msg, sendFlags_)) {
                        dropped_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                bypass_.poll();
                lk.lock();
            }
            lk.unlock();
            bypass_.drain();
        }

        int fd_;
        std::deque<std::string> queue_;
        std::size_t cap_;
        simcore::TokenBucket backpressure_;
        std::chrono::milliseconds pollEvery_;
        int sendFlags_;
        simcore::KernelBypassSocket bypass_;
        std::atomic<std::size_t> dropped_{0};
        std::mutex m_;
        std::condition_variable cv_;
        bool stop_ = false;
        std::thread worker_;
    };

    class RingBufferSink : public Sink {
    public:
        explicit RingBufferSink(std::size_t cap=8192) : cap_(cap) {}
        void write(const Record& r) override {
            std::lock_guard<std::mutex> lk(m_);
            if (buf_.size() < cap_) buf_.push_back(r.msg);
            else {
                buf_[head_] = r.msg;
                head_ = (head_ + 1) % cap_;
                wrapped_ = true;
            }
        }
        std::vector<std::string> snapshot() const {
            std::lock_guard<std::mutex> lk(m_);
            if (!wrapped_) return buf_;
            std::vector<std::string> out;
            out.reserve(buf_.size());
            for (std::size_t i=0;i<buf_.size();++i) {
                std::size_t idx = (head_ + i) % buf_.size();
                out.push_back(buf_[idx]);
            }
            return out;
        }
    private:
        std::size_t cap_;
        mutable std::mutex m_;
        std::vector<std::string> buf_;
        std::size_t head_ = 0;
        bool wrapped_ = false;
    };

    explicit Logger(Level lvl = static_cast<Level>(LOG_DEFAULT_LEVEL))
        : level_(static_cast<int>(lvl)) {}

    void setLevel(Level l) { level_.store(static_cast<int>(l), std::memory_order_relaxed); }
    Level level() const { return static_cast<Level>(level_.load(std::memory_order_relaxed)); }

    void addSink(std::shared_ptr<Sink> s) {
        std::lock_guard<std::mutex> lk(sinkMutex_);
        sinks_.push_back(std::move(s));
    }

    std::size_t total_dropped() const {
        std::vector<std::shared_ptr<Sink>> sinksCopy;
        {
            std::lock_guard<std::mutex> lk(sinkMutex_);
            sinksCopy = sinks_;
        }
        std::size_t total = 0;
        for (const auto& s : sinksCopy) {
            if (s) total += s->dropped();
        }
        return total;
    }

    std::size_t dropped() const { return total_dropped(); }

    bool willLog(Level l) const {
        return static_cast<int>(l) >= static_cast<int>(level());
    }

    template<typename... Args>
    void log(Level l, std::string_view fmt, Args&&... args) {
#ifndef LOG_ENABLED
        (void)l; (void)fmt; (void)sizeof...(Args);
#else
        if (!willLog(l)) return;
        writeRecord(l, format(fmt, std::forward<Args>(args)...));
#endif
    }

    template<typename... A> void trace(std::string_view f, A&&... a){ log(Level::Trace,f,std::forward<A>(a)...);}
    template<typename... A> void debug(std::string_view f, A&&... a){ log(Level::Debug,f,std::forward<A>(a)...);}
    template<typename... A> void info (std::string_view f, A&&... a){ log(Level::Info ,f,std::forward<A>(a)...);}
    template<typename... A> void warn (std::string_view f, A&&... a){ log(Level::Warn ,f,std::forward<A>(a)...);}
    template<typename... A> void error(std::string_view f, A&&... a){ log(Level::Error,f,std::forward<A>(a)...);}
    template<typename... A> void telemetry(std::string_view f, A&&... a){ log(Level::Info,f,std::forward<A>(a)...);}

private:
    template<typename... Args>
    static std::string format(std::string_view fmt, Args&&... args) {
        if constexpr (sizeof...(Args) == 0) {
            return std::string(fmt);
        } else {
            std::string repls[] = { toString(std::forward<Args>(args))... };
            std::ostringstream oss;
            std::size_t ai=0;
            for (std::size_t i=0;i<fmt.size();++i) {
                if (fmt[i]=='{' && i+1<fmt.size() && fmt[i+1]=='}') {
                    if (ai < sizeof...(Args)) oss<<repls[ai++];
                    ++i;
                } else oss<<fmt[i];
            }
            return oss.str();
        }
    }
    template<typename T>
    static std::string toString(T&& v){ std::ostringstream o; o<<v; return o.str(); }

    void writeRecord(Level l, std::string msg) {
        Record r{l,std::move(msg),
                 seq_.fetch_add(1,std::memory_order_relaxed),
                 std::chrono::steady_clock::now(),
                 std::this_thread::get_id()};
        std::lock_guard<std::mutex> lk(sinkMutex_);
        for (auto& s : sinks_) s->write(r);
    }

    std::atomic<int> level_;
    std::atomic<std::uint64_t> seq_{0};
    std::vector<std::shared_ptr<Sink>> sinks_;
    mutable std::mutex sinkMutex_;
};

#ifdef LOG_ENABLED
#define LOG_TRACE(L, ...) do{ if(L) (L)->trace(__VA_ARGS__); }while(0)
#define LOG_DEBUG(L, ...) do{ if(L) (L)->debug(__VA_ARGS__); }while(0)
#define LOG_INFO(L,  ...) do{ if(L) (L)->info (__VA_ARGS__); }while(0)
#define LOG_WARN(L,  ...) do{ if(L) (L)->warn (__VA_ARGS__); }while(0)
#define LOG_ERROR(L, ...) do{ if(L) (L)->error(__VA_ARGS__); }while(0)
#define LOG_TELEMETRY(L, ...) do{ if(L) (L)->telemetry(__VA_ARGS__); }while(0)
#else
#define LOG_TRACE(L, ...) do{}while(0)
#define LOG_DEBUG(L, ...) do{}while(0)
#define LOG_INFO(L,  ...) do{}while(0)
#define LOG_WARN(L,  ...) do{}while(0)
#define LOG_ERROR(L, ...) do{}while(0)
#define LOG_TELEMETRY(L, ...) do{}while(0)
#endif
