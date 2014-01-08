#ifndef NW_h
#define NW_h

#include <vector>
#include <string>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

class NW
{
public:
    NW();
    virtual ~NW();

    struct IP
    {
        IP(unsigned char aa = 0, unsigned char bb = 0, unsigned char cc = 0, unsigned dd = 0)
            : a(aa), b(bb), c(cc), d(dd)
        {}

        static IP localhost() { return IP(127, 0, 0, 1); }
        static IP any() { return IP(); }

        std::string toString() const
        {
            char buf[16];
            int w = snprintf(buf, sizeof(buf), "%d.%d.%d.%d", a, b, c, d);
            return std::string(buf, w);
        }

        bool operator==(const IP &o) const { return (a == o.a && b == o.b && c == o.c && d == o.d); }
        bool operator!=(const IP &o) const { return (a != o.a || b != o.b || c != o.c || d != o.d); }

        unsigned char a, b, c, d;
    };

    struct Interface
    {
        Interface()
            : ip(IP::any()), port(0)
        {}

        IP ip;
        uint16_t port;

        std::string toString() const
        {
            std::string ret = ip.toString();
            char buf[32];
            const int w = snprintf(buf, sizeof(buf), "%s:%d", ip.toString().c_str(), port);
            return std::string(buf, w);
        }
    };
    void setInterface(const Interface &interface)
    {
        setInterfaces(std::vector<Interface>(1, interface));
    }
    void setInterfaces(const std::vector<Interface> &interfaces);
    std::vector<Interface> interfaces() const;

    enum Error {
        Success,
        SocketInit,
    };
    Error exec();
    void stop();

    class Request
    {
    public:
        int socket() const { return mSocket; }
        Interface localInterface() const { return mLocalInterface; }
        Interface remoteInterface() const { return mRemoteInterface; }
        const std::vector<std::pair<std::string, std::string> > &headers() const { return mHeaders; }
        std::string path() const { return mPath; }

        enum Method {
            NoMethod,
            Get,
            Head,
            Post,
            Put,
            Delete,
            Trace,
            Connect
        };
        Method method() const { return mMethod; }

        enum Version {
            NoVersion,
            V1_0,
            V1_1
        };

        Version version() const { return mVersion; }

        enum Connection {
            NoConnection,
            KeepAlive,
            Close
        };

        Connection connection() const { return mConnection; }
        int contentLength() const { return mContentLength; }
        int read(char *buf, int max);
    private:
        Request(int socket, const Interface &local, const Interface &remote);
        ~Request();

        int mSocket;
        Interface mLocalInterface, mRemoteInterface;
        Method mMethod;
        Version mVersion;
        Connection mConnection;
        std::string mPath;

        std::vector<std::pair<std::string, std::string> > mHeaders;
        char *mBuffer;
        int mBufferLength, mBufferCapacity;
        enum {
            ParseRequest,
            ParseHeaders,
            ParseBody,
            ParseError
        } mState;
        int mContentLength;
        friend class NW;
    };

    virtual void handleConnection(Request *conn) = 0;
    enum LogType {
        Log_Debug,
        Log_Info,
        Log_Error
    };
    virtual void log(LogType, const char *) {}
private:
    void log(LogType level, const char *format, va_list v);
#ifdef __GNUC__
    void info(const char *format, ...) __attribute__ ((format (printf, 2, 3)));
    void debug(const char *format, ...) __attribute__ ((format (printf, 2, 3)));
    void error(const char *format, ...) __attribute__ ((format (printf, 2, 3)));
#else
    void info(const char *format, ...);
    void debug(const char *format, ...);
    void error(const char *format, ...);
#endif
    void parseHeaders(Request *conn, const char *buf, int len);

    enum Mode {
        Read = 0x1,
        Write = 0x2
    };

    Request *acceptConnection(int fd, const Interface &localInterface);
    bool processConnection(Request *connection);
    void wakeup(char byte);

    bool mRunning;
    mutable pthread_mutex_t mMutex;
    pthread_t mThread;
    int mPipe[2];
    bool mInterfacesDirty;
    std::vector<Interface> mInterfaces;
};

#endif
