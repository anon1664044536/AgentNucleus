#include <cstring>
#include <iostream>
#include <string>
#include <utility>

#include "agent_runtime/shared_memory.h"

namespace ar = agent_runtime;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << "check failed at line " << __LINE__ << ": "          \
                      << #condition << '\n';                                    \
            return 1;                                                           \
        }                                                                       \
    } while (false)

int main() {
    constexpr char payload[] = "agent-runtime-shared-memory";
    std::string error;
    auto producer = ar::SharedMemoryRegion::create(4096, &error);
    CHECK(producer.valid());
    std::memcpy(producer.data(), payload, sizeof(payload));

    int sockets[2] = {-1, -1};
    CHECK(ar::SharedMemoryChannel::create_pair(sockets, &error));
    ar::SharedBufferRef sent{.region_id = producer.region_id(),
                             .region_size = 4096,
                             .offset = 0,
                             .length = 4096};
    CHECK(ar::SharedMemoryChannel::send(
        sockets[0], sent, producer.descriptor(), &error));

    ar::SharedBufferRef received;
    int descriptor = -1;
    CHECK(ar::SharedMemoryChannel::receive(
        sockets[1], &received, &descriptor, &error));
    auto consumer =
        ar::SharedMemoryRegion::map_existing(
            descriptor, received.region_size, received.region_id, &error);
    CHECK(consumer.valid());
    CHECK(std::strcmp(static_cast<const char *>(consumer.data()), payload) == 0);
    ar::SharedMemoryChannel::close_descriptor(sockets[0]);
    ar::SharedMemoryChannel::close_descriptor(sockets[1]);

    auto notifier = ar::EventNotifier::create(&error);
    CHECK(notifier.valid());
    CHECK(notifier.signal(3, &error));
    std::uint64_t observed = 0;
    CHECK(notifier.wait(100, &observed, &error));
    CHECK(observed == 3);

    ar::AgentResultStore store;
    auto stored_region = ar::SharedMemoryRegion::create(64, &error);
    CHECK(stored_region.valid());
    constexpr char result_payload[] = "immutable-agent-result";
    std::memcpy(stored_region.data(), result_payload, sizeof(result_payload));
    CHECK(store.publish(7,
                        std::move(stored_region),
                        sizeof(result_payload) - 1,
                        ar::SharedDataType::text_utf8,
                        ar::shared_buffer_stdout_stderr,
                        &error));
    auto handle = store.acquire(7, &error);
    CHECK(handle.has_value());
    CHECK((handle->reference.flags & ar::shared_buffer_immutable) != 0);
    auto stored_consumer = ar::SharedMemoryRegion::map_existing_read_only(
        handle->release_descriptor(),
        handle->reference.region_size,
        handle->reference.region_id,
        &error);
    CHECK(stored_consumer.valid());
    CHECK(std::memcmp(stored_consumer.data(),
                      result_payload,
                      handle->reference.length) == 0);
    CHECK(store.erase(7));
    CHECK(!store.acquire(7, &error).has_value());

    std::cout << "shared memory tests passed\n";
    return 0;
}
