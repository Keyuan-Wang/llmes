#include "matching/add_rest_stage_profile.hpp"

#include <atomic>

namespace matching {
namespace {

std::atomic<bool> g_add_rest_stage_profile_enabled{false};
AddRestStageProfileSnapshot g_add_rest_stage_profile{};

}  // namespace

const char* AddRestStageName(AddRestStage stage) noexcept {
    switch (stage) {
    case AddRestStage::kValidation:
        return "validation";
    case AddRestStage::kMatch:
        return "match";
    case AddRestStage::kPoolAcquire:
        return "pool_acquire";
    case AddRestStage::kNodeInit:
        return "node_init";
    case AddRestStage::kLevelLookupExisting:
        return "level_lookup_existing";
    case AddRestStage::kLevelCreateNew:
        return "level_create_new";
    case AddRestStage::kFifoAppend:
        return "fifo_append";
    case AddRestStage::kIdIndexInsert:
        return "id_index_insert";
    case AddRestStage::kCount:
        break;
    }
    return "unknown";
}

void ResetAddRestStageProfile() noexcept {
    g_add_rest_stage_profile_enabled.store(false, std::memory_order_relaxed);
    g_add_rest_stage_profile = AddRestStageProfileSnapshot{};
}

void SetAddRestStageProfileEnabled(bool enabled) noexcept {
    g_add_rest_stage_profile_enabled.store(enabled, std::memory_order_relaxed);
}

bool AddRestStageProfileEnabled() noexcept {
    return g_add_rest_stage_profile_enabled.load(std::memory_order_relaxed);
}

void RecordAddRestStageProfile(
    const std::array<std::uint64_t, kAddRestStageCount>& stage_ns,
    const std::array<std::uint64_t, kAddRestStageCount>& stage_cycles) noexcept {
    ++g_add_rest_stage_profile.add_rest_count;
    for (std::size_t i = 0; i < kAddRestStageCount; ++i) {
        auto& stage = g_add_rest_stage_profile.stages[i];
        ++stage.count;
        stage.total_ns += stage_ns[i];
        stage.total_cycles += stage_cycles[i];
    }
}

AddRestStageProfileSnapshot GetAddRestStageProfileSnapshot() noexcept {
    return g_add_rest_stage_profile;
}

}  // namespace matching
