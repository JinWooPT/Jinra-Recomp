#pragma once

#include <array>

namespace mkwvk {

struct JinraShaderReference {
    int64_t offset = 0;
    size_t words = 0;
    VkShaderStageFlagBits stage{};
    VkShaderModuleIdentifierEXT identifier{VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT};
};

struct JinraGraphicsReferences {
    std::array<JinraShaderReference, 8> stages;
    uint32_t size = 0;
};

bool jinra_lazy_graphics_enabled();
void jinra_identify_shader(const std::vector<uint32_t>& words, JinraShaderReference& stage);
bool jinra_keep_graphics_references(const Program& program, const JinraGraphicsReferences& refs);
bool jinra_read_graphics_references(const Program& program, JinraGraphicsReferences& refs);
VkShaderModule jinra_load_graphics_module(const JinraShaderReference& stage);
void jinra_lazy_graphics_result(bool hit);

}  // namespace mkwvk
