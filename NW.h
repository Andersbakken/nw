#ifndef NW_h
#define NW_h

#include <map>
#include <vector>
#include <string>
#include <pthread.h>

class NW
{
public:
    NW();
    virtual ~NW();

    struct Interface
    {
        std::string host; // empty means all
        int port;
    };
    void setInterface(const Interface &interface)
    {
        setInterfaces(std::vector<Interface>(1, interface));
    }
    void setInterfaces(const std::vector<Interface> &interfaces);
    std::vector<Interface> interfaces() const;

    bool exec();
    void stop();

    struct Connection
    {
        int socket;
        std::vector<std::string> headers;

        int contentLength() const;
        int read(char *buf, int max);
    };

    virtual void handleConnection(Connection *conn) = 0;

private:
    void wakeup(char byte);
    static void *thread(void *);
    void lock();
    void unlock();
    bool mRunning;
    mutable pthread_mutex_t mMutex;
    pthread_t mThread;
    int mPipe[2];
    bool mInterfacesDirty;
    std::vector<Interface> mInterfaces;
};

#endif
