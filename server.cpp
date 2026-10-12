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
#include <cinttypes>
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

void writeHeader(FILE *f, const TTDBHeader &h) {
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
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
    
    FILE *read = fopen(sourcePath, "r");
    if (!read) {
        cout << "Err:: Opening file.\n";
        return -1;
    } 
    vector <string> lines;
    string tempray;

    while (true) {
        tempray.clear();
        int ch = fgetc(read);
        if (ch == EOF) {
            break;
        }
        while (ch != EOF && ch != '\n') {
            tempray.push_back((char)ch);
            ch = fgetc(read);
        }
        if (tempray.find_first_not_of(" \t\r") != string::npos) {
            lines.push_back(tempray);
        }
    } 
    fclose(read);

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
            if (x == 0 && words[x] == "func" && x+1 != words.size()) {
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

    FILE *write = fopen(resolveBinPath, "w");
    if (!write) {
        cout << "Err:: Opening File.\n";
        return -1;
    }

    for (int i = 0; i < final_lines.size(); i++) {
        fprintf(write, "[0x%" PRIx64 "] [%zu] \"%s\"\n",
                (uint64_t)offsets[i], final_lines[i].size(), final_lines[i].c_str());
    }
    fclose(write);

    for (int i = 0; i < funcCount; i++) {
        if (funcArray[i].funcName == "main") {
            return funcArray[i].byteOffsetInResolveBin;
        }
    } cout << "Unknown Function.\n";

    return -1;
}

bool convertTextToBinaryResolve(const char *textPath, const char *binaryPath) {
    FILE *read = fopen(textPath, "r");
    FILE *write = fopen(binaryPath, "wb");
    if (!read) {
        cout << "Err:: Opening file.\n";
        if (write) fclose(write);
        return false;
    } if (!write) {
        cout << "Err:: Opening file.\n";
        fclose(read);
        return false;
    }

    int64_t offset{};
    int32_t size{};
    char temp{};
    string code;

    while (fscanf(read, " %c", &temp) == 1) {
        fscanf(read, "%" SCNx64, (uint64_t*)&offset);
        fscanf(read, " %c", &temp);

        fscanf(read, " %c", &temp);
        fscanf(read, "%" SCNd32, &size);
        fscanf(read, " %c", &temp);

        fscanf(read, " ");

        code.clear();
        int ch = fgetc(read);
        while (ch != EOF && ch != '\n') {
            code.push_back((char)ch);
            ch = fgetc(read);
        }
        code.erase(0, 1);
        code.pop_back();

        size = code.size();

        fwrite(&offset, sizeof(int64_t), 1, write);
        fwrite(&size, sizeof(int32_t), 1, write);
        fwrite(code.c_str(), 1, size, write);
    }

    fclose(write);
    fclose(read);
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

    Token () {
        this->type = KEYWORD;
        this->text = "";
    }

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
    FILE *read = fopen(resolveBinPath, "rb");
    if (!read) {
        cout << "Err:: Opening file.\n";
        return;
    }
    vector <int64_t> offsets;
    vector <string> lines;
    int64_t offset {};
    int32_t size {};

    while (fread(&offset, sizeof(int64_t), 1, read) == 1) {
        fread(&size, sizeof(int32_t), 1, read);

        string code (size, ' ');
        fread(&code[0], 1, size, read);

        offsets.push_back(offset);
        lines.push_back(code);
    } fclose(read);

    int ip = 0;
    for (int i = 0; i < offsets.size(); i++) {
        if (offsets[i] == mainOffset) {
            ip = i;
            break;
        }
    }

    Stack <Frame> callStack;
    Frame mainFrame;
    mainFrame.returnLine = -1;
    mainFrame.argc = 0;
    mainFrame.localCount = 0;
    mainFrame.func_name = "main";
    callStack.push(mainFrame);

    ip++;

    while (!callStack.isEmpty()) {
        if (ip >= lines.size()) {
            cout << "Err:: Program just went past the last line.\n";
            break;
        }
 
        Token tokens[MAX_TOKENS];
        int n = tokenizeLine(lines[ip], tokens, MAX_TOKENS);
 
        if (tokens[0].text == "set") {
            Frame &frm = callStack.peek();
 
            int where = 0;
            int idx = -1;
            for (int i = 0; i < frm.argc; i++) {
                if (frm.argv[i].name == tokens[1].text) {
                    where = 1;
                    idx = i;
                }
            } for (int i = 0; i < frm.localCount; i++) {
                if (frm.locals[i].name == tokens[1].text) {
                    where = 2;
                    idx = i;
                }
            }
 
            if (where == 0) {
                frm.locals[frm.localCount].name = tokens[1].text;
                where = 2;
                idx = frm.localCount;
                frm.localCount++;
            }
 
            int32_t val = 0;
            bool found = false;
            for (int i = 0; i < frm.argc; i++) {
                if (frm.argv[i].name == tokens[2].text) {
                    val = frm.argv[i].value;
                    found = true;
                }
            } for (int i = 0; i < frm.localCount; i++) {
                if (frm.locals[i].name == tokens[2].text) {
                    val = frm.locals[i].value;
                    found = true;
                }
            } if (found == false) {
                val = stoi(tokens[2].text);
            } if (where == 1) {
                frm.argv[idx].value = val;
            } else {
                frm.locals[idx].value = val;
            }
            ip++;
        }
        else if (tokens[0].text == "add" || tokens[0].text == "sub" || tokens[0].text == "mul" || tokens[0].text == "div") {
            Frame &frme = callStack.peek();
 
            int where = 0;
            int idx = -2;
            for (int i = 0; i < frme.argc; i++) {
                if (frme.argv[i].name == tokens[1].text) {
                    where = 1;
                    idx = i;
                }
            } for (int i = 0; i < frme.localCount; i++) {
                if (frme.locals[i].name == tokens[1].text) {
                    where = 2;
                    idx = i;
                }
            }

            if (where == 0) {
                cout << "Err:: Variable with the name of '" << tokens[1].text << "' is not defined.\n";
                break;
            }
 
            int32_t first {};
            if (where == 1) {
                first = frme.argv[idx].value;
            } else {
                first = frme.locals[idx].value;
            }
 
            int32_t second {};
            bool found = false;
            for (int i = 0; i < frme.argc; i++) {
                if (frme.argv[i].name == tokens[2].text) {
                    second = frme.argv[i].value;
                    found = true;
                }
            } for (int i = 0; i < frme.localCount; i++) {
                if (frme.locals[i].name == tokens[2].text) {
                    second = frme.locals[i].value;
                    found = true;
                }
            }
            if (found == false) {
                second = stoi(tokens[2].text);
            }
 
            int32_t result = 0;
            if (tokens[0].text == "add") {
                result = first+ second;
            } else if (tokens[0].text == "sub") {
                result = first -second;
            } else if (tokens[0].text == "mul") {
                result = first*second;
            } else {
                if (second == 0) {
                    cout << "Err:: You can't divde by zero.\n";
                    break;
                }
                result = first/second;
            }
 
            if (where == 1) {
                frme.argv[idx].value = result;
            } else {
                frme.locals[idx].value = result;
            }
            ip++;
        }
        else if (tokens[0].text == "call") {
            if (callStack.depth() >= MAX_STACK_DEPTH) { // I added this fix after searching from google ToT mx stack depth is 64
                cout << "Err:: Memory overflow.\n";
                break;
            }

            int64_t target = stoll(tokens[1].text, nullptr, 16); // Searched this syntax from Online and what i learned was "int32_t is normal int but int64_t is long long in windows and macOS but in linux its long so i decided to write stoi but then asked Chatgpt what to use stoi or stoll it said stoll was better i didnt do more search on it"
            int indx = -1;
            for (int i = 0; i < offsets.size(); i++) {
                if (offsets[i] == target) {
                    indx = i;
                    break;
                }
            }
            if (indx == -1) {
                cout << "Err:: Such funtion never existed.\n";
                break;
            }

            Token tkns[MAX_TOKENS];
            int fn = tokenizeLine(lines[indx], tkns, MAX_TOKENS);

            if (n != fn) { 
                cout << "Err:: Wrong number of Arguments for \"" << tkns[1].text << "\".\n";
                break;
            }

            Frame &prev = callStack.peek();

            Frame nw_Frm;
            nw_Frm.func_name = tkns[1].text;
            nw_Frm.argc = fn - 2; // 1+1 (1 = func, 1 = functions' Name so param's r num_of_words - 2)
            nw_Frm.localCount = 0;
            nw_Frm.returnLine = ip + 1;

            for (int x = 0; x < nw_Frm.argc; x++) {
                nw_Frm.argv[x].name = tkns[x+2].text;
                nw_Frm.argv[x].value = 0;

                bool found = false;
                for (int i = 0; i < prev.argc; i++) {
                    if (prev.argv[i].name == tokens[x+2].text) {
                        nw_Frm.argv[x].value = prev.argv[i].value;
                        found = true;
                    }
                } for (int i = 0; i <prev.localCount; i++) {
                    if (prev.locals[i].name == tokens[x+2].text) {
                        nw_Frm.argv[x].value = prev.locals[i].value;
                        found = true;
                    }
                }
                if (found == false) {
                    nw_Frm.argv[x].value = stoi(tokens[x+2].text);
                }
            }
            callStack.push(nw_Frm);
            ip = indx + 1;
        }
        else if (tokens[0].text == "func_end") {
            Frame done = callStack.pop();
            if (!callStack.isEmpty()) {
                Token callTokens[MAX_TOKENS];
                int cn = tokenizeLine(lines[done.returnLine - 1], callTokens, MAX_TOKENS);
 
                Frame &caller = callStack.peek();
                for (int x = 0; x < done.argc; x++) {
                    if (x+2 < cn) {
                        for (int i = 0; i < caller.argc; i++) {
                            if (caller.argv[i].name == callTokens[x+2].text) {
                                caller.argv[i].value = done.argv[x].value;
                            }
                        } for (int i = 0; i < caller.localCount; i++) {
                            if (caller.locals[i].name == callTokens[x+2].text) {
                                caller.locals[i].value = done.argv[x].value;
                            }
                        }
                    }
                } ip = done.returnLine;
            }
        }
        else {
            ip++;
        }
        timeline.record(buildSnapshot(callStack));
    }
}

void writeTdbg(Timeline &timeline, const char *tdbgPath) {
    FILE *f = fopen(tdbgPath, "wb");
    if (!f) {
        cout << "Err:: Cant open tdbg file for Some reason.\n";
        return;
    } int32_t steps = timeline.getStepCount();

    TTDBHeader temp;
    temp.magic[0] ='T';
    temp.magic[1] = 'T';
    temp.magic[2] = 'D';
    temp.magic[3] ='B';
    temp.version = 1;
    temp.stepCount = steps;
    temp.indexOffset = 0; 
    writeHeader(f,temp);
    int64_t *arr = new int64_t[steps];
    int ct = 0;

    TimelineNode *node = timeline.begin();
    while (node != nullptr) {
        arr[ct] = ftell(f);        
        fwrite(node->data, sizeof(Snapshot), 1, f);
        node = node->next;
        ct++;
    }

    int64_t idxOffset = ftell(f);
    fwrite(arr, sizeof(int64_t), steps, f);
    rewind(f);
    temp.indexOffset = idxOffset;
    writeHeader(f, temp);
    fclose(f);
    delete[] arr;
}

int32_t main() {

    if (!validateProgram("source.bin")) {
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
    cout << "Steps: " << timeline.getStepCount() << endl;

    writeTdbg(timeline, "session.tdbg");

    return 0;
}