#include <iostream>
#include <string>
#include <unordered_set>

#include "context_manager.h"

namespace art = agent_runtime_top;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << "check failed at line " << __LINE__ << ": "          \
                      << #condition << '\n';                                    \
            return 1;                                                           \
        }                                                                       \
    } while (false)

int main() {
    art::ContextManager contexts(2);
    std::string error;
    const auto first = contexts.acquire(10, 0, 0, &error);
    CHECK(first.has_value());
    CHECK(first->created);
    CHECK(first->sequence_id == 1);
    CHECK(contexts.finish(first->context_id, &error));

    const auto child =
        contexts.acquire(11, 10, first->context_id, &error);
    CHECK(child.has_value());
    CHECK(!child->created);
    CHECK(child->sequence_id == first->sequence_id);
    CHECK(contexts.owns(11, first->context_id));
    CHECK(contexts.finish(child->context_id, &error));

    CHECK(!contexts.acquire(12, 0, first->context_id, &error).has_value());
    CHECK(!contexts.release(
        12, 0, first->context_id, nullptr, &error));
    const auto second = contexts.acquire(12, 0, 0, &error);
    CHECK(second.has_value());
    CHECK(second->sequence_id == 2);
    CHECK(contexts.finish(second->context_id, &error));
    CHECK(!contexts.acquire(13, 0, 0, &error).has_value());

    int released_sequence = -1;
    CHECK(contexts.release(
        11, 0, first->context_id, &released_sequence, &error));
    CHECK(released_sequence == -1);
    CHECK(contexts.release(
        10, 0, first->context_id, &released_sequence, &error));
    CHECK(released_sequence == 1);

    const auto replacement = contexts.acquire(13, 0, 0, &error);
    CHECK(replacement.has_value());
    CHECK(replacement->sequence_id == 1);
    CHECK(contexts.finish(replacement->context_id, &error));
    const auto stats = contexts.stats();
    CHECK(stats.active_contexts == 2);
    CHECK(stats.created_contexts == 3);
    CHECK(stats.shared_acquisitions == 1);
    CHECK(stats.released_contexts == 1);

    art::ContextManager swapping_contexts(1, 3);
    std::unordered_set<art::ContextId> snapshots;
    art::ContextSwapCallbacks callbacks;
    callbacks.save =
        [&](art::ContextId context_id, int sequence, std::string *) {
            if (sequence != 1) return false;
            snapshots.insert(context_id);
            return true;
        };
    callbacks.load =
        [&](art::ContextId context_id, int sequence, std::string *) {
            if (sequence != 1) return false;
            return snapshots.erase(context_id) == 1;
        };
    const auto resident =
        swapping_contexts.acquire(20, 0, 0, &error, &callbacks);
    CHECK(resident.has_value());
    CHECK(!swapping_contexts.acquire(
               21, 0, 0, &error, &callbacks)
               .has_value());
    CHECK(error == "all resident contexts are pinned by active requests");
    CHECK(swapping_contexts.finish(resident->context_id, &error));
    const auto causes_swap =
        swapping_contexts.acquire(21, 0, 0, &error, &callbacks);
    CHECK(causes_swap.has_value());
    CHECK(snapshots.contains(resident->context_id));
    CHECK(swapping_contexts.finish(causes_swap->context_id, &error));
    const auto restored = swapping_contexts.acquire(
        20, 0, resident->context_id, &error, &callbacks);
    CHECK(restored.has_value());
    CHECK(restored->restored);
    CHECK(!snapshots.contains(resident->context_id));
    CHECK(snapshots.contains(causes_swap->context_id));
    CHECK(swapping_contexts.finish(restored->context_id, &error));
    const auto swap_stats = swapping_contexts.stats();
    CHECK(swap_stats.swap_outs == 2);
    CHECK(swap_stats.swap_ins == 1);
    CHECK(swap_stats.resident_contexts == 1);
    CHECK(swap_stats.swapped_contexts == 1);
    std::cout << "context manager tests passed\n";
    return 0;
}
