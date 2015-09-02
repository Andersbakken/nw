#ifndef NW_h
#define NW_h

#include <vector>
#include <string>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <memory>
#include <map>
#include <stdlib.h>

class NW
{
    struct Connection;
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
    void setInterfaces(const std::vector<Interface> &interfaces);
    void setInterface(const Interface &interface) { setInterfaces(std::vector<Interface>(1, interface)); }
    std::vector<Interface> interfaces() const;

    int maxContentLength() const;
    void setMaxContentLength(int maxContentLength);

    enum Error {
        Success,
        SocketInit,
    };
    Error exec();
    void stop();

    class Request
    {
    public:
        ~Request();
        int socket() const { return mSocket; }
        Interface localInterface() const { return mLocalInterface; }
        Interface remoteInterface() const { return mRemoteInterface; }
        const std::vector<std::pair<std::string, std::string> > &headers() const { return mHeaders; }
        std::string path() const { return mPath; }
        bool hasHeader(const std::string &header) const;
        std::string headerValue(const std::string &header) const;
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

        enum ConnectionType {
            NoConnection,
            KeepAlive,
            Close
        };

        ConnectionType connection() const { return mConnectionType; }
        std::string body() const { return mBody; }
    private:
        Request(int socket, const Interface &local, const Interface &remote);

        int mSocket;
        Interface mLocalInterface, mRemoteInterface;
        Method mMethod;
        Version mVersion;
        ConnectionType mConnectionType;
        std::string mPath;

        std::vector<std::pair<std::string, std::string> > mHeaders;
        std::string mBody;

        friend struct Connection;

        friend class NW;
    };

    virtual void handleRequest(const std::shared_ptr<Request> &conn) = 0;
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
    void parseHeaders(const std::shared_ptr<Request> &conn, const char *buf, int len);
    std::shared_ptr<Connection> acceptConnection(int fd, const Interface &localInterface);
    bool processConnection(const std::shared_ptr<Connection> &conn);
    void wakeup(char byte);

    struct Connection
    {
        Connection(int sock, const Interface &local, const Interface &remote)
            : socket(sock), localInterface(local), remoteInterface(remote),
              state(ParseRequestLine), buffer(0), bufferLength(0), bufferCapacity(0),
              contentLength(-1)
        {}
        ~Connection()
        {
            if (buffer)
                free(buffer);
        }
        std::shared_ptr<Request> request;
        int socket;
        Interface localInterface, remoteInterface;
        enum {
            ParseRequestLine,
            ParseHeaders,
            ParseBody,
            Error,
        } state;
        char *buffer;
        int bufferLength, bufferCapacity, contentLength;
    };

    bool mRunning;
    mutable pthread_mutex_t mMutex;
    pthread_t mThread;
    int mPipe[2];
    bool mInterfacesDirty;
    std::vector<Interface> mInterfaces;
    int mMaxContentLength;
};

#endif
