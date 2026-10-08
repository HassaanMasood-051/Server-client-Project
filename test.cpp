#include <iostream>
#include <fstream>
using namespace std;

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

int main () {
    convertTextToBinaryResolve("resolve.txt", "resolve.bin");
}