#pragma once

#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_dlssg_vk.h>

#define JINRA_ANR_BRIDGE_ABI 1u

#ifdef JINRA_ANR_BRIDGE_BUILD
#define JINRA_ANR_EXPORT __declspec(dllexport)
#else
#define JINRA_ANR_EXPORT
#endif

// One serialized session per process. Provider and data paths must be absolute local paths.
// The caller owns all Vulkan objects, records outside render passes, and drains
// the queues before release/shutdown. Images passed to eval use GENERAL layout.
extern "C" {
JINRA_ANR_EXPORT uint32_t jinra_anr_bridge_version();
JINRA_ANR_EXPORT NVSDK_NGX_Result jinra_anr_init(const wchar_t* provider_path, const wchar_t* data_path,
    VkInstance instance, VkPhysicalDevice physical, VkDevice device,
    PFN_vkGetInstanceProcAddr instance_proc, PFN_vkGetDeviceProcAddr device_proc, uint32_t* max_frames);
JINRA_ANR_EXPORT NVSDK_NGX_Result jinra_anr_create(VkCommandBuffer cmd, const NVSDK_NGX_DLSSG_Create_Params* create);
JINRA_ANR_EXPORT NVSDK_NGX_Result jinra_anr_eval(VkCommandBuffer cmd, uint64_t backbuffer_sequence,
    const NVSDK_NGX_VK_DLSSG_Eval_Params* resources, const NVSDK_NGX_DLSSG_Opt_Eval_Params* options);
JINRA_ANR_EXPORT NVSDK_NGX_Result jinra_anr_release();
JINRA_ANR_EXPORT NVSDK_NGX_Result jinra_anr_shutdown();
}

#undef JINRA_ANR_EXPORT
