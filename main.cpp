#include "NW.h"

class Server : public NW
{
public:
    Server()
    {}

    virtual void handleConnection(Connection *conn)
    {


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
