#include <NW.h>

class Server : public NW
{
public:
    Server()
    {}

    virtual void handleRequest(const std::shared_ptr<Request> &conn)
    {
        printf("Got request %p\n", conn.get());
    }
    virtual void log(LogType level, const char *string)
    {
        const char *names[] = { "Debug", "Info", "Error" };
        printf("%s: %s", names[level], string);
    }
};


int main(int argc, char **argv)
{
    Server server;
    NW::Interface interface;
    interface.port = 9999;
    server.setInterface(interface);
    return server.exec() ? 0 : 1;
}
