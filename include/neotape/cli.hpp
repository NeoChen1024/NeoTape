#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace neotape {

struct Locator {
    std::string kind;
    std::string locator;
};

enum class ControlPolicy { auto_prompt, none };

enum class VolumePromptChoice { continue_current, change_locator, shell, abort };

struct VolumePromptRequest {
    std::string archive_uuid;
    uint64_t expected_volume = 0;
    Locator current_locator;
    bool write_mode = false;
};

struct VolumePromptResult {
    VolumePromptChoice choice = VolumePromptChoice::abort;
    std::optional<Locator> replacement_locator;
};

Locator parse_locator(std::string_view text);
ControlPolicy parse_control_policy(std::string_view text);
std::string control_policy_name(ControlPolicy policy);
void require_prompt_allowed(ControlPolicy policy);
VolumePromptResult prompt_for_volume_change(const VolumePromptRequest &request);

} // namespace neotape
