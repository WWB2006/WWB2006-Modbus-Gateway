#include "gateway/transport/TransportInterface.h"

namespace gateway::transport {

const char* toString(TransportKind kind) noexcept {
    switch (kind) {
        case TransportKind::Tcp:
            return "tcp";
        case TransportKind::Serial:
            return "rtu";
    }
    return "unknown";
}

const char* toString(TransportState state) noexcept {
    switch (state) {
        case TransportState::Closed:
            return "closed";
        case TransportState::Connecting:
            return "connecting";
        case TransportState::Open:
            return "open";
        case TransportState::Error:
            return "error";
    }
    return "unknown";
}

}  // namespace gateway::transport
