#include "agent_runtime/control_protocol.h"

#include <bit>
#include <limits>
#include <type_traits>

namespace agent_runtime {
namespace {

constexpr std::size_t kHeaderSize = 12;
constexpr std::uint32_t kMaxCollectionElements = 4096;
constexpr std::uint32_t kMaxStringSize = 256U * 1024U;

class Writer {
public:
    explicit Writer(std::vector<std::uint8_t> *output) : output_(output) {}

    void u8(std::uint8_t value) { output_->push_back(value); }

    void u16(std::uint16_t value) {
        for (unsigned shift = 0; shift < 16; shift += 8) {
            output_->push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            output_->push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            output_->push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    bool string(const std::string &value, std::string *error) {
        if (value.size() > kMaxStringSize) {
            if (error != nullptr) *error = "control string exceeds size limit";
            return false;
        }
        if (output_->size() > kMaxControlMessageSize - sizeof(std::uint32_t) ||
            value.size() > kMaxControlMessageSize - sizeof(std::uint32_t) -
                               output_->size()) {
            if (error != nullptr) *error = "control message exceeds size limit";
            return false;
        }
        u32(static_cast<std::uint32_t>(value.size()));
        output_->insert(output_->end(), value.begin(), value.end());
        return true;
    }

private:
    std::vector<std::uint8_t> *output_;
};

class Reader {
public:
    Reader(const std::uint8_t *data, std::size_t size)
        : data_(data), size_(size) {}

    bool u8(std::uint8_t *value) { return integer(value); }
    bool u16(std::uint16_t *value) { return integer(value); }
    bool u32(std::uint32_t *value) { return integer(value); }
    bool u64(std::uint64_t *value) { return integer(value); }

    bool string(std::string *value) {
        std::uint32_t length = 0;
        if (!u32(&length) || length > kMaxStringSize || remaining() < length) {
            return false;
        }
        value->assign(reinterpret_cast<const char *>(data_ + position_), length);
        position_ += length;
        return true;
    }

    std::size_t remaining() const { return size_ - position_; }

private:
    template <typename T>
    bool integer(T *value) {
        static_assert(std::is_unsigned_v<T>);
        if (remaining() < sizeof(T)) return false;
        T result = 0;
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            result |= static_cast<T>(data_[position_ + index]) << (index * 8U);
        }
        position_ += sizeof(T);
        *value = result;
        return true;
    }

    const std::uint8_t *data_;
    std::size_t size_;
    std::size_t position_{0};
};

bool encode_header(ControlOperation operation,
                   std::vector<std::uint8_t> *output) {
    Writer writer(output);
    writer.u32(kControlProtocolMagic);
    writer.u16(kControlProtocolVersion);
    writer.u16(static_cast<std::uint16_t>(operation));
    writer.u32(0);
    return true;
}

bool finish_message(std::vector<std::uint8_t> *output, std::string *error) {
    if (output->size() > kMaxControlMessageSize || output->size() < kHeaderSize) {
        if (error != nullptr) *error = "control message exceeds size limit";
        return false;
    }
    const std::uint32_t payload_size =
        static_cast<std::uint32_t>(output->size() - kHeaderSize);
    for (unsigned index = 0; index < 4; ++index) {
        (*output)[8 + index] =
            static_cast<std::uint8_t>(payload_size >> (index * 8U));
    }
    return true;
}

bool decode_header(Reader *reader,
                   ControlOperation *operation,
                   std::uint32_t *payload_size,
                   std::string *error) {
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t operation_value = 0;
    if (!reader->u32(&magic) || !reader->u16(&version) ||
        !reader->u16(&operation_value) || !reader->u32(payload_size)) {
        if (error != nullptr) *error = "truncated control header";
        return false;
    }
    if (magic != kControlProtocolMagic || version != kControlProtocolVersion) {
        if (error != nullptr) *error = "unsupported control protocol";
        return false;
    }
    if (operation_value < static_cast<std::uint16_t>(ControlOperation::ping) ||
        operation_value >
            static_cast<std::uint16_t>(ControlOperation::release_result)) {
        if (error != nullptr) *error = "unknown control operation";
        return false;
    }
    *operation = static_cast<ControlOperation>(operation_value);
    return true;
}

bool encode_task(const AgentTaskSpec &task, Writer *writer, std::string *error) {
    if (task.priority < std::numeric_limits<std::int32_t>::min() ||
        task.priority > std::numeric_limits<std::int32_t>::max() ||
        task.resources.timeout.count() < 0) {
        if (error != nullptr) *error = "task contains an invalid numeric value";
        return false;
    }
    writer->u64(task.id);
    writer->u64(task.parent_id);
    writer->u32(std::bit_cast<std::uint32_t>(
        static_cast<std::int32_t>(task.priority)));
    writer->u32(task.resources.cpu_threads);
    writer->u64(task.resources.memory_bytes);
    writer->u64(static_cast<std::uint64_t>(task.resources.timeout.count()));
    writer->u32(task.retry_limit);
    if (!writer->string(task.name, error) || !writer->string(task.kind, error)) {
        return false;
    }
    if (task.dependencies.size() > kMaxCollectionElements ||
        task.command.size() > kMaxCollectionElements) {
        if (error != nullptr) *error = "control collection exceeds size limit";
        return false;
    }
    writer->u32(static_cast<std::uint32_t>(task.dependencies.size()));
    for (const AgentId dependency : task.dependencies) writer->u64(dependency);
    writer->u32(static_cast<std::uint32_t>(task.command.size()));
    for (const auto &argument : task.command) {
        if (!writer->string(argument, error)) return false;
    }
    return true;
}

bool decode_task(Reader *reader, AgentTaskSpec *task) {
    std::uint32_t priority = 0;
    std::uint64_t timeout_ms = 0;
    if (!reader->u64(&task->id) || !reader->u64(&task->parent_id) ||
        !reader->u32(&priority) ||
        !reader->u32(&task->resources.cpu_threads) ||
        !reader->u64(&task->resources.memory_bytes) ||
        !reader->u64(&timeout_ms) || !reader->u32(&task->retry_limit) ||
        !reader->string(&task->name) || !reader->string(&task->kind)) {
        return false;
    }
    task->priority = std::bit_cast<std::int32_t>(priority);
    if (timeout_ms > static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    task->resources.timeout = std::chrono::milliseconds(timeout_ms);

    std::uint32_t count = 0;
    if (!reader->u32(&count) || count > kMaxCollectionElements) return false;
    task->dependencies.resize(count);
    for (auto &dependency : task->dependencies) {
        if (!reader->u64(&dependency)) return false;
    }
    if (!reader->u32(&count) || count > kMaxCollectionElements) return false;
    task->command.resize(count);
    for (auto &argument : task->command) {
        if (!reader->string(&argument)) return false;
    }
    return true;
}

bool encode_agent(const ControlAgentInfo &agent,
                  Writer *writer,
                  std::string *error) {
    writer->u64(agent.id);
    writer->u64(agent.parent_id);
    writer->u8(static_cast<std::uint8_t>(agent.state));
    writer->u64(std::bit_cast<std::uint64_t>(agent.process_id));
    writer->u32(agent.unresolved_dependencies);
    writer->u32(agent.retry_count);
    writer->u64(agent.context_id);
    return writer->string(agent.name, error) && writer->string(agent.kind, error) &&
           writer->string(agent.error, error);
}

bool decode_agent(Reader *reader, ControlAgentInfo *agent) {
    std::uint8_t state = 0;
    std::uint64_t process_id = 0;
    if (!reader->u64(&agent->id) || !reader->u64(&agent->parent_id) ||
        !reader->u8(&state) || !reader->u64(&process_id) ||
        !reader->u32(&agent->unresolved_dependencies) ||
        !reader->u32(&agent->retry_count) || !reader->u64(&agent->context_id) ||
        !reader->string(&agent->name) || !reader->string(&agent->kind) ||
        !reader->string(&agent->error)) {
        return false;
    }
    if (state > static_cast<std::uint8_t>(AgentState::cancelled)) return false;
    agent->state = static_cast<AgentState>(state);
    agent->process_id = std::bit_cast<std::int64_t>(process_id);
    return true;
}

void encode_reference(const SharedBufferRef &reference, Writer *writer) {
    writer->u64(reference.region_id);
    writer->u64(reference.region_size);
    writer->u64(reference.offset);
    writer->u64(reference.length);
    writer->u32(reference.data_type);
    writer->u32(reference.flags);
    writer->u64(reference.version);
}

bool decode_reference(Reader *reader, SharedBufferRef *reference) {
    return reader->u64(&reference->region_id) &&
           reader->u64(&reference->region_size) &&
           reader->u64(&reference->offset) &&
           reader->u64(&reference->length) &&
           reader->u32(&reference->data_type) &&
           reader->u32(&reference->flags) &&
           reader->u64(&reference->version) && reference->version == 1 &&
           reference->region_id != 0 && reference->region_size != 0 &&
           reference->offset <= reference->region_size &&
           reference->length <= reference->region_size - reference->offset;
}

}  // namespace

ControlAgentInfo make_control_info(const AgentSnapshot &snapshot) {
    return {.id = snapshot.spec.id,
            .parent_id = snapshot.spec.parent_id,
            .state = snapshot.state,
            .process_id = snapshot.process_id,
            .unresolved_dependencies = snapshot.unresolved_dependencies,
            .retry_count = snapshot.retry_count,
            .context_id = snapshot.context_id,
            .name = snapshot.spec.name,
            .kind = snapshot.spec.kind,
            .error = snapshot.error};
}

bool encode_control_request(const ControlRequest &request,
                            std::vector<std::uint8_t> *output,
                            std::string *error) {
    if (error != nullptr) error->clear();
    if (output == nullptr) {
        if (error != nullptr) *error = "control output pointer is null";
        return false;
    }
    output->clear();
    encode_header(request.operation, output);
    Writer writer(output);
    writer.u64(request.target_id);
    if (!encode_task(request.task, &writer, error)) return false;
    return finish_message(output, error);
}

bool decode_control_request(const std::uint8_t *data,
                            std::size_t size,
                            ControlRequest *request,
                            std::string *error) {
    if (error != nullptr) error->clear();
    if (data == nullptr || request == nullptr || size > kMaxControlMessageSize) {
        if (error != nullptr) *error = "invalid control request buffer";
        return false;
    }
    Reader reader(data, size);
    std::uint32_t payload_size = 0;
    if (!decode_header(&reader, &request->operation, &payload_size, error) ||
        payload_size != reader.remaining() || !reader.u64(&request->target_id) ||
        !decode_task(&reader, &request->task) || reader.remaining() != 0) {
        if (error != nullptr && error->empty()) *error = "invalid control request";
        return false;
    }
    return true;
}

bool encode_control_response(const ControlResponse &response,
                             std::vector<std::uint8_t> *output,
                             std::string *error) {
    if (error != nullptr) error->clear();
    if (output == nullptr || response.agents.size() > kMaxCollectionElements) {
        if (error != nullptr) *error = "invalid control response collection";
        return false;
    }
    output->clear();
    encode_header(ControlOperation::ping, output);
    Writer writer(output);
    writer.u8(response.success ? 1 : 0);
    if (!writer.string(response.message, error)) return false;
    writer.u32(static_cast<std::uint32_t>(response.agents.size()));
    for (const auto &agent : response.agents) {
        if (!encode_agent(agent, &writer, error)) return false;
    }
    writer.u8(response.result_available ? 1 : 0);
    if (response.result_available) encode_reference(response.result, &writer);
    return finish_message(output, error);
}

bool decode_control_response(const std::uint8_t *data,
                             std::size_t size,
                             ControlResponse *response,
                             std::string *error) {
    if (error != nullptr) error->clear();
    if (data == nullptr || response == nullptr || size > kMaxControlMessageSize) {
        if (error != nullptr) *error = "invalid control response buffer";
        return false;
    }
    Reader reader(data, size);
    ControlOperation ignored = ControlOperation::ping;
    std::uint32_t payload_size = 0;
    std::uint8_t success = 0;
    std::uint32_t count = 0;
    if (!decode_header(&reader, &ignored, &payload_size, error) ||
        payload_size != reader.remaining() || !reader.u8(&success) || success > 1 ||
        !reader.string(&response->message) || !reader.u32(&count) ||
        count > kMaxCollectionElements) {
        if (error != nullptr && error->empty()) *error = "invalid control response";
        return false;
    }
    response->success = success == 1;
    response->agents.resize(count);
    for (auto &agent : response->agents) {
        if (!decode_agent(&reader, &agent)) {
            if (error != nullptr) *error = "invalid agent response";
            return false;
        }
    }
    std::uint8_t result_available = 0;
    if (!reader.u8(&result_available) || result_available > 1) {
        if (error != nullptr) *error = "invalid result response";
        return false;
    }
    response->result_available = result_available == 1;
    response->result = {};
    if (response->result_available &&
        !decode_reference(&reader, &response->result)) {
        if (error != nullptr) *error = "invalid shared result reference";
        return false;
    }
    if (reader.remaining() != 0) {
        if (error != nullptr) *error = "trailing control response data";
        return false;
    }
    return true;
}

}  // namespace agent_runtime
