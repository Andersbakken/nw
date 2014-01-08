#ifndef NW_h
#define NW_h

#include <vector>
#include <string>
#include <pthread.h>
#include <stdint.h>

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

    class Connection
    {
    public:
        int socket() const { return mSocket; }
        Interface localInterface() const { return mLocalInterface; }
        Interface remoteInterface() const { return mRemoteInterface; }
        const std::vector<std::pair<std::string, std::string> > &headers() const { return mHeaders; }
        std::string path() const { return mPath; }

        enum Method {
            Invalid,
            Get,
            Head,
            Post,
            Put,
            Delete,
            Trace,
            Connect
        };
        Method method() const { return mMethod; }

        int contentLength() const { return mContentLength; }
        int read(char *buf, int max);
    private:
        Connection(int socket, const Interface &local, const Interface &remote);
        ~Connection();

        int mSocket;
        Interface mLocalInterface, mRemoteInterface;
        Method mMethod;
        std::string mPath;

        std::vector<std::pair<std::string, std::string> > mHeaders;
        char *mBuffer;
        int mBufferLength, mBufferCapacity;
        enum {
            ParseMethod,
            ParseHeaders,
            ParseBody,
            ParseError

        } mState;
        int mContentLength;
        friend class NW;
    };

    virtual void handleConnection(Connection *conn) = 0;
    enum LogType {
        Log_Debug,
        Log_Error
    };
    virtual void log(LogType, const char *) {}
private:
    void log(LogType level, const char *format, va_list v);
#ifdef __GNUC__
    void debug(const char *format, ...) __attribute__ ((format (printf, 2, 3)));
    void error(const char *format, ...) __attribute__ ((format (printf, 2, 3)));
#else
    void debug(const char *format, ...);
    void error(const char *format, ...);
#endif
    void parseHeaders(Connection *conn, const char *buf, int len);

    enum Mode {
        Read = 0x1,
        Write = 0x2
    };

    Connection *acceptConnection(int fd, const Interface &localInterface);
    void processConnection(Connection *connection);
    void wakeup(char byte);

    bool mRunning;
    mutable pthread_mutex_t mMutex;
    pthread_t mThread;
    int mPipe[2];
    bool mInterfacesDirty;
    std::vector<Interface> mInterfaces;
};

#endif
