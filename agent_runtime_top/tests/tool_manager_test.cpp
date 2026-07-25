#include <iostream>
#include <string>

#include "tool_manager.h"

namespace ar = agent_runtime;
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
    art::ToolManager tools(false);
    ar::InvocationRequest request;
    request.agent_id = 1;
    request.kind = ar::InvocationKind::tool;
    request.target = "echo";
    request.operation = "execute";
    request.payload = "hello";
    auto result = tools.execute(request, {}, [] { return false; });
    CHECK(result.status == ar::InvocationStatus::ok);
    CHECK(result.output == "hello");

    request.target = "missing";
    result = tools.execute(request, {}, [] { return false; });
    CHECK(result.status == ar::InvocationStatus::rejected);

    request.target = "echo";
    result = tools.execute(request, {}, [] { return true; });
    CHECK(result.status == ar::InvocationStatus::cancelled);

    request.target = "concat";
    request.operation = "execute";
    request.payload = "left:";
    result = tools.execute(request, {}, [] { return false; });
    CHECK(result.status == ar::InvocationStatus::ok);
    CHECK(result.output == "left:");

    request.operation = "unsupported";
    result = tools.execute(request, {}, [] { return false; });
    CHECK(result.status == ar::InvocationStatus::rejected);

    request.operation = "execute";
    request.payload.assign(1024U * 1024U + 1U, 'x');
    result = tools.execute(request, {}, [] { return false; });
    CHECK(result.status == ar::InvocationStatus::rejected);

    const auto names = tools.list();
    CHECK(names.size() == 2);
    CHECK(names[0] == "concat");
    CHECK(names[1] == "echo");
    std::cout << "tool manager tests passed\n";
    return 0;
}
