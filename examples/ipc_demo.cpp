#include <cstring>
#include <iostream>
#include <string>

#include "agent_runtime/shared_memory.h"

namespace ar = agent_runtime;

int main() {
    constexpr char payload[] =
        R"({"intent":"power_query","meter_id":"CN-001"})";
    std::string error;
    auto producer = ar::SharedMemoryRegion::create(sizeof(payload), &error);
    if (!producer.valid()) {
        std::cerr << error << '\n';
        return 1;
    }
    std::memcpy(producer.data(), payload, sizeof(payload));

    int channel[2] = {-1, -1};
    if (!ar::SharedMemoryChannel::create_pair(channel, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    ar::SharedBufferRef sent{.region_id = producer.region_id(),
                             .region_size = sizeof(payload),
                             .offset = 0,
                             .length = sizeof(payload),
                             .data_type = 1};
    if (!ar::SharedMemoryChannel::send(
            channel[0], sent, producer.descriptor(), &error)) {
        std::cerr << error << '\n';
        return 1;
    }

    ar::SharedBufferRef received;
    int received_descriptor = -1;
    if (!ar::SharedMemoryChannel::receive(
            channel[1], &received, &received_descriptor, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    auto consumer = ar::SharedMemoryRegion::map_existing(
        received_descriptor, received.region_size, received.region_id, &error);
    if (!consumer.valid()) {
        std::cerr << error << '\n';
        return 1;
    }
    const auto *text = static_cast<const char *>(consumer.data()) + received.offset;
    std::cout << text << '\n';
    ar::SharedMemoryChannel::close_descriptor(channel[0]);
    ar::SharedMemoryChannel::close_descriptor(channel[1]);
    return std::strcmp(text, payload) == 0 ? 0 : 1;
}
