#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "agent_runtime/scheduler.h"

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
    ar::AgentScheduler scheduler;
    std::string error;
    std::vector<ar::AgentTaskSpec> tasks;
    tasks.push_back({.id = 1, .name = "root", .kind = "test"});
    tasks.push_back({.id = 2,
                     .name = "left",
                     .kind = "test",
                     .dependencies = {1}});
    tasks.push_back({.id = 3,
                     .name = "right",
                     .kind = "test",
                     .dependencies = {1}});
    tasks.push_back({.id = 4,
                     .name = "join",
                     .kind = "test",
                     .dependencies = {2, 3}});
    CHECK(scheduler.submit_batch(tasks, &error));

    ar::MemorySnapshot memory;
    memory.available_bytes = std::numeric_limits<std::uint64_t>::max();

    const auto root = scheduler.try_dispatch(memory);
    CHECK(root == 1);
    CHECK(scheduler.mark_running(*root));
    CHECK(scheduler.complete(*root));

    const auto first_branch = scheduler.try_dispatch(memory);
    const auto second_branch = scheduler.try_dispatch(memory);
    CHECK(first_branch.has_value() && second_branch.has_value());
    CHECK(*first_branch != *second_branch);
    CHECK((*first_branch == 2 || *first_branch == 3));
    CHECK((*second_branch == 2 || *second_branch == 3));
    CHECK(scheduler.mark_running(*first_branch));
    CHECK(scheduler.complete(*first_branch));

    CHECK(!scheduler.try_dispatch(memory).has_value());
    CHECK(scheduler.mark_running(*second_branch));
    CHECK(scheduler.complete(*second_branch));

    const auto join = scheduler.try_dispatch(memory);
    CHECK(join == 4);
    CHECK(scheduler.mark_running(*join));
    CHECK(scheduler.complete(*join));
    CHECK(scheduler.all_terminal());

    ar::AgentTaskSpec retry{.id = 5,
                            .name = "retry",
                            .kind = "test",
                            .retry_limit = 1};
    CHECK(scheduler.submit(retry));
    const auto attempt_one = scheduler.try_dispatch(memory);
    CHECK(attempt_one == 5);
    CHECK(scheduler.mark_running(5));
    CHECK(scheduler.fail(5, "expected failure"));
    const auto attempt_two = scheduler.try_dispatch(memory);
    CHECK(attempt_two == 5);
    CHECK(scheduler.mark_running(5));
    CHECK(scheduler.complete(5));

    const auto snapshot = scheduler.snapshot(5);
    CHECK(snapshot.has_value());
    CHECK(snapshot->retry_count == 1);

    ar::AgentScheduler dynamic;
    ar::AgentTaskSpec parent{.id = 20, .name = "parent", .kind = "test"};
    CHECK(dynamic.submit(parent));
    CHECK(dynamic.try_dispatch(memory) == 20);
    CHECK(dynamic.mark_running(20));
    ar::AgentTaskSpec child{.id = 21,
                            .name = "dynamic-child",
                            .kind = "test",
                            .dependencies = {20}};
    CHECK(dynamic.spawn(20, child));
    CHECK(!dynamic.try_dispatch(memory).has_value());
    CHECK(dynamic.complete(20));
    CHECK(dynamic.try_dispatch(memory) == 21);

    ar::AgentScheduler admission;
    ar::AgentTaskSpec memory_a{.id = 30, .name = "memory-a", .kind = "test"};
    ar::AgentTaskSpec memory_b{.id = 31, .name = "memory-b", .kind = "test"};
    memory_a.resources.memory_bytes = 800;
    memory_b.resources.memory_bytes = 800;
    CHECK(admission.submit_batch({memory_a, memory_b}));
    memory.available_bytes = 1000;
    const auto admitted = admission.try_dispatch(memory);
    CHECK(admitted.has_value());
    CHECK(!admission.try_dispatch(memory).has_value());
    CHECK(admission.mark_running(*admitted));
    CHECK(admission.complete(*admitted));
    CHECK(admission.try_dispatch(memory).has_value());

    ar::AgentScheduler cascade;
    CHECK(cascade.submit_batch({
        ar::AgentTaskSpec{.id = 40, .name = "fails", .kind = "test"},
        ar::AgentTaskSpec{.id = 41,
                          .name = "dependent",
                          .kind = "test",
                          .dependencies = {40}},
    }));
    CHECK(cascade.try_dispatch(memory) == 40);
    CHECK(cascade.mark_running(40));
    CHECK(cascade.fail(40, "failure"));
    CHECK(cascade.snapshot(41)->state == ar::AgentState::cancelled);
    CHECK(cascade.all_terminal());
    std::cout << "scheduler tests passed\n";
    return 0;
}
