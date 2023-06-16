#pragma once

#include <codecvt>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <locale>
#include <memory>
#include <mutex>
#include <ostream>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include <string.h>

#define LOGL(level, msg)                                                                           \
    if (Loggy::isLevel(level)) {                                                                   \
        Loggy::writer(level, __FILE__, __LINE__) << msg;                                           \
        Loggy::queue();                                                                            \
    }

#define LOG_FLUSH()                                                                                \
    {                                                                                              \
        Loggy::wait_queues();                                                                      \
    }

#define LOGT(msg) LOGL(Loggy::LTRACE, msg)
#define LOGD(msg) LOGL(Loggy::LDEBUG, msg)
#define LOGI(msg) LOGL(Loggy::LINFO, msg)
#define LOGE(msg) LOGL(Loggy::LERROR, msg)

namespace Loggy {
    using namespace std;

    constexpr int DEFAULT_BUF_CNT = 1000;
    constexpr const char* DEFAULT_TIME_FMT = "%Y%m%d.%H%M%S";
    constexpr double DROP_NOTIFY_SECONDS = 5.0;
    constexpr double FLUSH_SECONDS = 1.0;

    enum {
        LINVALID = 0,
        LTRACE = 9,
        LDEBUG = 10,
        LINFO = 20,
        LERROR = 40,
        LWARN = 30,
        LCRITICAL = 50,
        LMAX = 50,
    };

    unordered_map<int, string> levelNames_ = {
        { LINVALID, "INVALID" },
        { LTRACE, "TRACE" },
        { LDEBUG, "DEBUG" },
        { LINFO, "INFO" },
        { LERROR, "ERROR" },
        { LWARN, "WARN" },
        { LCRITICAL, "CRITICAL" },
    };

    wstring str2w(const string& in)
    {
#ifdef _WIN32
        if (in.empty())
            return std::wstring();
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, &in[0], (int)in.size(), NULL, 0);
        std::wstring wstrTo(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, &in[0], (int)in.size(), &wstrTo[0], size_needed);
        return wstrTo;
#else
        thread_local std::wstring_convert<std::codecvt_utf8<wchar_t>> wcu16;
        return wcu16.from_bytes(in);
#endif
    }

    string w2str(const wstring& in)
    {
#ifdef _WIN32
        if (in.empty())
            return std::string();
        int size_needed
            = WideCharToMultiByte(CP_UTF8, 0, &in[0], (int)in.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &in[0], (int)in.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
#else
        thread_local std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> wcu8;
        return wcu8.to_bytes(in);
#endif
    }


    template <class T> class SafeQueue {
    public:
        SafeQueue(void)
            : q()
            , m()
            , c()
            , x()
        {
        }

        ~SafeQueue(void) { lock_guard<mutex> lock(m); }

        // Add an element to the queue.
        void push(T t)
        {
            lock_guard<mutex> lock(m);
            q.push(t);
            c.notify_one();
        }

        // Get the "front"-element.
        // If the queue is empty, wait till a element is avaiable.
        T pop(void)
        {
            unique_lock<mutex> lock(m);
            while (!x && q.empty()) {
                // release lock as long as the wait and reaquire it afterwards.
                c.wait(lock);
            }

            if (x) {
                return T();
            };

            T val = q.front();
            q.pop();

            if (q.empty()) {
                c.notify_all();
            }
            return val;
        }

        size_t size() { return q.size(); }

        void join(void)
        {
            unique_lock<mutex> lock(m);
            while (!q.empty()) {
                c.wait(lock);
            }
        }

        size_t drain(void)
        {
            unique_lock<mutex> lock(m);
            std::queue<T> empty;
            swap(q, empty);
            c.notify_all();
            return empty.size();
        }

        size_t quit()
        {
            x = true;
            return drain();
        }

    private:
        queue<T> q;
        mutable mutex m;
        condition_variable c;
        bool x;
    };

    static string timestamp(const char format[], const time_t& rawtime)
    {
        struct tm timeinfo;
        char buffer[120];
#ifdef _WIN32
        localtime_s(&timeinfo, &rawtime);
#else
        localtime_r(&rawtime, &timeinfo);
#endif

        strftime(buffer, sizeof(buffer), format, &timeinfo);
        return string(buffer);
    }

#ifdef _WIN32
#define _LOGGY_CVT_FILENAME(s) s
#else
#define _LOGGY_CVT_FILENAME(s) Loggy::w2str(s)
#endif

    class Output {
        SafeQueue<wstring> queue_;  // this should be first
        wofstream fstream_;
        wostream& wstream_;
        size_t max_;
        int level_;
        size_t dropped_ = 0;
        bool alive_ = true;
        time_t firstDrop_ = 0;

        std::thread thread_;  // this must be last

    public:
        Output(wostream& s, int level, int max)
            : wstream_(s)
            , level_(level)
            , max_(max)
            , thread_(&Output::worker, this)
        {
        }

        Output(const wstring& s, int level, size_t max)
            : fstream_(_LOGGY_CVT_FILENAME(s), std::wofstream::out | std::wofstream::app)
            , wstream_(fstream_)
            , level_(level)
            , max_(max)
            , thread_(&Output::worker, this)
        {
        }

        ~Output()
        {
            alive_ = false;
            dropped_ += queue_.quit();
            if (dropped_) {
                logDropped();
            }
            thread_.join();
        }

        void wait() { queue_.join(); wstream_.flush(); }

        void logDropped()
        {
            wstringstream ws;
            time_t t;
            time(&t);
            ws << Loggy::timestamp(DEFAULT_TIME_FMT, t).c_str();
            ws << " dropped " << dropped_ << " entries";
            queue_.push(ws.str());
            dropped_ = 0;
        }

        void add(wstring& str, time_t& t)
        {
            if (alive_) {
                if (max_ == 0 || queue_.size() < max_) {
                    queue_.push(str);
                }
                else {
                    ++dropped_;
                    if (dropped_ == 1) {
                        firstDrop_ = t;
                    }
                    else if (difftime(t, firstDrop_) > DROP_NOTIFY_SECONDS) {
                        logDropped();
                    }
                }
            }
        }

        void worker()
        {
            int written = 0;
            time_t lastFlush = 0;

            while (alive_) {
                if (!queue_.size() && written > 0) {
                    time_t t;
                    time(&t);
                    if (difftime(t, lastFlush) > FLUSH_SECONDS) {
                        wstream_.flush();
                        lastFlush = t;
                        written = 0;
                    }
                }
                auto t = queue_.pop();
                if (alive_) {
                    wstream_ << t << std::endl;
                    written += 1;
                }
            }
        }
    };

    class Log {
    public:
        ~Log() { resetOutput(); };

        int level_ = LINFO;
        int trigFrom_ = LINVALID;
        int trigTo_ = LINVALID;
        int trigCnt_ = LINVALID;
        string timeFormat_ = DEFAULT_TIME_FMT;
        mutex mutex_;

        deque<Output> outputs_;
        Output default_output_;

        vector<wstring> buffer_;

        Log()
            : default_output_(wcout, LINFO, 1) {};

        bool isLevel(int level) { return level >= level_; }

        void resetOutput()
        {
            lock_guard<mutex> lock(mutex_);
            outputs_.clear();
        }

        void addOutput(const wstring& path, int level, int bufferSize)
        {
            lock_guard<mutex> lock(mutex_);
            outputs_.emplace_back(path, level, bufferSize);
        }

        void addOutput(wostream& stream, int level, int bufferSize)
        {
            lock_guard<mutex> lock(mutex_);
            outputs_.emplace_back(stream, level, bufferSize);
        }

        std::vector<const char*> getFiles()
        {
            std::vector<const char*> ret;
            return ret;
        }

        void setTrigger(int levelFrom, int levelTo, int lookbackCount)
        {
            trigFrom_ = levelFrom;
            trigTo_ = levelTo;
            trigCnt_ = lookbackCount;
        }

        void setLevel(int level) { level_ = level; }

        struct LastLog {
            wstringstream ws;
            time_t tm = 0;
        };

        static LastLog& lastLog()
        {
            thread_local LastLog ll_;
            return ll_;
        }

        static const char* basename(const char* file)
        {
            const char* b = strrchr(file, '\\');
            if (!b)
                b = strrchr(file, '/');
            return b ? b + 1 : file;
        }

        static const char* levelname(int level) { return levelNames_[level].c_str(); }

        wostream& writer(int level, const char* file, int line)
        {
            auto& ll = lastLog();
            time(&ll.tm);
            ll.ws.clear();
            ll.ws.str(L"");
            return ll.ws << timestamp(timeFormat_.c_str(), ll.tm).c_str() << " " << basename(file)
                << ":" << line << " " << levelname(level) << " ";
        }

        void queue()
        {
            lock_guard<mutex> lock(mutex_);
            auto& ll = lastLog();
            auto s = ll.ws.str();

            if (outputs_.empty()) {
                default_output_.add(s, ll.tm);
            }
            else {
                for (auto& out : outputs_) {
                    out.add(s, ll.tm);
                }
            }
        }
        void wait_queues()
        {
            if (outputs_.empty()) {
                default_output_.wait();
            }
            else {
                for (auto& out : outputs_) {
                    out.wait();
                }
            }
        }
    };

    static Log& getInstance()
    {
        static Log l;
        return l;
    }

    void resetOutput() { getInstance().resetOutput(); }

    void addOutput(const wstring& path, int level = LDEBUG, int bufferSize = DEFAULT_BUF_CNT)
    {
        getInstance().addOutput(path, level, bufferSize);
    }

    void addOutput(wostream& stream, int level = LDEBUG, int bufferSize = DEFAULT_BUF_CNT)
    {
        getInstance().addOutput(stream, level, bufferSize);
    }

    void setTrigger(int levelFrom, int levelTo, int lookbackCount)
    {
        getInstance().setTrigger(levelFrom, levelTo, lookbackCount);
    }

    std::vector<const char*> getFiles() { return getInstance().getFiles(); }

    void setLevel(int level) { getInstance().setLevel(level); }

    bool isLevel(int level) { return getInstance().isLevel(level); }

    wostream& writer(int level, const char* file, int line)
    {
        return getInstance().writer(level, file, line);
    }

    void queue() { getInstance().queue(); }

    void wait_queues() { getInstance().wait_queues(); }

}  // end namespace Loggy