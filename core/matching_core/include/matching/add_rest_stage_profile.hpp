#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

namespace matching {

enum class AddRestStage : std::uint8_t {
    kValidation = 0,
    kMatch,
    kPoolAcquire,
    kNodeInit,
    kLevelLookupExisting,
    kLevelCreateNew,
    kFifoAppend,
    kIdIndexInsert,
    kCount
};

inline constexpr std::size_t kAddRestStageCount =
    static_cast<std::size_t>(AddRestStage::kCount);

struct AddRestStageStats {
    std::uint64_t count = 0;
    std::uint64_t total_ns = 0;
    std::uint64_t total_cycles = 0;
};

struct AddRestStageProfileSnapshot {
    std::uint64_t add_rest_count = 0;
    std::array<AddRestStageStats, kAddRestStageCount> stages{};
};

const char* AddRestStageName(AddRestStage stage) noexcept;

void ResetAddRestStageProfile() noexcept;
void SetAddRestStageProfileEnabled(bool enabled) noexcept;
bool AddRestStageProfileEnabled() noexcept;
void RecordAddRestStageProfile(
    const std::array<std::uint64_t, kAddRestStageCount>& stage_ns,
    const std::array<std::uint64_t, kAddRestStageCount>& stage_cycles) noexcept;
AddRestStageProfileSnapshot GetAddRestStageProfileSnapshot() noexcept;

}  // namespace matching
