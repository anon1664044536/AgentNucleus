#include "context_manager.h"

#include <algorithm>
#include <limits>

namespace agent_runtime_top {

ContextManager::ContextManager(std::size_t max_resident_contexts,
                               std::size_t max_logical_contexts)
    : max_resident_contexts_(
          std::max<std::size_t>(1, max_resident_contexts)),
      max_logical_contexts_(
          std::max(max_resident_contexts_,
                   max_logical_contexts == 0
                       ? max_resident_contexts_
                       : max_logical_contexts)) {
    free_sequences_.reserve(max_resident_contexts_);
    for (std::size_t sequence = max_resident_contexts_;
         sequence > 0;
         --sequence) {
        free_sequences_.push_back(static_cast<int>(sequence));
    }
}

std::optional<ContextId> ContextManager::find_for_agent_locked(
    AgentId agent_id) const {
    for (const auto &[context_id, entry] : contexts_) {
        if (entry.owners.contains(agent_id)) return context_id;
    }
    return std::nullopt;
}

bool ContextManager::authorized_locked(const Entry &entry,
                                       AgentId agent_id,
                                       AgentId parent_id) const {
    return entry.owners.contains(agent_id) ||
           (parent_id != 0 && entry.owners.contains(parent_id));
}

std::optional<int> ContextManager::obtain_sequence_locked(
    ContextId excluded_context,
    const ContextSwapCallbacks *swap,
    std::string *error) {
    if (!free_sequences_.empty()) {
        const int sequence = free_sequences_.back();
        free_sequences_.pop_back();
        return sequence;
    }
    if (swap == nullptr || !swap->save) {
        if (error != nullptr) *error = "all model contexts are in use";
        return std::nullopt;
    }

    auto candidate = contexts_.end();
    for (auto current = contexts_.begin(); current != contexts_.end();
         ++current) {
        const Entry &entry = current->second;
        if (!entry.resident || entry.pin_count != 0 ||
            entry.id == excluded_context) {
            continue;
        }
        if (candidate == contexts_.end() ||
            entry.last_access < candidate->second.last_access) {
            candidate = current;
        }
    }
    if (candidate == contexts_.end()) {
        if (error != nullptr) {
            *error = "all resident contexts are pinned by active requests";
        }
        return std::nullopt;
    }

    Entry &entry = candidate->second;
    if (!swap->save(entry.id, entry.sequence_id, error)) {
        if (error != nullptr && error->empty()) {
            *error = "cannot swap out least-recently-used context";
        }
        return std::nullopt;
    }

    const int sequence = entry.sequence_id;
    entry.sequence_id = -1;
    entry.resident = false;
    ++stats_.swap_outs;
    --stats_.resident_contexts;
    ++stats_.swapped_contexts;
    return sequence;
}

bool ContextManager::make_resident_locked(
    Entry *entry,
    const ContextSwapCallbacks *swap,
    std::string *error) {
    if (entry->resident) return true;
    if (swap == nullptr || !swap->load) {
        if (error != nullptr) *error = "context is swapped out";
        return false;
    }

    const auto sequence =
        obtain_sequence_locked(entry->id, swap, error);
    if (!sequence.has_value()) return false;
    if (!swap->load(entry->id, *sequence, error)) {
        free_sequences_.push_back(*sequence);
        if (error != nullptr && error->empty()) {
            *error = "cannot restore swapped context";
        }
        return false;
    }

    entry->sequence_id = *sequence;
    entry->resident = true;
    ++stats_.swap_ins;
    ++stats_.resident_contexts;
    --stats_.swapped_contexts;
    return true;
}

std::optional<ContextLease> ContextManager::acquire(
    AgentId agent_id,
    AgentId parent_id,
    ContextId requested_context,
    std::string *error,
    const ContextSwapCallbacks *swap) {
    if (error != nullptr) error->clear();
    if (agent_id == 0) {
        if (error != nullptr) *error = "agent id 0 cannot own a context";
        return std::nullopt;
    }

    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (requested_context == 0) {
        if (const auto existing = find_for_agent_locked(agent_id);
            existing.has_value()) {
            Entry &entry = contexts_.at(*existing);
            const bool was_swapped = !entry.resident;
            if (!make_resident_locked(&entry, swap, error)) {
                return std::nullopt;
            }
            entry.last_access = now;
            ++entry.pin_count;
            return ContextLease{.context_id = entry.id,
                                .sequence_id = entry.sequence_id,
                                .created = false,
                                .restored = was_swapped};
        }
        if (contexts_.size() >= max_logical_contexts_) {
            if (error != nullptr) {
                *error = "logical context capacity is exhausted";
            }
            return std::nullopt;
        }
        const auto sequence =
            obtain_sequence_locked(0, swap, error);
        if (!sequence.has_value()) return std::nullopt;

        Entry entry;
        entry.id = next_context_id_++;
        entry.sequence_id = *sequence;
        entry.resident = true;
        entry.pin_count = 1;
        entry.owners.insert(agent_id);
        entry.last_access = now;
        const ContextLease lease{.context_id = entry.id,
                                 .sequence_id = entry.sequence_id,
                                 .created = true,
                                 .restored = false};
        contexts_.emplace(entry.id, std::move(entry));
        ++stats_.created_contexts;
        ++stats_.active_contexts;
        ++stats_.active_owners;
        ++stats_.resident_contexts;
        return lease;
    }

    const auto found = contexts_.find(requested_context);
    if (found == contexts_.end()) {
        if (error != nullptr) *error = "requested context does not exist";
        return std::nullopt;
    }
    Entry &entry = found->second;
    if (!authorized_locked(entry, agent_id, parent_id)) {
        if (error != nullptr) *error = "context ownership check failed";
        return std::nullopt;
    }
    const bool was_swapped = !entry.resident;
    if (!make_resident_locked(&entry, swap, error)) {
        return std::nullopt;
    }
    if (entry.owners.insert(agent_id).second) {
        ++stats_.active_owners;
        ++stats_.shared_acquisitions;
    }
    entry.last_access = now;
    ++entry.pin_count;
    return ContextLease{.context_id = entry.id,
                        .sequence_id = entry.sequence_id,
                        .created = false,
                        .restored = was_swapped};
}

bool ContextManager::finish(ContextId context_id, std::string *error) {
    if (error != nullptr) error->clear();
    std::lock_guard lock(mutex_);
    const auto found = contexts_.find(context_id);
    if (found == contexts_.end()) {
        if (error != nullptr) *error = "context does not exist";
        return false;
    }
    Entry &entry = found->second;
    if (entry.pin_count == 0) {
        if (error != nullptr) *error = "context is not pinned";
        return false;
    }
    --entry.pin_count;
    entry.last_access = std::chrono::steady_clock::now();
    return true;
}

bool ContextManager::release(AgentId agent_id,
                             AgentId parent_id,
                             ContextId context_id,
                             int *released_sequence,
                             std::string *error,
                             bool *context_removed) {
    if (error != nullptr) error->clear();
    if (released_sequence != nullptr) *released_sequence = -1;
    if (context_removed != nullptr) *context_removed = false;
    std::lock_guard lock(mutex_);
    const auto found = contexts_.find(context_id);
    if (found == contexts_.end()) {
        if (error != nullptr) *error = "context does not exist";
        return false;
    }
    Entry &entry = found->second;
    AgentId owner_to_release = agent_id;
    if (!entry.owners.contains(owner_to_release) &&
        parent_id != 0 && entry.owners.contains(parent_id)) {
        owner_to_release = parent_id;
    }
    if (!entry.owners.contains(owner_to_release)) {
        if (error != nullptr) *error = "context ownership check failed";
        return false;
    }
    if (entry.owners.size() == 1 && entry.pin_count != 0) {
        if (error != nullptr) *error = "context is busy";
        return false;
    }

    entry.owners.erase(owner_to_release);
    if (stats_.active_owners > 0) --stats_.active_owners;
    if (!entry.owners.empty()) {
        entry.last_access = std::chrono::steady_clock::now();
        return true;
    }

    if (entry.resident) {
        free_sequences_.push_back(entry.sequence_id);
        --stats_.resident_contexts;
        if (released_sequence != nullptr) {
            *released_sequence = entry.sequence_id;
        }
    } else {
        --stats_.swapped_contexts;
    }
    contexts_.erase(found);
    ++stats_.released_contexts;
    --stats_.active_contexts;
    if (context_removed != nullptr) *context_removed = true;
    return true;
}

bool ContextManager::owns(AgentId agent_id, ContextId context_id) const {
    std::lock_guard lock(mutex_);
    const auto found = contexts_.find(context_id);
    return found != contexts_.end() &&
           found->second.owners.contains(agent_id);
}

ContextManagerStats ContextManager::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

}  // namespace agent_runtime_top
