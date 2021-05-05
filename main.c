#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <uv.h>

const ushort testListen4ClientPort = 5501;
const ushort testListen4ForwarderPort = 5502;
const ushort testServerPort = 5503;
const char*  testIpAddress = "127.0.0.1";

static char* signalingEstablished = "established";

#define connTypeToServer 0
#define connTypeToListener 1
#define connTypeToForwarder 0
#define connTypeToClient 1
typedef struct connInfo
{
    uint8_t type;
    uv_stream_t* peer;
} * ConnInfo;

static uv_tcp_t* newTcpWithInfo()
{
    uv_tcp_t* result = malloc(sizeof(uv_tcp_t));
    result->data=malloc(sizeof (struct connInfo));
    memset(result->data, 0, sizeof (struct connInfo));
    return result;
}

static void peerBind(uv_stream_t* peerA, uv_stream_t* peerB)
{
    ((ConnInfo)peerA->data)->peer = peerB;
    ((ConnInfo)peerB->data)->peer = peerA;
}

static void peerCloseFree(uv_stream_t* stream)
{
    uv_stream_t* peerHandle = ((ConnInfo)stream->data)->peer;
    if(peerHandle)
    {
        free(peerHandle->data);
        peerHandle->data = NULL;
        uv_close((uv_handle_t*)peerHandle, (uv_close_cb)free);
    }
    free(stream->data);
    stream->data = NULL;
    uv_close((uv_handle_t*)stream, (uv_close_cb)free);
}

static uv_loop_t* loop;

/*
 * socket peer table
 * Client -> Listener <- Forwarder -> Server
 *
 * Steps:
 * 1.Forwarder trying connect to Listener
 * 2.Listener open the port for Client
 * 3.Client trying connect to Listener
 * 4.Listener accept connect from Forwarder
 * 5.Forwarder connect to Server
 * 6.Server accept connection from Forwarder
 * 7.Forwarder send "established" to Listener
 * 8.Listener accept connection from Client
 * 9.Deliver the channel to Client and Server
 */

static void alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    buf->base = (char*)malloc(suggested_size);
    buf->len = suggested_size;
}

static void afterWrite(uv_write_t* req, int status)
{
    if(status != 0)
        peerCloseFree(req->handle);

    if(req->bufs)
        free(req->bufs->base);
    free(req);
}

static uv_tcp_t* listenToForwarder;
//static uv_tcp_t* waitingLineToforwarder;

void listenerRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
{
    if(nread <= 0)
    {
        peerCloseFree(stream);
        return;
    }

    if(((ConnInfo)stream->data)->peer==NULL)
    {
        //all listener connection connection should have a peer
        printf("why are you here?\n");
        free(buf->base);
        peerCloseFree(stream);
        return;
    }

    //step 9: deliver forwarder data to client
    //step 9: deliver client data to forwarder
    uv_buf_t newbuf[1];
    newbuf[0] = uv_buf_init(buf->base, nread);
    uv_write(
                (uv_write_t*) malloc(sizeof(uv_write_t)),
                ((ConnInfo)stream->data)->peer,
                newbuf,
                1,
                afterWrite
                );

}

void listenerConnection(uv_stream_t* listenHandle, int status)
{
    if(status != 0)
        return;

    uv_tcp_t* connForwarder;
    uv_tcp_t* connClient;
    switch (((ConnInfo)listenHandle->data)->type) {
    case connTypeToClient:
        connClient = newTcpWithInfo();
        ((ConnInfo)connClient->data)->type = connTypeToClient;
        uv_tcp_init(uv_default_loop(), connClient);
        uv_accept(listenHandle, (uv_stream_t*)connClient);

        connForwarder = newTcpWithInfo();
        ((ConnInfo)connForwarder->data)->type = connTypeToForwarder;
        uv_tcp_init(uv_default_loop(), connForwarder);
        if(uv_accept((uv_stream_t*)listenToForwarder, (uv_stream_t*)connForwarder)!=0)
        {
            peerCloseFree((uv_stream_t*)connClient);
            peerCloseFree((uv_stream_t*)connForwarder);
            return;
        }

        peerBind((uv_stream_t*)connClient, (uv_stream_t*)connForwarder);

        uv_buf_t newbuf[1];
        newbuf[0].base = malloc(strlen(signalingEstablished)+1);
        strcpy(newbuf[0].base, signalingEstablished);
        newbuf[0].len = strlen(signalingEstablished);
        uv_write(
                    (uv_write_t*) malloc(sizeof(uv_write_t)),
                    (uv_stream_t*)connForwarder,
                    newbuf,
                    1,
                    afterWrite
                    );

        uv_read_start((uv_stream_t*)connForwarder, alloc_cb, listenerRead);
        uv_read_start((uv_stream_t*)connClient, alloc_cb, listenerRead);
        break;
    case connTypeToForwarder:
    default:
        break;
    }
}

int listener()
{
    listenToForwarder = newTcpWithInfo();
    ((ConnInfo)listenToForwarder->data)->type = connTypeToForwarder;

    uv_tcp_init(loop, listenToForwarder);
    struct sockaddr_in forwarderAddr;
    uv_ip4_addr(testIpAddress, testListen4ForwarderPort, &forwarderAddr);
    uv_tcp_bind(listenToForwarder, (const struct sockaddr*)&forwarderAddr, 0);
    uv_listen((uv_stream_t*)listenToForwarder, SOMAXCONN, listenerConnection);


    uv_tcp_t* listenToClient = newTcpWithInfo();
    ((ConnInfo)listenToClient->data)->type = connTypeToClient;

    uv_tcp_init(loop, listenToClient);
    struct sockaddr_in clientAddr;
    uv_ip4_addr(testIpAddress, testListen4ClientPort, &clientAddr);
    uv_tcp_bind(listenToClient, (const struct sockaddr*)&clientAddr, 0);
    uv_listen((uv_stream_t*)listenToClient, SOMAXCONN, listenerConnection);


    return 0;
}


void forwarderConnect(uv_connect_t* req, int status);
int forwarder();
void forwarderRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
{
    if(nread <= 0)
    {
        peerCloseFree(stream);
        return;
    }

    //step 5
    if(((ConnInfo)stream->data)->peer==NULL)
    {
        if(((ConnInfo)stream->data)->type==connTypeToListener)
        {
            printf("recv string should be \"%s\": %.*s\n", signalingEstablished, (int)nread, buf->base);

            //Establish a new connection to the Server
            uv_tcp_t* tcpHandle = newTcpWithInfo();
            ((ConnInfo)tcpHandle->data)->type=connTypeToServer;
            peerBind((uv_stream_t*)tcpHandle, stream);

            uv_tcp_init(loop, tcpHandle);

            struct sockaddr_in addr;
            uv_ip4_addr(testIpAddress, testServerPort, &addr);

            int r;
            if ((r = uv_tcp_connect(
                     (uv_connect_t*)malloc(sizeof(uv_connect_t)),
                     tcpHandle, (const struct sockaddr *)&addr,
                     forwarderConnect)
                 ) != 0)
            {
                peerCloseFree(stream);
                printf("failed to connect to server, err: %s\n", uv_strerror(r));
            }

            //Establish a new connection to the Listener
            forwarder();

            free(buf->base);
            return;
        }
        else
            printf("why are you here??? server connection without listener peer.\n");
    }

    //step 9: deliver listener data to server
    //step 9: deliver server data to listener
    uv_buf_t newbuf[1];
    newbuf[0] = uv_buf_init(buf->base, nread);
    uv_write(
                (uv_write_t*) malloc(sizeof(uv_write_t)),
                ((ConnInfo)stream->data)->peer,
                newbuf,
                1,
                afterWrite
                );
}

void forwarderConnect(uv_connect_t* req, int status)
{
    if(status != 0)
        peerCloseFree(req->handle);
    else
    {
        uv_tcp_keepalive((uv_tcp_t*)req->handle, 1, 15);
        uv_read_start(req->handle, alloc_cb, forwarderRead);
    }
    free(req);
}

int forwarder()
{
    uv_tcp_t* tcpHandle = newTcpWithInfo();
    ((ConnInfo)tcpHandle->data)->type=connTypeToListener;
    uv_tcp_init(loop, tcpHandle);

    struct sockaddr_in addr;
    uv_ip4_addr(testIpAddress, testListen4ForwarderPort, &addr);

    int r;
    if ((r = uv_tcp_connect(
             (uv_connect_t*)malloc(sizeof(uv_connect_t)),
             tcpHandle, (const struct sockaddr *)&addr,
             forwarderConnect)
         ) != 0)
    {
        printf("failed to connect to server, err: %s\n", uv_strerror(r));
        return 2;
    }

    return 0;
}

int main(int argc, char* argv[])
{
    loop = uv_default_loop();

    int optind;
    for (optind = 1; optind < argc && argv[optind][0] == '-'; optind++) {
        if(strcmp(argv[optind], "-l")==0)
            listener();
        else if(strcmp(argv[optind], "-f")==0)
            forwarder();
//        else if(strcmp(argv[optind], "-fa"))
//            ;
//        else if(strcmp(argv[optind], "-sa"))
//            ;
        else
        {
//            fprintf(stderr, "Usage: %s [-ilw] [file...]\n", argv[0]);
//            exit(EXIT_FAILURE);
        }
    }
    return uv_run(loop, UV_RUN_DEFAULT);
}
