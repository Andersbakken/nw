#include "NW.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef NDEBUG
#define CHECK(op) op
#else
#define CHECK(op)                               \
    do {                                        \
        const int retVal = op;                  \
        assert(!retVal);                        \
    } while (0)
#endif

#define EINTRWRAP(op)                           \
    do {                                        \
        ret = op;                               \
    } while (ret == -1 && errno == EINTR);

struct LockScope
{
    LockScope(pthread_mutex_t &mutex)
        : mMutex(mutex)
    {
        CHECK(pthread_mutex_lock(&mMutex));
    }
    ~LockScope()
    {
        CHECK(pthread_mutex_unlock(&mMutex));
    }

    pthread_mutex_t mMutex;
};

NW::NW()
    : mRunning(false), mInterfacesDirty(false)
{
    signal(SIGPIPE, SIG_IGN);
    CHECK(pthread_mutex_init(&mMutex, 0));
    CHECK(pipe(mPipe));
}

NW::~NW()
{
    stop();
    close(mPipe[0]);
    close(mPipe[1]);
    CHECK(pthread_mutex_destroy(&mMutex));
}

void NW::setInterfaces(const std::vector<Interface> &interfaces)
{
    {
        LockScope scope(mMutex);
        mInterfaces = interfaces;
        mInterfacesDirty = true;
        if (mRunning)
            wakeup('i');
    }
}

void NW::stop()
{
    bool join = false;
    {
        LockScope scope(mMutex);
        if (mRunning) {
            mRunning = false;
            if (!pthread_equal(pthread_self(), mThread)) {
                wakeup('q');
                join = true;
            }
        }
    }
    if (join) {
        void *retval;
        pthread_join(mThread, &retval);
    }
}

int NW::Connection::contentLength() const
{

}

int NW::Connection::read(char *buf, int max)
{

}

bool NW::exec()
{
    {
        LockScope scope(mMutex);
        assert(!mRunning);
        mRunning = true;
    }

    std::vector<std::pair<Interface, ::socket> > interfaces;
    while (true) {
        fd_set r, w;
        {
            LockScope scope(mMutex);
            if (!mRunning) {
                mRunning = false;
                break;
            }
            if (mInterfacesDirty) {
                mInterfacesDirty = false;
                std::vector<std::pair<Interface, int> >::const_iterator end = interfaces.end();
                std::vector<std::pair<Interface, int> >::const_iterator it = interfaces.begin();
                while (it != end) {
                    if (it->second)
                        close(it->second);
                    ++it;
                }

                interfaces = mInterfaces;
            }
        }
        FD_ZERO
    }
}

void * NW::thread(void *userData)
{
    assert(userData);
    NW *self = reinterpret_cast<NW*>(userData);
    self->exec();
    return 0;
}

void NW::wakeup(char byte)
{
    int ret;
    EINTRWRAP(write(mPipe[1], &byte, sizeof(byte)));
}

std::vector<Interface> NW::interfaces() const
{
    LockScope scope(mMutex);
    return mInterfaces;
}
