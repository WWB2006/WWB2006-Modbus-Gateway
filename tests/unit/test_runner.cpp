#include <iostream>
#include <cstdlib>
#include <vector>
#include <string>

struct TestCase {
    std::string name;
    std::string cmd;
};

int main(int argc, char* argv[]) {
    std::cout << "========================================================\n"
              << "       Modbus Gateway Unit Test Suite Runner            \n"
              << "========================================================\n";

    std::vector<std::string> tests = {
        "tst_crc.exe",
        "tst_codec.exe",
        "tst_reconnect.exe",
        "tst_storage.exe",
        "tst_scheduler.exe"
    };

    int passed = 0;
    int failed = 0;

    for (const auto& t : tests) {
        std::cout << "\n>>> Running test: " << t << " ...\n";
        int rc = std::system(t.c_str());
        if (rc == 0) {
            std::cout << ">>> [PASSED] " << t << "\n";
            passed++;
        } else {
            std::cerr << ">>> [FAILED] " << t << " (exit code " << rc << ")\n";
            failed++;
        }
    }

    std::cout << "\n========================================================\n"
              << " Test Summary: " << passed << " passed, " << failed << " failed\n"
              << "========================================================\n";

    return (failed == 0) ? 0 : 1;
}
