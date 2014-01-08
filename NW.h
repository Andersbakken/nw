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

        bool operator==(const IP &o) const { return (a == o.a && b == o.b && c == o.c && d == o.d); }
        bool operator!=(const IP &o) const { return (a != o.a || b != o.b || c != o.c || d != o.d); }

        unsigned char a, b, c, d;
    };

    struct Interface
    {
        Interface()
            : ip(IP::any()), port(0), backlog(128)
        {}

        IP ip;
        uint16_t port, backlog;
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
        const std::vector<std::string> &headers() const { return mHeaders; }

        int contentLength() const;
        int read(char *buf, int max);
    private:
        Connection(int socket, const Interface &local, const Interface &remote);
        ~Connection();

        int mSocket;
        Interface mLocalInterface, mRemoteInterface;

        std::vector<std::string> mHeaders;
        char *mBuffer;
        int mBufferLength;
        friend class NW;
    };

    virtual void handleConnection(Connection *conn) = 0;
    enum LogType {
        Debug,
        Error
    };
    virtual void log(LogType, const char *) {}
private:
    enum Mode {
        Read = 0x1,
        Write = 0x2
    };

    void acceptConnection(int fd, const Interface &localInterface);
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
