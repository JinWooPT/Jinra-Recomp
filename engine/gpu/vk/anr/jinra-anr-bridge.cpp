#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_NO_PROTOTYPES
#define NGX_SNIPPET_BUILD
#include <windows.h>
#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>

#include <map>
#include <string>
#include <type_traits>
#include <variant>
#include <wctype.h>

namespace {

// The provider calls this virtual interface with the Windows MSVC ABI.
class Parameters final : public NVSDK_NGX_Parameter {
    using Value = std::variant<unsigned long long, float, double, unsigned, int,
                               ID3D11Resource*, ID3D12Resource*, void*>;
    std::map<std::string, Value, std::less<>> values;

    template <class T> void write(const char* name, T value) {
        const auto position = values.lower_bound(name);
        if (position != values.end() && position->first == name) {
            position->second = value;
        } else {
            values.emplace_hint(position, name, value);
        }
    }

    template <class T> NVSDK_NGX_Result read(const char* name, T* out) const {
        const auto it = values.find(name);
        if (it == values.end()) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        return std::visit([&](auto value) {
            if constexpr (std::is_convertible_v<decltype(value), T>) {
                *out = static_cast<T>(value);
                return NVSDK_NGX_Result_Success;
            } else {
                return NVSDK_NGX_Result_FAIL_InvalidParameter;
            }
        }, it->second);
    }

public:
    void Set(const char* name, unsigned long long value) override { write(name, value); }
    void Set(const char* name, float value) override { write(name, value); }
    void Set(const char* name, double value) override { write(name, value); }
    void Set(const char* name, unsigned value) override { write(name, value); }
    void Set(const char* name, int value) override { write(name, value); }
    void Set(const char* name, ID3D11Resource* value) override { write(name, value); }
    void Set(const char* name, ID3D12Resource* value) override { write(name, value); }
    void Set(const char* name, void* value) override { write(name, value); }

    NVSDK_NGX_Result Get(const char* name, unsigned long long* out) const override { return read(name, out); }
    NVSDK_NGX_Result Get(const char* name, float* out) const override { return read(name, out); }
    NVSDK_NGX_Result Get(const char* name, double* out) const override { return read(name, out); }
    NVSDK_NGX_Result Get(const char* name, unsigned* out) const override { return read(name, out); }
    NVSDK_NGX_Result Get(const char* name, int* out) const override { return read(name, out); }
    NVSDK_NGX_Result Get(const char* name, ID3D11Resource** out) const override { return read(name, out); }
    NVSDK_NGX_Result Get(const char* name, ID3D12Resource** out) const override { return read(name, out); }
    NVSDK_NGX_Result Get(const char* name, void** out) const override { return read(name, out); }
    void Reset() override { values.clear(); }
};

using Populate = NVSDK_NGX_Result (NVSDK_CONV*)(NVSDK_NGX_Parameter*);

struct Session {
    HMODULE provider = nullptr;
    decltype(&NVSDK_NGX_VULKAN_Init_Ext2) init = nullptr;
    Populate populate = nullptr;
    decltype(&NVSDK_NGX_VULKAN_CreateFeature1) create = nullptr;
    decltype(&NVSDK_NGX_VULKAN_EvaluateFeature) evaluate = nullptr;
    decltype(&NVSDK_NGX_VULKAN_ReleaseFeature) release = nullptr;
    decltype(&NVSDK_NGX_VULKAN_Shutdown1) shutdown = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    NVSDK_NGX_Handle* feature = nullptr;
    bool ready = false;
    Parameters params;
};

// The caller serializes calls and owns queue lifetime.
Session session;

template <class T> bool resolve(T& function, const char* name) {
    function = reinterpret_cast<T>(GetProcAddress(session.provider, name));
    return function != nullptr;
}

bool local_absolute(const wchar_t* path) {
    return path && iswalpha(path[0]) && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

NVSDK_NGX_Result provider_create(VkCommandBuffer cmd, NVSDK_NGX_Feature feature,
        NVSDK_NGX_Parameter* params, NVSDK_NGX_Handle** handle) {
    return session.create(session.device, cmd, feature, params, handle);
}

NVSDK_NGX_Result provider_evaluate(VkCommandBuffer cmd, const NVSDK_NGX_Handle* handle,
        const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback_C) {
    return session.evaluate(cmd, handle, params, nullptr);
}

} // namespace

// Reuse the SDK's parameter serialization without importing the NVIDIA loader.
#define NVSDK_NGX_Parameter_SetUI(params, name, value) (params)->Set(name, static_cast<unsigned>(value))
#define NVSDK_NGX_Parameter_SetF(params, name, value) (params)->Set(name, static_cast<float>(value))
#define NVSDK_NGX_Parameter_SetVoidPointer(params, name, value) (params)->Set(name, static_cast<void*>(value))
#define NVSDK_NGX_Parameter_GetVoidPointer(params, name, out) (params)->Get(name, out)
#define NVSDK_NGX_VULKAN_CreateFeature provider_create
#define NVSDK_NGX_VULKAN_EvaluateFeature_C provider_evaluate
#define JINRA_ANR_BRIDGE_BUILD
#include "jinra-anr-bridge.h"
#undef NVSDK_NGX_VULKAN_EvaluateFeature_C
#undef NVSDK_NGX_VULKAN_CreateFeature
#undef NVSDK_NGX_Parameter_GetVoidPointer
#undef NVSDK_NGX_Parameter_SetVoidPointer
#undef NVSDK_NGX_Parameter_SetF
#undef NVSDK_NGX_Parameter_SetUI

uint32_t jinra_anr_bridge_version() {
    return JINRA_ANR_BRIDGE_ABI;
}

NVSDK_NGX_Result jinra_anr_init(const wchar_t* provider_path, const wchar_t* data_path,
        VkInstance instance, VkPhysicalDevice physical, VkDevice device,
        PFN_vkGetInstanceProcAddr instance_proc, PFN_vkGetDeviceProcAddr device_proc, uint32_t* max_frames) {
    if (!max_frames) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *max_frames = 0;
    if (!local_absolute(provider_path) || !local_absolute(data_path) ||
        !instance || !physical || !device || !instance_proc || !device_proc)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (session.provider) return NVSDK_NGX_Result_FAIL_FeatureAlreadyExists;

    session.provider = LoadLibraryExW(provider_path, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!session.provider) return NVSDK_NGX_Result_FAIL_PlatformError;
    if (!resolve(session.init, "NVSDK_NGX_VULKAN_Init_Ext2") ||
        !resolve(session.populate, "NVSDK_NGX_VULKAN_PopulateParameters_Impl") ||
        !resolve(session.create, "NVSDK_NGX_VULKAN_CreateFeature1") ||
        !resolve(session.evaluate, "NVSDK_NGX_VULKAN_EvaluateFeature") ||
        !resolve(session.release, "NVSDK_NGX_VULKAN_ReleaseFeature") ||
        !resolve(session.shutdown, "NVSDK_NGX_VULKAN_Shutdown1")) {
        FreeLibrary(session.provider);
        session.provider = nullptr;
        return NVSDK_NGX_Result_FAIL_OutOfDate;
    }

    auto result = session.init(1, data_path, instance, physical, device, instance_proc, device_proc,
                               NVSDK_NGX_Version_API, nullptr);
    if (NVSDK_NGX_FAILED(result)) return result;
    session.device = device;
    result = session.populate(&session.params);
    if (NVSDK_NGX_FAILED(result)) return result;
    int available = 0;
    session.params.Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
    if (!available) return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    unsigned limit = 0;
    session.params.Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &limit);
    *max_frames = limit ? limit : 1;
    session.ready = true;
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result jinra_anr_create(VkCommandBuffer cmd, const NVSDK_NGX_DLSSG_Create_Params* create) {
    if (!session.ready) return NVSDK_NGX_Result_FAIL_NotInitialized;
    if (session.feature) return NVSDK_NGX_Result_FAIL_FeatureAlreadyExists;
    if (!cmd || !create) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    NVSDK_NGX_DLSSG_Create_Params settings = *create;
    session.params.Set(NVSDK_NGX_DLSSG_Parameter_Width, settings.Width);
    session.params.Set(NVSDK_NGX_DLSSG_Parameter_Height, settings.Height);
    session.params.Set(NVSDK_NGX_DLSSG_Parameter_InternalWidth, settings.RenderWidth);
    session.params.Set(NVSDK_NGX_DLSSG_Parameter_InternalHeight, settings.RenderHeight);
    session.params.Set(NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0u);
    const auto result = NGX_VK_CREATE_DLSSG(cmd, 1, 1, &session.feature, &session.params, &settings);
    if (NVSDK_NGX_SUCCEED(result) && !session.feature) return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    return result;
}

NVSDK_NGX_Result jinra_anr_eval(VkCommandBuffer cmd, uint64_t backbuffer_sequence,
        const NVSDK_NGX_VK_DLSSG_Eval_Params* resources, const NVSDK_NGX_DLSSG_Opt_Eval_Params* options) {
    if (!session.feature) return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    if (!cmd || !resources || !options) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    NVSDK_NGX_VK_DLSSG_Eval_Params inputs = *resources;
    NVSDK_NGX_DLSSG_Opt_Eval_Params settings = *options;
    session.params.Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, static_cast<unsigned long long>(backbuffer_sequence));
    return NGX_VK_EVALUATE_DLSSG(cmd, session.feature, &session.params, &inputs, &settings);
}

NVSDK_NGX_Result jinra_anr_release() {
    if (!session.feature) return NVSDK_NGX_Result_Success;
    const auto result = session.release(session.feature);
    if (NVSDK_NGX_SUCCEED(result)) session.feature = nullptr;
    return result;
}

NVSDK_NGX_Result jinra_anr_shutdown() {
    if (!session.device) return NVSDK_NGX_Result_Success;
    auto result = jinra_anr_release();
    if (NVSDK_NGX_FAILED(result)) return result;
    result = session.shutdown(session.device);
    if (NVSDK_NGX_SUCCEED(result)) {
        session.device = VK_NULL_HANDLE;
        session.ready = false;
        session.params.Reset();
    }
    // Keep the provider mapped after initialization. V.B18 has crashed in
    // teardown on the Vulkan probe, and the game already owns process exit.
    return result;
}
