#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <mutex>
#include <chrono>
#include <thread>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <atomic>

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
#else
#define LOG_TRACE(L, ...) do{}while(0)
#define LOG_DEBUG(L, ...) do{}while(0)
#define LOG_INFO(L,  ...) do{}while(0)
#define LOG_WARN(L,  ...) do{}while(0)
#define LOG_ERROR(L, ...) do{}while(0)
#endif
