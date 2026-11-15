// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code). 
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection 


#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <sys/types.h>
#include "helper.h"
using namespace std;


// ---- Constants ----
const char *SERVER_PATH = "./server";      // the program we start when the server is down
const int IO_BUFFER_SIZE = 64 * 1024;      // fixed buffer for moving bytes
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
int lbPort = 5000;
string serverHost = "127.0.0.0";
int serverPort = 5001;
const int CLIENT_TIMEOUT_SEC = 30;


// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *front, *rear;
    int count;


public:
    // Implement these functions:
    Queue()
    {

    front = NULL;
    rear = NULL;
    count = 0;
    }
    ~Queue()
    {

    while (front != NULL) {
        Node *temp = front;
        front = front->next;
        delete temp;
    }
    }
    void enqueue(const T &val)
    {

    Node *n = new Node;
    n->data = val;
    n->next = NULL;
    if (rear == NULL) {
        front = n;
        rear = n;
    } else {
        rear->next = n;
        rear = n;
    }
    count++;
    }
    T dequeue()
    {

    if (front == NULL) {
        return T();
    }
    Node *temp = front;
    T val = temp->data;
    front = front->next;
    if (front == NULL) {
        rear = NULL;
    }
    delete temp;
    count--;
    return val;
    }
    bool isEmpty()
    {
        
    return count == 0;
    }
    int size()
    {

    return count;
    }
};

struct ClientInfo
{
    int sock;           // the connected socket
    string address;     // the client's IP as text, e.g. "127.0.0.1"
    int port;           // the client's port
    time_t arrivedAt;   // when accept() returned
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;


// ---- Basic socket helpers ----
bool sendAll(int sock, const char *buf, size_t n) 
{
    // Reply format: [status: 1 byte][sending_material][checksum: 4 bytes].
    // send() may send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = send(sock, buf + sent, n - sent, MSG_NOSIGNAL);
        if (r <= 0) {
            return false;
        }
        sent += r;
    }
    return true;
}
bool sendError(int sock, const string &message)
{
    // Reply format: [status: 1 byte][payload until close][checksum: 4 bytes].
    // For an error: send one byte with value 1, then the message text. 
    char one = 1;
    if (!sendAll(sock, &one, 1)) {
        return false;
    }
    if (!sendAll(sock, message.c_str(), message.size())) {
        return false;
    }
    uint32_t sum = addToChecksum(0, message.c_str(), message.size());
    return sendAll(sock, (char*)&sum, 4);
}
int createListenSocket(int port)
{ 
    // Create a TCP socket with SOCK_CLOEXEC(flag).
    // set SO_REUSEADDR so we can restart the load balancer right away.
    // bind() to the port on any address, then listen(LISTEN_BACKLOG).
    // If bind or listen fails, print the error and exit, because without this socket we cannot work.
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) {
        perror("socket");
        exit(1);
    }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    if (listen(s, LISTEN_BACKLOG) < 0) {
        perror("listen");
        exit(1);
    }
    return s;
}
int connectToServer(const char *host, int port)
{
    // Create a TCP socket (SOCK_CLOEXEC) and connect it to host:port.
    // Return the socket, or -1 if the connection fails 
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) {
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        close(s);
        return -1;
    }
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================


bool startServer(int port)
{
    // Build the command text: SERVER_PATH + " " + port as text + "&"
    //   Example: "./server 5001 &"
    // Call system() with it.
    // Return true if system() returned 0, otherwise false.
    string cmd = string(SERVER_PATH) + " " + to_string(port) + " &";
    int r = system(cmd.c_str());
    if (r == 0) {
        return true;
    }
    return false;
}
int connectToServerWithWakeup()
{
    // 1. Try connectToServer(). If it works, return that socket. The server is alive.
    // 2. Otherwise call startServer(). If it fails, return -1.
    // 3. Try connectToServer() up to STARTUP_MAX_ATTEMPTS times 
    // 4. Return the first working socket, or -1 if the server never came up.
    int s = connectToServer(serverHost.c_str(), serverPort);
    if (s != -1) {
        return s;
    }
    if (!startServer(serverPort)) {
        return -1;
    }
    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; i++) {
        usleep(STARTUP_POLL_INTERVAL_MS * 1000);
        s = connectToServer(serverHost.c_str(), serverPort);
        if (s != -1) {
            return s;
        }
    }
    return -1;
}


// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================


int forwardStream(int from, int to)
{
    char *buf = new char[IO_BUFFER_SIZE];
    ssize_t n = recv(from, buf, 1, 0);
    if (n <= 0) {
        delete[] buf;
        return 2;
    }
    if (!sendAll(to, buf, 1)) {
        delete[] buf;
        return 2;
    }

    TrailerChecker tc;
    while ((n = recv(from, buf, IO_BUFFER_SIZE, 0)) > 0) {
        string out = tc.feed(buf, n);
        if (out.size() > 0) {
            if (!sendAll(to, out.c_str(), out.size())) {
                delete[] buf;
                return 2;
            }
        }
    }
    delete[] buf;
    if (n < 0) {
        return 2;
    }
    if (!tc.finish()) {
        return 1;
    }
    if (!sendAll(to, tc.held.c_str(), 4)) {
        return 2;
    }
    return 0;
}

void relayOneClient(ClientInfo &c)
{
    // 1. serverSock = connectToServerWithWakeup().
    //    If it is -1: sendError "backend server failed to start", close clientSock, return.
    // 3. Client -> server: repeat recv() from clientSock into a buffer of IO_BUFFER_SIZE and sendAll() it to serverSock.
    //    Stop when recv() returns 0 (the client finished sending) or fails.
    // 4. Call shutdown(serverSock, SHUT_WR), so the server knows the string is complete.
    // 5. Server -> client: repeat recv() from serverSock and sendAll() to clientSock.
    //    Stop when recv() returns 0 (the server sent its whole reply and closed) or fails.
    // 6. Close both sockets.
    int serverSock = connectToServerWithWakeup();
    if (serverSock == -1) {
        sendError(c.sock, "backend server failed to start");
        close(c.sock);
        return;
    }

    int up = forwardStream(c.sock, serverSock);
    shutdown(serverSock, SHUT_WR);
    if (up == 1) {
        sendError(c.sock, "upload checksum mismatch");
        close(serverSock);
        close(c.sock);
        return;
    }
    if (up == 2) {
        close(serverSock);
        close(c.sock);
        return;
    }

    forwardStream(serverSock, c.sock);

    close(serverSock);
    close(c.sock);
}


// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================


void acceptLoop(int listenSock)
{
    // Repeat: accept() a new client. 
    //  If pendingClients.size() is already MAX_PENDING_CLIENTS: unlock, sendError "server busy, try again later",
    // else
    // Lock queueMutex.
    //   close the client, and go on.
    //   Otherwise enqueue the client socket.
    // Never wait for the server here, so new clients are always accepted quickly.
    // If accept() fails, print the error and keep going (do not end the thread).
    while (flag) {
        struct sockaddr_in caddr;
        socklen_t len = sizeof(caddr);
        int cs = accept4(listenSock, (struct sockaddr*)&caddr, &len, SOCK_CLOEXEC);
        if (cs < 0) {
            perror("accept");
            continue;
        }

        ClientInfo c;
        c.sock = cs;
        char ipText[64];
        inet_ntop(AF_INET, &caddr.sin_addr, ipText, sizeof(ipText));
        c.address = ipText;
        c.port = ntohs(caddr.sin_port);
        c.arrivedAt = time(NULL);

        queueMutex.lock();
        if (pendingClients.size() >= MAX_PENDING_CLIENTS) {
            queueMutex.unlock();
            sendError(cs, "server busy, try again later");
            close(cs);
        } else {
            pendingClients.enqueue(c);
            queueMutex.unlock();
        }
    }
}
void dispatcherLoop()
{
    // This runs in the main thread and never ends.
    // Repeat:
    //   Lock queueMutex, take ONE client out of the queue if there is one, unlock.
    //   If we got a client: call relayOneClient(). The next client is taken out of the queue only after this one is finished.
    while (flag) {
        ClientInfo c;
        bool got = false;

        queueMutex.lock();
        if (!pendingClients.isEmpty()) {
            c = pendingClients.dequeue();
            got = true;
        }
        queueMutex.unlock();

        if (got) {
            if (time(NULL) - c.arrivedAt > CLIENT_TIMEOUT_SEC) {
                close(c.sock);
            } else {
                relayOneClient(c);
            }
        } else {
            usleep(20 * 1000);
        }
    }
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}
int main(int argc, char *argv[])
{

    signal(SIGINT, handler); // whenever ctrl+c will be pressed to exit the program the flag wll be set so insead of using infinite loop you can loop on flag to safely close things and exit
    // 2. listenSock = createListenSocket(LB_PORT).
    // 3. Start a thread that runs acceptLoop(listenSock) and detach it.
    // 4. Call dispatcherLoop().
    signal(SIGPIPE, SIG_IGN);
    if (argc >= 2) {
        serverHost = argv[1];
    }
    if (argc >= 3) {
        serverPort = atoi(argv[2]);
    }

    int listenSock = createListenSocket(lbPort);
    thread t(acceptLoop, listenSock);
    t.detach();
    dispatcherLoop();

    close(listenSock);
    return 0;
}