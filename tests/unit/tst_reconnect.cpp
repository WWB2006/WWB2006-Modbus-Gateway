#include "gateway/device/ReconnectPolicy.h"
#include <iostream>
#include <cassert>

using namespace gateway::device;

int main() {
    std::cout << "[TEST] Running tst_reconnect..." << std::endl;

    ReconnectPolicy policy(1000, 16000, 5);

    // Attempt 0: ~1000 to 1250
    int d0 = policy.nextDelayMs();
    std::cout << "  Attempt 0 delay: " << d0 << "ms" << std::endl;
    assert(d0 >= 1000 && d0 <= 1250);

    // Attempt 1: ~2000 to 2500
    int d1 = policy.nextDelayMs();
    std::cout << "  Attempt 1 delay: " << d1 << "ms" << std::endl;
    assert(d1 >= 2000 && d1 <= 2500);

    // Attempt 2: ~4000 to 5000
    int d2 = policy.nextDelayMs();
    std::cout << "  Attempt 2 delay: " << d2 << "ms" << std::endl;
    assert(d2 >= 4000 && d2 <= 5000);

    // Attempt 3: ~8000 to 10000
    int d3 = policy.nextDelayMs();
    std::cout << "  Attempt 3 delay: " << d3 << "ms" << std::endl;
    assert(d3 >= 8000 && d3 <= 10000);

    // Attempt 4: capped by maxDelay 16000 (+ jitter up to 4000)
    int d4 = policy.nextDelayMs();
    std::cout << "  Attempt 4 delay: " << d4 << "ms" << std::endl;
    assert(d4 >= 16000 && d4 <= 20000);

    // Attempt 5: exceeded maxRetries (5) -> returns -1
    int d5 = policy.nextDelayMs();
    std::cout << "  Attempt 5 delay: " << d5 << "ms (expected -1)" << std::endl;
    assert(d5 == -1);
    assert(policy.hasExceededMaxRetries());

    // Reset test
    policy.reset();
    assert(policy.currentAttempt() == 0);
    assert(!policy.hasExceededMaxRetries());
    int dReset = policy.nextDelayMs();
    assert(dReset >= 1000 && dReset <= 1250);

    std::cout << "[PASS] tst_reconnect passed all tests!" << std::endl;
    return 0;
}
