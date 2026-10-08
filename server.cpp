#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <sstream>
#include <iomanip>
using namespace std;

const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2;
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024;
const int32_t IO_BUFFER_SIZE = 64 * 1024;
const int32_t SOCKET_TIMEOUT_SEC = 5;

template <typename T>
class Stack {
    struct Node {
        T data;
        Node *next;

        public:
        Node () {
            this->next = nullptr;
        }

        Node (T val) {
            this->next = nullptr;
            this->data = val;
        } 
    };

    Node *top;
    int32_t count;

public:
    Stack() {
        this->top = nullptr;
        this->count = 0;
    }

    ~Stack () {
        for (int i = 0; i < this->count; i++) {
            Node* temp = this->top;
            this->top = this->top->next;
            delete temp;
        }
    }

    void push(const T &val) {
        Node* temp = new Node (val);
        if (this->count == 0) {
            this->top = temp;
            this->count ++;
            return;
        }
        temp->next = this->top;
        this->top = temp;
        this->count++;
    }

    T pop() {
        if (this->count == 0) {
            throw "Stack is empty.\n";
        } else if (this->count == 1) {
            T val = this->top->data;
            delete top;
            this->top = nullptr;
            this->count --;
            return val;
        }

        Node* temp = this->top;
        T val = this->top->data;
        this->top = this->top->next;
        this->count --;
        delete temp;
        return val;
    }
    
    T &peek() {
        if (this->isEmpty()) {
            throw "Stack is Empty.\n";
        }
        return this->top->data;
    }

    bool isEmpty() {
        return (this->count == 0);
    }

    int32_t depth() {
        return this->count;
    }

    int32_t snapshot_into(T out[], int32_t maxLen) {
        int len {};
        if (maxLen < this->count) {
            len = maxLen;
        } else {
            len = this->count;
        }

        Node* temp = this->top;
        for (int i = 0; i < len; i++) {
            out[i] = temp->data;
            temp = temp->next;
        } return len;
    }
};

struct Snapshot;

struct TimelineNode {
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};

class Timeline {
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    Timeline() {
        this->head = nullptr;
        this->tail = nullptr;
        this->stepCount = 0;
    }

    void record(Snapshot *s) {
        TimelineNode* temp = new TimelineNode();
        temp->data = s;
        temp->next = nullptr;
        temp->prev = this->tail;

        if (this->stepCount == 0) {
            this->head = temp;
            this->tail = temp;
            this->stepCount ++;
            return;
        } 

        this->tail->next = temp;
        this->tail = temp;
        this->stepCount ++;
    }
    
    TimelineNode *begin() {
        return this->head;
    }

    int32_t getStepCount() {
        return this->stepCount;
    }
};

struct Variable {
    string name;
    int32_t value;
};

struct Frame {
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};

struct Snapshot {
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
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

struct FuncEntry {
    string funcName;
    int64_t byteOffsetInResolveBin;

    FuncEntry () {
        this->funcName = "NULL";
        this->byteOffsetInResolveBin = 0x0;
    }

    FuncEntry (string _funcName, int64_t __byte) {
        this->funcName = _funcName;
        this->byteOffsetInResolveBin = __byte;
    }
};

bool validateProgram(const char *sourcePath) {
    ifstream read (sourcePath);
    if (!read) {
        cout << "Err:: Opening File.\n";
        return false;
    }

    vector <string> words;
    words.push_back ("");
    int num_of_words {};

    string line = "";
    while (getline (read,line)) {
        for (int i = 0; i < line.size(); i++) {
            if (line[i] == ' ' || line[i] == '\t') {
                if (words[num_of_words] != "") {
                    words.push_back("");
                    num_of_words++;
                }
            } else {
                words[num_of_words].push_back(line[i]);
            } 
        } line = "";

        if (words[num_of_words] != "") {
            words.push_back("");
            num_of_words++;
        }
    }

    Stack <int> stk;
    for (int i = 0; i < words.size(); i++) {
        if (words[i] == "func") {
            if (!stk.isEmpty() && stk.peek() == 1) {
                return false;
            } else {
                stk.push(1);
            }
        } else if (words[i] == "func_end") {
            if (!stk.isEmpty() && stk.peek() == 1) {
                stk.pop();
            } else {
                return false;
            }
        }
    } 
    
    if (stk.isEmpty()) {
        return true;
    } else {
        return false;
    }
}

int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath) {
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    
    ifstream read (sourcePath);
    if (!read) {
        cout << "Err:: Opening file.\n";
        return -1;
    } 
    vector <string> lines;
    string tempray;

    while (getline(read, tempray)) {
        if (tempray.find_first_not_of(" \t\r") != string::npos) {
            lines.push_back(tempray);
        }
    } 
    read.close();

    vector <int64_t> offsets;
    int64_t start = 0x0;
    for (int i = 0; i < lines.size(); i++) {
        vector <string> words (1);
        int num_of_words {};
        for (int j = 0; j < lines[i].size(); j++) {
            if (lines[i][j] == ' ' || lines[i][j] == '\t' || lines[i][j] == '\r') {
                if (!words[num_of_words].empty()) {
                    words.push_back("");
                    num_of_words++;
                }
            } else {
                words[num_of_words].push_back(lines[i][j]);
            }
        } if (words[num_of_words].empty()) {
            words.pop_back();
        }

        string temp = "";
        for (int ct = 0; ct < words.size(); ct++) {
            if (words[ct] == "call" && (ct + 1) < words.size()) {
                temp += "call 0x00000000";
                ct++;
            } else {
                temp += words[ct];
            }
            if (ct + 1 < words.size()) {
                temp.push_back(' ');
            }
        }
        offsets.push_back (start);
        start = start + temp.size() + 8 + 4;
    }

    for (int i = 0; i < lines.size(); i++) {
        vector <string> words (1);
        int num_of_words {};
        for (int j = 0; j < lines[i].size(); j++) {
            if (lines[i][j] == ' ' || lines[i][j] == '\t' || lines[i][j] == '\r') {
                if (!words[num_of_words].empty()) {
                    words.push_back("");
                    num_of_words++;
                }
            } else {
                words[num_of_words].push_back(lines[i][j]);
            }
        } if (words[num_of_words].empty()) {
            words.pop_back();
        } if (words.empty()) {
            continue;
        }

        for (int x = 0; x < words.size(); x++) {
            // FIX: only the first word can be the "func" keyword
            if (x == 0 && words[x] == "func" && x+1 != words.size()) {
                // FIX: bounds check so funcArray can't overflow
                if (funcCount >= MAX_FUNCS) {
                    cout << "Err:: Too many functions.\n";
                    return -1;
                }
                FuncEntry temp (words[x+1],offsets[i]); 
                funcArray [funcCount ++] = temp;
            }
        }
    }

    vector <string> final_lines;
    for (int i = 0; i < lines.size(); i++) {
        vector <string> words (1);
        int num_of_words {};
        for (int j = 0; j < lines[i].size(); j++) {
            if (lines[i][j] == ' ' || lines[i][j] == '\t' || lines[i][j] == '\r') {
                if (!words[num_of_words].empty()) {
                    words.push_back("");
                    num_of_words++;
                }
            } else {
                words[num_of_words].push_back(lines[i][j]);
            }
        } if (words[num_of_words].empty()) {
            words.pop_back();
        } if (words.empty()) {
            continue;
        }

        string toPush = "";
        for (int ct = 0; ct < words.size(); ct++) {
            if (words[ct] == "call" && (ct + 1) < words.size()) {
                toPush += "call ";
                string targetName = words[ct + 1];
                int64_t targetOffset = -1;
                for (int f = 0; f < funcCount; f++) {
                    if (funcArray[f].funcName == targetName) {
                        targetOffset = funcArray[f].byteOffsetInResolveBin;
                        break;
                    }
                }
                // FIX: call to an undefined function is an error
                if (targetOffset == -1) {
                    cout << "Err:: Call to undefined function '" << targetName << "'.\n";
                    return -1;
                }
                stringstream ss;
                ss << "0x" << hex << setw(8) << setfill('0') << targetOffset;
                toPush += ss.str();

                ct++;
            } else {
                toPush += words[ct];
            }

            if (ct + 1 < words.size()) {
                toPush.push_back(' ');
            }
        } 
        final_lines.push_back(toPush);
    }

    ofstream write (resolveBinPath);
    if (!write) {
        cout << "Err:: Opening File.\n";
        return -1;
    }

    for (int i = 0; i < final_lines.size(); i++) {
        write << "[0x" << hex << offsets[i] << dec << "] ["
            << final_lines[i].size() << "] \"" << final_lines[i] << "\"" << endl;
    }
    write.close();

    for (int i = 0; i < funcCount; i++) {
        if (funcArray[i].funcName == "main") {
            return funcArray[i].byteOffsetInResolveBin;
        }
    } cout << "Unknown Function.\n";

    return -1;
}

bool convertTextToBinaryResolve(const char *textPath, const char *binaryPath) {
    ifstream read (textPath);
    ofstream write (binaryPath, ios::binary);
    if (!read) {
        cout << "Err:: Opening file.\n";
        return false;
    } if (!write) {
        cout << "Err:: Opening file.\n";
        return false;
    }

    int64_t offset{};
    int32_t size{};
    char temp{};
    string code;

    while (read >> temp) {
        read >> hex >> offset;
        read >> temp;

        read >> temp;
        read >> dec >> size;
        read >> temp;

        read >> ws;
        getline(read, code);
        code.erase(0, 1);
        code.pop_back();

        size = code.size();

        write.write((char*)&offset, sizeof(int64_t));
        write.write((char*)&size, sizeof(int32_t));
        write.write(code.c_str(), size);
    }

    write.close();
    return true;
}

enum TokenType {
    KEYWORD,
    IDENTIFIER,
    PARAM
};

struct Token {
    TokenType type;
    string text;

    Token (TokenType _type, string _text) {
        this->type = _type;
        this->text = _text;
    }
};

int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens) {
    vector <string> words (1);
    int num_of_words {};

    for (int i = 0; i < line.size(); i++) {
        if (line[i] == ' ' || line[i] == '\t' || line[i] == '\r') {
            if (!words[num_of_words].empty()) {
                words.push_back("");
                num_of_words++;
            }
        } else {
            words[num_of_words].push_back(line[i]);
        }
    } if (words[num_of_words].empty()) {
        words.pop_back();
    } else {
        num_of_words++;
    }

    int ct {};
    for (int i = 0; i < num_of_words && i < maxTokens; i++) {
        if (i == 0) {
            Token tkn (KEYWORD,words[i]);
            tokens[ct++] = tkn; 
        } else if (i == 1) {
            Token tkn (IDENTIFIER,words[i]);
            tokens[ct++] = tkn; 
        } else {
            Token tkn (PARAM,words[i]);
            tokens[ct++] = tkn; 
        }
    } return ct;
}

Snapshot *buildSnapshot(Stack <Frame> &callStack) {
    Snapshot* temp = new Snapshot ();
    temp->stackDepth = callStack.snapshot_into (temp->callStack, MAX_STACK_DEPTH);
    return temp;
}

void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline) {
    ifstream read (resolveBinPath, ios::binary);

}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.txt");
    if (mainOffset == -1) {
        return 1;
    }
    if (!convertTextToBinaryResolve("resolve.txt", "resolve.bin")) {
        return 1;
    }

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}