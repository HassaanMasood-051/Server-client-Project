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

int main () {
    resolveProgram ("source.bin","resolve.txt");
    convertTextToBinaryResolve("resolve.txt","resolve.bin");
}