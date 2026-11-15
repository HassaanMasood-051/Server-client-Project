// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <signal.h>
#include <vector>
#include <sstream>
#include <sys/socket.h>
#include "helper.h"
#include <fstream>
#include <netinet/in.h>
#include <arpa/inet.h>
using namespace std;

// ---- Constants ----
#define BUFFER_SIZE 4096
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.0";
int LB_PORT = 5000;

// These two must be the same as in Phase 1

const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;

// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) ----
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader 
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

// ---- Structs only the client needs ----
struct TdbgReader
{
    ssize_t file_index;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep; 
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool receiveData(int sock, void *buffer, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t bytes = recv(sock, (char*)buffer + total, n - total, 0);
        if (bytes <= 0) {
            return false;
        }
        total += bytes;
    }
    return true;
}

bool sendData(int sock, const void *buffer, size_t n) {
    size_t total = 0;
    while (total < n) {
        ssize_t bytes = send(sock, (const char*)buffer + total, n - total, 0);
        if (bytes <= 0) {
            return false;
        }
        total += bytes;
    }
    return true;
}

int connectServer(const char *ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    } struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &serverAddr.sin_addr) <= 0) {
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        close(sock);
        return -1;
    }

    return sock;
}

bool sendSourceFile(int sock, const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (file == NULL) {
        return false;
    }

    char zero = 0;
    if (!sendData(sock, &zero, 1)) {
        fclose(file);
        return false;
    }

    uint32_t sum = 0;
    char buffer[BUFFER_SIZE];
    size_t bytesRead;
    while ((bytesRead = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
        sum = addToChecksum(sum, buffer, bytesRead);
        if (!sendData(sock, buffer, bytesRead)) {
            fclose(file);
            return false;
        }
    }

    if (!sendData(sock, &sum, 4)) {
        fclose(file);
        return false;
    }

    fclose(file);
    shutdown(sock, SHUT_WR);
    return true;
}

bool receiveResponse(int sock, const char *outputFile, string &errorMsg) {
    char status;
    if (!receiveData(sock, &status, 1)) {
        errorMsg = "Failed to receive status";
        return false;
    }

    FILE *outFile = NULL;
    if (status == 0) {
        outFile = fopen(outputFile, "wb");
        if (outFile == NULL) {
            errorMsg = "Cannot create output file";
            return false;
        }
    }

    char buffer[BUFFER_SIZE];
    ssize_t bytesRead;
    TrailerChecker tc;
    while ((bytesRead = recv(sock, buffer, BUFFER_SIZE, 0)) > 0) {
        string part = tc.feed(buffer, bytesRead);
        if (outFile != NULL) {
            fwrite(part.c_str(), 1, part.size(), outFile);
        } else if (status == 1) {
            errorMsg.append(part);
        }
    }

    if (outFile != NULL) {
        fclose(outFile);
    }

    if (bytesRead < 0) {
        errorMsg = "Error receiving data";
        if (outFile != NULL) {
            remove(outputFile);
        }
        return false;
    }

    if (!tc.finish()) {
        errorMsg = "response corrupted";
        if (outFile != NULL) {
            remove(outputFile);
        }
        return false;
    }

    if (status == 1) {
        return false;
    }

    return true;
}

bool requestDebugFile(const string &host, int port, const string &sourcePath, const char *outputFile, string &errorMsg)
{
    int sock = connectServer(host.c_str(), port);
    if (sock < 0) {
        errorMsg = "cannot connect to the load balancer";
        return false;
    }

    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (!sendSourceFile(sock, sourcePath.c_str())) {
        errorMsg = "cannot send the source file " + sourcePath;
        close(sock);
        return false;
    }

    bool ok = receiveResponse(sock, outputFile, errorMsg);
    close(sock);
    return ok;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================

bool readFully(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char*)buf + got, n - got);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

bool openTdbg(const char *path, TdbgReader &r)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }

    off_t fileSize = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    TTDBHeader h;
    if (!readFully(fd, h.magic, 4) || !readFully(fd, &h.version, 4) ||
        !readFully(fd, &h.stepCount, 4) || !readFully(fd, &h.indexOffset, 8)) {
        close(fd);
        return false;
    }

    if (h.magic[0] != 'T' || h.magic[1] != 'T' || h.magic[2] != 'D' || h.magic[3] != 'B') {
        close(fd);
        return false;
    }
    if (h.stepCount <= 0 || h.indexOffset < 20) {
        close(fd);
        return false;
    }
    if (h.indexOffset + (int64_t)h.stepCount * 8 > fileSize) {
        close(fd);
        return false;
    }

    vector<int64_t> idx(h.stepCount);
    lseek(fd, h.indexOffset, SEEK_SET);
    for (int i = 0; i < h.stepCount; i++) {
        if (!readFully(fd, &idx[i], 8)) {
            close(fd);
            return false;
        }
        if (idx[i] < 20 || idx[i] + (int64_t)sizeof(Snapshot) > h.indexOffset) {
            close(fd);
            return false;
        }
    }

    r.file_index = fd;
    r.header = h;
    r.index = idx;
    return true;
}
void closeTdbg(TdbgReader &r)
{
    // Free r.index and close the file.
    r.index.clear();
    if (r.file_index >= 0) {
        close(r.file_index);
    }
    r.file_index = -1;
}

string getText(const string *p)
{
    const char *b = (const char*)p;
    size_t len;
    memcpy(&len, b + 8, sizeof(size_t));
    if (len > 15) {
        return "?";
    }
    return string(b + 16, len);
}

Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
    if (step < 0 || step >= r.header.stepCount) {
        return nullptr;
    }

    char *raw = new char[sizeof(Snapshot)];
    lseek(r.file_index, r.index[step], SEEK_SET);
    if (!readFully(r.file_index, raw, sizeof(Snapshot))) {
        delete[] raw;
        return nullptr;
    }

    Snapshot *rs = (Snapshot*)raw;
    int32_t depth = rs->stackDepth;
    if (depth < 0 || depth > MAX_STACK_DEPTH) {
        delete[] raw;
        return nullptr;
    }

    Snapshot *s = new Snapshot();
    s->stackDepth = depth;

    for (int i = 0; i < depth; i++) {
        Frame &src = rs->callStack[i];
        Frame &dst = s->callStack[i];

        if (src.argc < 0 || src.argc > MAX_VARS_PER_FRAME || src.localCount < 0 || src.localCount > MAX_VARS_PER_FRAME) {
            delete[] raw;
            delete s;
            return nullptr;
        }

        dst.func_name = getText(&src.func_name);
        dst.argc = src.argc;
        dst.returnLine = src.returnLine;
        dst.localCount = src.localCount;

        for (int j = 0; j < src.argc; j++) {
            dst.argv[j].name = getText(&src.argv[j].name);
            dst.argv[j].value = src.argv[j].value;
        }
        for (int j = 0; j < src.localCount; j++) {
            dst.locals[j].name = getText(&src.locals[j].name);
            dst.locals[j].value = src.locals[j].value;
        }
    }

    delete[] raw;
    return s;
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
    s.active_flag = 0;
    s.reader.file_index = -1;
    s.currentStep = 0;
}
void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
    if (s.active_flag) {
        closeTdbg(s.reader);
    }
    s.active_flag = 0;
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
    Snapshot *snap = readSnapshotAt(s.reader, (int32_t)s.currentStep);
    if (snap == nullptr) {
        cout << "damaged .tdbg file" << endl;
        return false;
    }
    printSnapshot((int32_t)s.currentStep + 1, s.reader.header.stepCount, *snap);
    delete snap;
    return true;
}
bool moveBy(DebugSession &s, int32_t delta)
{
    if (!s.active_flag) {
        cout << "no session yet, use: start <source file>" << endl;
        return false;
    }

    int64_t target = s.currentStep + (int64_t)delta;
    if (target < 0) {
        cout << "already at the first step" << endl;
        return false;
    }
    if (target >= s.reader.header.stepCount) {
        cout << "already at the last step" << endl;
        return false;
    }

    int64_t old = s.currentStep;
    s.currentStep = target;
    if (!showCurrent(s)) {
        s.currentStep = old;
        return false;
    }
    return true;
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    string err;
    if (!requestDebugFile(LB_HOST, LB_PORT, sourcePath, TDBG_FILE, err)) {
        cout << err << endl;
        return false;
    }

    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    TdbgReader fresh;
    fresh.file_index = -1;
    if (!openTdbg(TDBG_FILE, fresh)) {
        cout << "server sent a damaged .tdbg file" << endl;
        return false;
    }

    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    if (s.active_flag) {
        closeTdbg(s.reader);
    }
    s.reader = fresh;
    s.currentStep = 0;
    s.active_flag = 1;

    // 5. Show step 1 with showCurrent(s).
    return showCurrent(s);
}
bool cmdNext(DebugSession &s) { return moveBy(s, 1); }
bool cmdPrv(DebugSession &s) { return moveBy(s, -1); }
bool cmdJmpNext(DebugSession &s, int32_t n) { return moveBy(s, n); }
bool cmdJmpPrv(DebugSession &s, int32_t n) { return moveBy(s, -n); }

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    cout << "Step " << step << " / " << totalSteps << endl;
    cout << "  Stack depth: " << s.stackDepth << endl;

    for (int i = 0; i < s.stackDepth; i++) {
        const Frame &f = s.callStack[i];
        cout << "  Frame " << i << ": " << f.func_name << endl;
        cout << "    returnLine: " << f.returnLine << endl;
        cout << "    args (" << f.argc << "):" << endl;
        for (int j = 0; j < f.argc; j++) {
            cout << "      " << f.argv[j].name << " = " << f.argv[j].value << endl;
        }
        cout << "    locals (" << f.localCount << "):" << endl;
        for (int j = 0; j < f.localCount; j++) {
            cout << "      " << f.locals[j].name << " = " << f.locals[j].value << endl;
        }
    }
    cout << endl;
}
void printHelp()
{
    // Print the commands
    cout << "commands:" << endl;
    cout << "  start <source file>     send the file and open the debug session" << endl;
    cout << "  next                    go one step forward" << endl;
    cout << "  prv                     go one step back" << endl;
    cout << "  jmp next <n>            go n steps forward" << endl;
    cout << "  jmp prv <n>             go n steps back" << endl;
    cout << "  help                    show this" << endl;
    cout << "  quit / exit             leave" << endl;
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool isNumber(const string &w)
{
    if (w.empty() || w.size() > 10) return false;
    for (int i = 0; i < w.size(); i++) {
        if (w[i] < '0' || w[i] > '9') return false;
    }
    return true;
}

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    vector<string> words;
    stringstream ss(line);
    string w;
    while (ss >> w) {
        words.push_back(w);
    }

    if (words.size() == 0) {
        return true;
    }
    // 2. If the first word is "ttdb", drop it.
    if (words[0] == PROGRAM_NAME) {
        words.erase(words.begin());
    }
    if (words.size() == 0) {
        return true;
    }

    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    if (words[0] == "start" && words.size() == 2) {
        return cmdStart(s, words[1]);
    }
    else if (words[0] == "next" && words.size() == 1) {
        return cmdNext(s);
    }
    else if (words[0] == "prv" && words.size() == 1) {
        return cmdPrv(s);
    }
    else if (words[0] == "jmp" && words.size() == 3 && (words[1] == "next" || words[1] == "prv")) {
        if (!isNumber(words[2])) {
            cout << "jmp needs a positive number" << endl;
            return false;
        }
        long long n = stoll(words[2]);
        if (n <= 0 || n > MAX_JUMP) {
            cout << "jmp needs a number between 1 and " << MAX_JUMP << endl;
            return false;
        }
        if (words[1] == "next") {
            return cmdJmpNext(s, (int32_t)n);
        }
        return cmdJmpPrv(s, (int32_t)n);
    }
    else if (words[0] == "help" && words.size() == 1) {
        printHelp();
        return true;
    }
    else if ((words[0] == "quit" || words[0] == "exit") && words.size() == 1) {
        quitRequested = true;
        return true;
    }

    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
    cout << "unknown command" << endl;
    printHelp();
    return false;
}
int main(int argc, char *argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    signal(SIGPIPE, SIG_IGN);

    if (argc >= 2) {
        LB_HOST = argv[1];
    }
    if (argc >= 3) {
        LB_PORT = atoi(argv[2]);
    }

    // DebugSession session; initSession(session);
    DebugSession session;
    initSession(session);

    // Print a short welcome and the help text.

    cout << "Time Travel Debugger" << endl;
    printHelp();

    //
    // Infinite loop:
    //   if quitRequested, break
    bool quitRequested = false;
    string line;
    while (true) {
        if (quitRequested) {
            break;
        }
        cout << PROMPT << flush;
        if (!getline(cin, line)) {
            break;
        }
        runCommand(session, line, quitRequested);
    }

    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.
    closeSession(session);
    return 0;
}