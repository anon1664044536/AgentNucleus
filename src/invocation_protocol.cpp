#include "agent_runtime/invocation_protocol.h"

#include <bit>
#include <limits>
#include <type_traits>

namespace agent_runtime {
namespace {

constexpr std::size_t kHeaderSize = 12;
constexpr std::uint16_t kRequestMessage = 1;
constexpr std::uint16_t kResponseMessage = 2;
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
        if (value.size() > kMaxStringSize ||
            output_->size() >
                kMaxInvocationMessageSize - sizeof(std::uint32_t) ||
            value.size() >
                kMaxInvocationMessageSize - sizeof(std::uint32_t) -
                    output_->size()) {
            if (error != nullptr) *error = "invocation string exceeds size limit";
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
        std::uint32_t size = 0;
        if (!u32(&size) || size > kMaxStringSize || remaining() < size) {
            return false;
        }
        value->assign(
            reinterpret_cast<const char *>(data_ + position_), size);
        position_ += size;
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
            result |= static_cast<T>(data_[position_ + index])
                      << (index * 8U);
        }
        position_ += sizeof(T);
        *value = result;
        return true;
    }

    const std::uint8_t *data_;
    std::size_t size_;
    std::size_t position_{0};
};

void encode_header(std::uint16_t message_type,
                   std::vector<std::uint8_t> *output) {
    Writer writer(output);
    writer.u32(kInvocationProtocolMagic);
    writer.u16(kInvocationProtocolVersion);
    writer.u16(message_type);
    writer.u32(0);
}

bool finish_message(std::vector<std::uint8_t> *output, std::string *error) {
    if (output->size() < kHeaderSize ||
        output->size() > kMaxInvocationMessageSize) {
        if (error != nullptr) *error = "invocation message exceeds size limit";
        return false;
    }
    const auto payload_size =
        static_cast<std::uint32_t>(output->size() - kHeaderSize);
    for (unsigned index = 0; index < 4; ++index) {
        (*output)[8 + index] =
            static_cast<std::uint8_t>(payload_size >> (index * 8U));
    }
    return true;
}

bool decode_header(Reader *reader,
                   std::uint16_t expected_type,
                   std::uint32_t *payload_size,
                   std::string *error) {
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t type = 0;
    if (!reader->u32(&magic) || !reader->u16(&version) ||
        !reader->u16(&type) || !reader->u32(payload_size)) {
        if (error != nullptr) *error = "truncated invocation header";
        return false;
    }
    if (magic != kInvocationProtocolMagic ||
        version != kInvocationProtocolVersion || type != expected_type) {
        if (error != nullptr) *error = "unsupported invocation protocol";
        return false;
    }
    return true;
}

bool valid_reference(const SharedBufferRef &reference) {
    return reference.version == 1 && reference.region_id != 0 &&
           reference.region_size != 0 &&
           reference.offset <= reference.region_size &&
           reference.length <= reference.region_size - reference.offset &&
           reference.data_type <=
               static_cast<std::uint32_t>(SharedDataType::json_utf8);
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
           reader->u64(&reference->version) && valid_reference(*reference);
}

void encode_metrics(const InvocationMetrics &metrics, Writer *writer) {
    writer->u64(metrics.queue_time_us);
    writer->u64(metrics.execution_time_us);
    writer->u64(metrics.input_tokens);
    writer->u64(metrics.output_tokens);
    writer->u64(metrics.reused_tokens);
    writer->u64(metrics.cache_bytes);
    writer->u8(metrics.cache_hit ? 1 : 0);
}

bool decode_metrics(Reader *reader, InvocationMetrics *metrics) {
    std::uint8_t cache_hit = 0;
    if (!reader->u64(&metrics->queue_time_us) ||
        !reader->u64(&metrics->execution_time_us) ||
        !reader->u64(&metrics->input_tokens) ||
        !reader->u64(&metrics->output_tokens) ||
        !reader->u64(&metrics->reused_tokens) ||
        !reader->u64(&metrics->cache_bytes) ||
        !reader->u8(&cache_hit) || cache_hit > 1) {
        return false;
    }
    metrics->cache_hit = cache_hit == 1;
    return true;
}

}  // namespace

bool encode_invocation_request(const InvocationRequest &request,
                               std::vector<std::uint8_t> *output,
                               std::string *error) {
    if (error != nullptr) error->clear();
    if (output == nullptr || request.agent_id == kInvalidAgentId ||
        request.target.empty() || request.operation.empty() ||
        request.inputs.size() > kMaxInvocationInputs ||
        request.resources.timeout.count() < 0) {
        if (error != nullptr) *error = "invalid invocation request";
        return false;
    }
    if (request.kind != InvocationKind::model &&
        request.kind != InvocationKind::tool) {
        if (error != nullptr) *error = "invalid invocation kind";
        return false;
    }
    for (const auto &input : request.inputs) {
        if (input.producer_id == kInvalidAgentId ||
            !valid_reference(input.reference)) {
            if (error != nullptr) *error = "invalid invocation input";
            return false;
        }
    }

    output->clear();
    encode_header(kRequestMessage, output);
    Writer writer(output);
    writer.u64(request.agent_id);
    writer.u64(request.parent_id);
    writer.u8(static_cast<std::uint8_t>(request.kind));
    writer.u64(request.context_id);
    writer.u32(std::bit_cast<std::uint32_t>(
        static_cast<std::int32_t>(request.priority)));
    writer.u32(request.resources.cpu_threads);
    writer.u64(request.resources.memory_bytes);
    writer.u64(
        static_cast<std::uint64_t>(request.resources.timeout.count()));
    if (!writer.string(request.target, error) ||
        !writer.string(request.operation, error) ||
        !writer.string(request.payload, error)) {
        return false;
    }
    writer.u32(static_cast<std::uint32_t>(request.inputs.size()));
    for (const auto &input : request.inputs) {
        writer.u64(input.producer_id);
        encode_reference(input.reference, &writer);
    }
    return finish_message(output, error);
}

bool decode_invocation_request(const std::uint8_t *data,
                               std::size_t size,
                               InvocationRequest *request,
                               std::string *error) {
    if (error != nullptr) error->clear();
    if (data == nullptr || request == nullptr ||
        size > kMaxInvocationMessageSize) {
        if (error != nullptr) *error = "invalid invocation request buffer";
        return false;
    }
    Reader reader(data, size);
    std::uint32_t payload_size = 0;
    std::uint8_t kind = 0;
    std::uint32_t priority = 0;
    std::uint64_t timeout_ms = 0;
    std::uint32_t input_count = 0;
    if (!decode_header(
            &reader, kRequestMessage, &payload_size, error) ||
        payload_size != reader.remaining() ||
        !reader.u64(&request->agent_id) ||
        !reader.u64(&request->parent_id) || !reader.u8(&kind) ||
        !reader.u64(&request->context_id) || !reader.u32(&priority) ||
        !reader.u32(&request->resources.cpu_threads) ||
        !reader.u64(&request->resources.memory_bytes) ||
        !reader.u64(&timeout_ms) || !reader.string(&request->target) ||
        !reader.string(&request->operation) ||
        !reader.string(&request->payload) || !reader.u32(&input_count) ||
        input_count > kMaxInvocationInputs) {
        if (error != nullptr && error->empty()) {
            *error = "invalid invocation request";
        }
        return false;
    }
    if (kind < static_cast<std::uint8_t>(InvocationKind::model) ||
        kind > static_cast<std::uint8_t>(InvocationKind::tool) ||
        timeout_ms > static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max()) ||
        request->agent_id == kInvalidAgentId || request->target.empty() ||
        request->operation.empty()) {
        if (error != nullptr) *error = "invalid invocation request fields";
        return false;
    }
    request->kind = static_cast<InvocationKind>(kind);
    request->priority =
        std::bit_cast<std::int32_t>(priority);
    request->resources.timeout = std::chrono::milliseconds(timeout_ms);
    request->inputs.resize(input_count);
    for (auto &input : request->inputs) {
        if (!reader.u64(&input.producer_id) ||
            input.producer_id == kInvalidAgentId ||
            !decode_reference(&reader, &input.reference)) {
            if (error != nullptr) *error = "invalid invocation input";
            return false;
        }
    }
    if (reader.remaining() != 0) {
        if (error != nullptr) *error = "trailing invocation request data";
        return false;
    }
    return true;
}

bool encode_invocation_response(const InvocationResponse &response,
                                std::vector<std::uint8_t> *output,
                                std::string *error) {
    if (error != nullptr) error->clear();
    if (output == nullptr || response.agent_id == kInvalidAgentId ||
        static_cast<std::uint8_t>(response.status) >
            static_cast<std::uint8_t>(InvocationStatus::busy) ||
        (response.output_available && !valid_reference(response.output))) {
        if (error != nullptr) *error = "invalid invocation response";
        return false;
    }
    output->clear();
    encode_header(kResponseMessage, output);
    Writer writer(output);
    writer.u64(response.agent_id);
    writer.u64(std::bit_cast<std::uint64_t>(response.process_id));
    writer.u8(static_cast<std::uint8_t>(response.status));
    writer.u64(response.context_id);
    if (!writer.string(response.message, error)) return false;
    encode_metrics(response.metrics, &writer);
    writer.u8(response.output_available ? 1 : 0);
    if (response.output_available) {
        encode_reference(response.output, &writer);
    }
    return finish_message(output, error);
}

bool decode_invocation_response(const std::uint8_t *data,
                                std::size_t size,
                                InvocationResponse *response,
                                std::string *error) {
    if (error != nullptr) error->clear();
    if (data == nullptr || response == nullptr ||
        size > kMaxInvocationMessageSize) {
        if (error != nullptr) *error = "invalid invocation response buffer";
        return false;
    }
    Reader reader(data, size);
    std::uint32_t payload_size = 0;
    std::uint8_t status = 0;
    std::uint8_t output_available = 0;
    std::uint64_t process_id = 0;
    if (!decode_header(
            &reader, kResponseMessage, &payload_size, error) ||
        payload_size != reader.remaining() ||
        !reader.u64(&response->agent_id) || !reader.u64(&process_id) ||
        !reader.u8(&status) ||
        status > static_cast<std::uint8_t>(InvocationStatus::busy) ||
        !reader.u64(&response->context_id) ||
        !reader.string(&response->message) ||
        !decode_metrics(&reader, &response->metrics) ||
        !reader.u8(&output_available) || output_available > 1) {
        if (error != nullptr && error->empty()) {
            *error = "invalid invocation response";
        }
        return false;
    }
    response->status = static_cast<InvocationStatus>(status);
    response->process_id = std::bit_cast<std::int64_t>(process_id);
    response->output_available = output_available == 1;
    response->output = {};
    if (response->output_available &&
        !decode_reference(&reader, &response->output)) {
        if (error != nullptr) *error = "invalid invocation output";
        return false;
    }
    if (response->agent_id == kInvalidAgentId || reader.remaining() != 0) {
        if (error != nullptr) *error = "invalid invocation response fields";
        return false;
    }
    return true;
}

}  // namespace agent_runtime
