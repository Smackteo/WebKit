/*
 * Copyright 2017 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/vk/VulkanInterface.h"
#include "tools/gpu/vk/VkTestMemoryAllocator.h"
#include "tools/gpu/vk/VkTestUtils.h"

#ifdef SK_VULKAN

#ifndef SK_GPU_TOOLS_VK_LIBRARY_NAME
    #if defined _WIN32
        #define SK_GPU_TOOLS_VK_LIBRARY_NAME vulkan-1.dll
    #elif defined SK_BUILD_FOR_MAC
        #define SK_GPU_TOOLS_VK_LIBRARY_NAME libvk_swiftshader.dylib
    #else
        #define SK_GPU_TOOLS_VK_LIBRARY_NAME        libvulkan.so
        #define SK_GPU_TOOLS_VK_LIBRARY_NAME_BACKUP libvulkan.so.1
    #endif
#endif

#define STRINGIFY2(S) #S
#define STRINGIFY(S) STRINGIFY2(S)

#include <algorithm>

#if defined(__GLIBC__)
#include <execinfo.h>
#endif
#include "include/gpu/vk/VulkanBackendContext.h"
#include "include/gpu/vk/VulkanExtensions.h"
#include "src/base/SkAutoMalloc.h"
#include "tools/library/LoadDynamicLibrary.h"

#if defined(SK_ENABLE_SCOPED_LSAN_SUPPRESSIONS)
#include <sanitizer/lsan_interface.h>
#endif

using namespace skia_private;

namespace sk_gpu_test {

bool LoadVkLibraryAndGetProcAddrFuncs(PFN_vkGetInstanceProcAddr* instProc) {
    static void* vkLib = nullptr;
    static PFN_vkGetInstanceProcAddr localInstProc = nullptr;
    if (!vkLib) {
        vkLib = SkLoadDynamicLibrary(STRINGIFY(SK_GPU_TOOLS_VK_LIBRARY_NAME));
        if (!vkLib) {
            // vulkaninfo tries to load the library from two places, so we do as well
            // https://github.com/KhronosGroup/Vulkan-Tools/blob/078d44e4664b7efa0b6c96ebced1995c4425d57a/vulkaninfo/vulkaninfo.h#L249
#ifdef SK_GPU_TOOLS_VK_LIBRARY_NAME_BACKUP
            vkLib = SkLoadDynamicLibrary(STRINGIFY(SK_GPU_TOOLS_VK_LIBRARY_NAME_BACKUP));
            if (!vkLib) {
                return false;
            }
#else
            return false;
#endif
        }
        localInstProc = (PFN_vkGetInstanceProcAddr) SkGetProcedureAddress(vkLib,
                                                                          "vkGetInstanceProcAddr");
    }
    if (!localInstProc) {
        return false;
    }
    *instProc = localInstProc;
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// Helper code to set up Vulkan context objects

#ifdef SK_ENABLE_VK_LAYERS
const char* kDebugLayerNames[] = {
    // single merged layer
    "VK_LAYER_KHRONOS_validation",
    // not included in standard_validation
    //"VK_LAYER_LUNARG_api_dump",
    //"VK_LAYER_LUNARG_vktrace",
    //"VK_LAYER_LUNARG_screenshot",
};

static uint32_t remove_patch_version(uint32_t specVersion) {
    return (specVersion >> 12) << 12;
}

// Returns the index into layers array for the layer we want. Returns -1 if not supported.
static int should_include_debug_layer(const char* layerName,
                                       uint32_t layerCount, VkLayerProperties* layers,
                                       uint32_t version) {
    for (uint32_t i = 0; i < layerCount; ++i) {
        if (!strcmp(layerName, layers[i].layerName)) {
            // Since the layers intercept the vulkan calls and forward them on, we need to make sure
            // layer was written against a version that isn't older than the version of Vulkan we're
            // using so that it has all the api entry points.
            if (version <= remove_patch_version(layers[i].specVersion)) {
                return i;
            }
            return -1;
        }

    }
    return -1;
}

static void print_backtrace() {
#if defined(__GLIBC__)
    void* stack[64];
    int count = backtrace(stack, std::size(stack));
    backtrace_symbols_fd(stack, count, 2);
#else
    // Please add implementations for other platforms.
#endif
}

VKAPI_ATTR VkBool32 VKAPI_CALL
DebugUtilsMessenger(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                    VkDebugUtilsMessageTypeFlagsEXT messageTypes,
                    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
                    void* userData) {
    // VUID-VkDebugUtilsMessengerCallbackDataEXT-pMessage-parameter
    // pMessage must be a null-terminated UTF-8 string
    SkASSERT(callbackData->pMessage != nullptr);

    static constexpr const char* kSkippedMessages[] = {
            "Nothing for now, this string works around msvc bug with empty array",
    };

    // See if it's an issue we are aware of and don't want to be spammed about.
    // Always report the debug message if message ID is missing
    if (callbackData->pMessageIdName != nullptr) {
        for (const char* skipped : kSkippedMessages) {
            if (strstr(callbackData->pMessageIdName, skipped) != nullptr) {
                return VK_FALSE;
            }
        }
    }

    bool printStackTrace = true;
    bool fail = false;

    const char* severity = "message";
    if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        severity = "error";
        fail = true;
    } else if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        severity = "warning";
    } else if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) != 0) {
        severity = "info";
        printStackTrace = false;
    }

    std::string type;
    if ((messageTypes & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) != 0) {
        type += " <general>";
    }
    if ((messageTypes & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0) {
        type += " <validation>";
    }
    if ((messageTypes & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0) {
        type += " <performance>";
    }

    SkDebugf("Vulkan %s%s [%s]: %s\n",
             severity,
             type.c_str(),
             callbackData->pMessageIdName ? callbackData->pMessageIdName : "<no id>",
             callbackData->pMessage);

    if (printStackTrace) {
        print_backtrace();
    }

    if (fail) {
        SkDEBUGFAIL("Vulkan debug layer error");
    }

    return VK_FALSE;
}
#endif

#define ACQUIRE_VK_INST_PROC_LOCAL(name, instance)                                 \
    PFN_vk##name grVk##name =                                                      \
        reinterpret_cast<PFN_vk##name>(getInstProc(instance, "vk" #name));         \
    do {                                                                           \
        if (grVk##name == nullptr) {                                               \
            SkDebugf("Function ptr for vk%s could not be acquired\n", #name);      \
            return false;                                                          \
        }                                                                          \
    } while (0)

// Returns the index into layers array for the layer we want. Returns -1 if not supported.
static bool should_include_extension(const char* extensionName) {
    const char* kValidExtensions[] = {
            // single merged layer
            VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME,
            VK_EXT_BLEND_OPERATION_ADVANCED_EXTENSION_NAME,
            VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME,
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
            VK_EXT_DEVICE_FAULT_EXTENSION_NAME,
            VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
            VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
            VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
            VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME,
            VK_EXT_FRAME_BOUNDARY_EXTENSION_NAME,
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
            VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
            VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME,
            VK_EXT_RGBA10X6_FORMATS_EXTENSION_NAME,
            VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
            VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
            VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME,
            VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
            VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
            VK_KHR_MAINTENANCE1_EXTENSION_NAME,
            VK_KHR_MAINTENANCE2_EXTENSION_NAME,
            VK_KHR_MAINTENANCE3_EXTENSION_NAME,
            VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
            VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME,
            // Below are all platform specific extensions. The name macros like we use above are
            // all defined in platform specific vulkan headers. We currently don't include these
            // headers as they are a little bit of a pain (e.g. windows headers requires including
            // <windows.h> which causes all sorts of fun annoyances/problems. So instead we are
            // just listing the strings these macros are defined to. This really shouldn't cause
            // any long term issues as the chances of the strings connected to the name macros
            // changing is next to zero.
            "VK_KHR_win32_surface",  // VK_KHR_WIN32_SURFACE_EXTENSION_NAME
            "VK_KHR_xcb_surface",    // VK_KHR_XCB_SURFACE_EXTENSION_NAME,
            "VK_ANDROID_external_memory_android_hardware_buffer",
            // VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
            "VK_KHR_android_surface",  // VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };

    for (size_t i = 0; i < std::size(kValidExtensions); ++i) {
        if (!strcmp(extensionName, kValidExtensions[i])) {
            return true;
        }
    }
    return false;
}

static bool init_instance_extensions_and_layers(PFN_vkGetInstanceProcAddr getInstProc,
                                                uint32_t specVersion,
                                                TArray<VkExtensionProperties>* instanceExtensions,
                                                TArray<VkLayerProperties>* instanceLayers) {
    if (getInstProc == nullptr) {
        return false;
    }

    ACQUIRE_VK_INST_PROC_LOCAL(EnumerateInstanceExtensionProperties, VK_NULL_HANDLE);
    ACQUIRE_VK_INST_PROC_LOCAL(EnumerateInstanceLayerProperties, VK_NULL_HANDLE);

    VkResult res;
    uint32_t layerCount = 0;
#ifdef SK_ENABLE_VK_LAYERS
    // instance layers
    res = grVkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    if (VK_SUCCESS != res) {
        return false;
    }
    VkLayerProperties* layers = new VkLayerProperties[layerCount];
    res = grVkEnumerateInstanceLayerProperties(&layerCount, layers);
    if (VK_SUCCESS != res) {
        delete[] layers;
        return false;
    }

    uint32_t nonPatchVersion = remove_patch_version(specVersion);
    for (size_t i = 0; i < std::size(kDebugLayerNames); ++i) {
        int idx = should_include_debug_layer(kDebugLayerNames[i], layerCount, layers,
                                             nonPatchVersion);
        if (idx != -1) {
            instanceLayers->push_back() = layers[idx];
        }
    }
    delete[] layers;
#endif

    // instance extensions
    // via Vulkan implementation and implicitly enabled layers
    {
        uint32_t extensionCount = 0;
        res = grVkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
        if (VK_SUCCESS != res) {
            return false;
        }
        VkExtensionProperties* extensions = new VkExtensionProperties[extensionCount];
        res = grVkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions);
        if (VK_SUCCESS != res) {
            delete[] extensions;
            return false;
        }
        for (uint32_t i = 0; i < extensionCount; ++i) {
            if (should_include_extension(extensions[i].extensionName)) {
                instanceExtensions->push_back() = extensions[i];
            }
        }
        delete [] extensions;
    }

    // via explicitly enabled layers
    layerCount = instanceLayers->size();
    for (uint32_t layerIndex = 0; layerIndex < layerCount; ++layerIndex) {
        uint32_t extensionCount = 0;
        res = grVkEnumerateInstanceExtensionProperties((*instanceLayers)[layerIndex].layerName,
                                                       &extensionCount, nullptr);
        if (VK_SUCCESS != res) {
            return false;
        }
        VkExtensionProperties* extensions = new VkExtensionProperties[extensionCount];
        res = grVkEnumerateInstanceExtensionProperties((*instanceLayers)[layerIndex].layerName,
                                                       &extensionCount, extensions);
        if (VK_SUCCESS != res) {
            delete[] extensions;
            return false;
        }
        for (uint32_t i = 0; i < extensionCount; ++i) {
            if (should_include_extension(extensions[i].extensionName)) {
                instanceExtensions->push_back() = extensions[i];
            }
        }
        delete[] extensions;
    }

    return true;
}

#define GET_PROC_LOCAL(F, inst, device) PFN_vk ## F F = (PFN_vk ## F) getProc("vk" #F, inst, device)

static bool init_device_extensions_and_layers(const skgpu::VulkanGetProc& getProc,
                                              uint32_t specVersion, VkInstance inst,
                                              VkPhysicalDevice physDev,
                                              TArray<VkExtensionProperties>* deviceExtensions,
                                              TArray<VkLayerProperties>* deviceLayers) {
    if (getProc == nullptr) {
        return false;
    }

    GET_PROC_LOCAL(EnumerateDeviceExtensionProperties, inst, VK_NULL_HANDLE);
    GET_PROC_LOCAL(EnumerateDeviceLayerProperties, inst, VK_NULL_HANDLE);

    if (!EnumerateDeviceExtensionProperties ||
        !EnumerateDeviceLayerProperties) {
        return false;
    }

    VkResult res;
    // device layers
    uint32_t layerCount = 0;
#ifdef SK_ENABLE_VK_LAYERS
    res = EnumerateDeviceLayerProperties(physDev, &layerCount, nullptr);
    if (VK_SUCCESS != res) {
        return false;
    }
    VkLayerProperties* layers = new VkLayerProperties[layerCount];
    res = EnumerateDeviceLayerProperties(physDev, &layerCount, layers);
    if (VK_SUCCESS != res) {
        delete[] layers;
        return false;
    }

    uint32_t nonPatchVersion = remove_patch_version(specVersion);
    for (size_t i = 0; i < std::size(kDebugLayerNames); ++i) {
        int idx = should_include_debug_layer(kDebugLayerNames[i], layerCount, layers,
                                             nonPatchVersion);
        if (idx != -1) {
            deviceLayers->push_back() = layers[idx];
        }
    }
    delete[] layers;
#endif

    // device extensions
    // via Vulkan implementation and implicitly enabled layers
    {
        uint32_t extensionCount = 0;
        res = EnumerateDeviceExtensionProperties(physDev, nullptr, &extensionCount, nullptr);
        if (VK_SUCCESS != res) {
            return false;
        }
        VkExtensionProperties* extensions = new VkExtensionProperties[extensionCount];
        res = EnumerateDeviceExtensionProperties(physDev, nullptr, &extensionCount, extensions);
        if (VK_SUCCESS != res) {
            delete[] extensions;
            return false;
        }
        for (uint32_t i = 0; i < extensionCount; ++i) {
            if (should_include_extension(extensions[i].extensionName)) {
                deviceExtensions->push_back() = extensions[i];
            }
        }
        delete[] extensions;
    }

    // via explicitly enabled layers
    layerCount = deviceLayers->size();
    for (uint32_t layerIndex = 0; layerIndex < layerCount; ++layerIndex) {
        uint32_t extensionCount = 0;
        res = EnumerateDeviceExtensionProperties(physDev,
            (*deviceLayers)[layerIndex].layerName,
            &extensionCount, nullptr);
        if (VK_SUCCESS != res) {
            return false;
        }
        VkExtensionProperties* extensions = new VkExtensionProperties[extensionCount];
        res = EnumerateDeviceExtensionProperties(physDev,
            (*deviceLayers)[layerIndex].layerName,
            &extensionCount, extensions);
        if (VK_SUCCESS != res) {
            delete[] extensions;
            return false;
        }
        for (uint32_t i = 0; i < extensionCount; ++i) {
            if (should_include_extension(extensions[i].extensionName)) {
                deviceExtensions->push_back() = extensions[i];
            }
        }
        delete[] extensions;
    }

    return true;
}

#define ACQUIRE_VK_INST_PROC_NOCHECK(name, instance) \
    PFN_vk##name grVk##name = reinterpret_cast<PFN_vk##name>(getInstProc(instance, "vk" #name))

#define ACQUIRE_VK_INST_PROC(name, instance)                                                     \
    PFN_vk##name grVk##name = reinterpret_cast<PFN_vk##name>(getInstProc(instance, "vk" #name)); \
    do {                                                                                         \
        if (grVk##name == nullptr) {                                                             \
            SkDebugf("Function ptr for vk%s could not be acquired\n", #name);                    \
            if (inst != VK_NULL_HANDLE) {                                                        \
                destroy_instance(getInstProc, inst, debugMessenger, hasDebugExtension);          \
            }                                                                                    \
            return false;                                                                        \
        }                                                                                        \
    } while (0)

#define ACQUIRE_VK_PROC_NOCHECK(name, instance, device) \
    PFN_vk##name grVk##name = reinterpret_cast<PFN_vk##name>(getProc("vk" #name, instance, device))

#define ACQUIRE_VK_PROC(name, instance, device)                                         \
    PFN_vk##name grVk##name =                                                           \
            reinterpret_cast<PFN_vk##name>(getProc("vk" #name, instance, device));      \
    do {                                                                                \
        if (grVk##name == nullptr) {                                                    \
            SkDebugf("Function ptr for vk%s could not be acquired\n", #name);           \
            if (inst != VK_NULL_HANDLE) {                                               \
                destroy_instance(getInstProc, inst, debugMessenger, hasDebugExtension); \
            }                                                                           \
            return false;                                                               \
        }                                                                               \
    } while (0)

#define ACQUIRE_VK_PROC_LOCAL(name, instance, device)                              \
    PFN_vk##name grVk##name =                                                      \
            reinterpret_cast<PFN_vk##name>(getProc("vk" #name, instance, device)); \
    do {                                                                           \
        if (grVk##name == nullptr) {                                               \
            SkDebugf("Function ptr for vk%s could not be acquired\n", #name);      \
            return false;                                                          \
        }                                                                          \
    } while (0)

static bool destroy_instance(PFN_vkGetInstanceProcAddr getInstProc,
                             VkInstance inst,
                             VkDebugUtilsMessengerEXT* debugMessenger,
                             bool hasDebugExtension) {
    if (hasDebugExtension && *debugMessenger != VK_NULL_HANDLE) {
        ACQUIRE_VK_INST_PROC_LOCAL(DestroyDebugUtilsMessengerEXT, inst);
        grVkDestroyDebugUtilsMessengerEXT(inst, *debugMessenger, nullptr);
        *debugMessenger = VK_NULL_HANDLE;
    }
    ACQUIRE_VK_INST_PROC_LOCAL(DestroyInstance, inst);
    grVkDestroyInstance(inst, nullptr);
    return true;
}

static bool setup_features(const skgpu::VulkanGetProc& getProc, VkInstance inst,
                           VkPhysicalDevice physDev, uint32_t physDeviceVersion,
                           skgpu::VulkanExtensions* extensions, VkPhysicalDeviceFeatures2* features,
                           bool isProtected) {
    SkASSERT(physDeviceVersion >= VK_API_VERSION_1_1);

    // Setup all extension feature structs we may want to use.
    void** tailPNext = &features->pNext;

    // If |isProtected| is given, attach that first
    VkPhysicalDeviceProtectedMemoryFeatures* protectedMemoryFeatures = nullptr;
    if (isProtected) {
        protectedMemoryFeatures =
          (VkPhysicalDeviceProtectedMemoryFeatures*)sk_malloc_throw(
              sizeof(VkPhysicalDeviceProtectedMemoryFeatures));
        protectedMemoryFeatures->sType =
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES;
        protectedMemoryFeatures->pNext = nullptr;
        *tailPNext = protectedMemoryFeatures;
        tailPNext = &protectedMemoryFeatures->pNext;
    }

    VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT* rasterOrder = nullptr;
    if (extensions->hasExtension(VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME, 1) ||
        extensions->hasExtension(VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME, 1)) {
        rasterOrder =
                (VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT*)sk_malloc_throw(
                        sizeof(VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT));
        rasterOrder->sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT;
        rasterOrder->pNext = nullptr;
        *tailPNext = rasterOrder;
        tailPNext = &rasterOrder->pNext;
    }

    VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT* blend = nullptr;
    if (extensions->hasExtension(VK_EXT_BLEND_OPERATION_ADVANCED_EXTENSION_NAME, 2)) {
        blend = (VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT*) sk_malloc_throw(
                sizeof(VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT));
        blend->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_FEATURES_EXT;
        blend->pNext = nullptr;
        *tailPNext = blend;
        tailPNext = &blend->pNext;
    }

    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT* edsFeature = nullptr;
    if (extensions->hasExtension(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, 1)) {
        edsFeature = (VkPhysicalDeviceExtendedDynamicStateFeaturesEXT*)sk_malloc_throw(
                sizeof(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT));
        edsFeature->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT;
        edsFeature->pNext = nullptr;
        edsFeature->extendedDynamicState = VK_TRUE;
        *tailPNext = edsFeature;
        tailPNext = &edsFeature->pNext;
    }

    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT* eds2Feature = nullptr;
    if (extensions->hasExtension(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, 1)) {
        eds2Feature = (VkPhysicalDeviceExtendedDynamicState2FeaturesEXT*)sk_malloc_throw(
                sizeof(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT));
        eds2Feature->sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;
        eds2Feature->pNext = nullptr;
        eds2Feature->extendedDynamicState2 = VK_TRUE;
        *tailPNext = eds2Feature;
        tailPNext = &eds2Feature->pNext;
    }

    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT* vidsFeature = nullptr;
    if (extensions->hasExtension(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, 1)) {
        vidsFeature = (VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT*)sk_malloc_throw(
                sizeof(VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT));
        vidsFeature->sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT;
        vidsFeature->pNext = nullptr;
        vidsFeature->vertexInputDynamicState = VK_TRUE;
        *tailPNext = vidsFeature;
        tailPNext = &vidsFeature->pNext;
    }

    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT* gplFeature = nullptr;
    if (extensions->hasExtension(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, 1)) {
        gplFeature = (VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT*)sk_malloc_throw(
                sizeof(VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT));
        gplFeature->sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT;
        gplFeature->pNext = nullptr;
        gplFeature->graphicsPipelineLibrary = VK_TRUE;
        *tailPNext = gplFeature;
        tailPNext = &gplFeature->pNext;
    }

    VkPhysicalDeviceSamplerYcbcrConversionFeatures* ycbcrFeature = nullptr;
    {
        ycbcrFeature = (VkPhysicalDeviceSamplerYcbcrConversionFeatures*) sk_malloc_throw(
                sizeof(VkPhysicalDeviceSamplerYcbcrConversionFeatures));
        ycbcrFeature->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
        ycbcrFeature->pNext = nullptr;
        ycbcrFeature->samplerYcbcrConversion = VK_TRUE;
        *tailPNext = ycbcrFeature;
        tailPNext = &ycbcrFeature->pNext;
    }

    VkPhysicalDevicePipelineCreationCacheControlFeatures* cacheControlFeatures = nullptr;
    if (physDeviceVersion >= VK_API_VERSION_1_3 ||
        extensions->hasExtension(VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME, 1)) {
        cacheControlFeatures =
                (VkPhysicalDevicePipelineCreationCacheControlFeatures*)sk_malloc_throw(
                        sizeof(VkPhysicalDevicePipelineCreationCacheControlFeatures));
        cacheControlFeatures->sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES;
        cacheControlFeatures->pNext = nullptr;
        cacheControlFeatures->pipelineCreationCacheControl = VK_TRUE;
        *tailPNext = cacheControlFeatures;
        tailPNext = &cacheControlFeatures->pNext;
    }

    ACQUIRE_VK_PROC_LOCAL(GetPhysicalDeviceFeatures2, inst, VK_NULL_HANDLE);
    grVkGetPhysicalDeviceFeatures2(physDev, features);

    // Disable depth/stencil coherence even if supported, in case it comes with a perf cost.
    if (rasterOrder != nullptr) {
        rasterOrder->rasterizationOrderDepthAttachmentAccess = VK_FALSE;
        rasterOrder->rasterizationOrderStencilAttachmentAccess = VK_FALSE;
    }

    if (isProtected) {
        if (!protectedMemoryFeatures->protectedMemory) {
            return false;
        }
    }
    return true;
    // If we want to disable any extension features do so here.
}

bool CreateVkBackendContext(PFN_vkGetInstanceProcAddr getInstProc,
                            skgpu::VulkanBackendContext* ctx,
                            skgpu::VulkanExtensions* extensions,
                            VkPhysicalDeviceFeatures2* features,
                            VkDebugUtilsMessengerEXT* debugMessenger,
                            uint32_t* presentQueueIndexPtr,
                            const CanPresentFn& canPresent,
                            bool isProtected) {
    VkResult err;

    ACQUIRE_VK_INST_PROC_NOCHECK(EnumerateInstanceVersion, VK_NULL_HANDLE);
    uint32_t instanceVersion = 0;
    // Vulkan 1.1 is required, so vkEnumerateInstanceVersion should always be available.
    SkASSERT(grVkEnumerateInstanceVersion != nullptr);
    err = grVkEnumerateInstanceVersion(&instanceVersion);
    if (err) {
        SkDebugf("failed to enumerate instance version. Err: %d\n", err);
        return false;
    }
    SkASSERT(instanceVersion >= VK_API_VERSION_1_1);
    // We can set the apiVersion to be whatever the highest api we may use in skia. For now we
    // set it to 1.1 since that is the most common Vulkan version on Android devices.
    const uint32_t apiVersion = VK_API_VERSION_1_1;

    instanceVersion = std::min(instanceVersion, apiVersion);

    STArray<2, VkPhysicalDevice> physDevs;
    VkDevice device;
    VkInstance inst = VK_NULL_HANDLE;

    const VkApplicationInfo app_info = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO, // sType
        nullptr,                            // pNext
        "vktest",                           // pApplicationName
        0,                                  // applicationVersion
        "vktest",                           // pEngineName
        0,                                  // engineVerison
        apiVersion,                         // apiVersion
    };

    TArray<VkLayerProperties> instanceLayers;
    TArray<VkExtensionProperties> instanceExtensions;

    if (!init_instance_extensions_and_layers(getInstProc, instanceVersion,
                                             &instanceExtensions,
                                             &instanceLayers)) {
        return false;
    }

    TArray<const char*> instanceLayerNames;
    TArray<const char*> instanceExtensionNames;
    for (int i = 0; i < instanceLayers.size(); ++i) {
        instanceLayerNames.push_back(instanceLayers[i].layerName);
    }
    for (int i = 0; i < instanceExtensions.size(); ++i) {
        instanceExtensionNames.push_back(instanceExtensions[i].extensionName);
    }

    VkInstanceCreateInfo instance_create = {
            VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,   // sType
            nullptr,                                  // pNext
            0,                                        // flags
            &app_info,                                // pApplicationInfo
            (uint32_t)instanceLayerNames.size(),      // enabledLayerNameCount
            instanceLayerNames.begin(),               // ppEnabledLayerNames
            (uint32_t)instanceExtensionNames.size(),  // enabledExtensionNameCount
            instanceExtensionNames.begin(),           // ppEnabledExtensionNames
    };

    bool hasDebugExtension = false;
    *debugMessenger = VK_NULL_HANDLE;

#ifdef SK_ENABLE_VK_LAYERS
    for (int i = 0; i < instanceExtensionNames.size() && !hasDebugExtension; ++i) {
        if (!strcmp(instanceExtensionNames[i], VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            hasDebugExtension = true;
        }
    }

    // Fine grain control of validation layer features
    const char* name = "VK_LAYER_KHRONOS_validation";
    const VkBool32 settingValidateCore = VK_TRUE;
    // Syncval is disabled for now, but would be useful to enable eventually.
    const VkBool32 settingValidateSync = VK_FALSE;
    const VkBool32 settingThreadSafety = VK_TRUE;
    // Shader validation could be useful (previously broken on Android, might already be fixed:
    // http://anglebug.com/42265520).
    const VkBool32 settingCheckShaders = VK_FALSE;
    // If syncval is enabled, submit time validation could stay disabled due to performance issues:
    // https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/7285
    const VkBool32 settingSyncvalSubmitTimeValidation = VK_FALSE;
    // Extra properties in syncval make it easier to filter the messages.
    const VkBool32 settingSyncvalMessageExtraProperties = VK_TRUE;
    const VkLayerSettingEXT layerSettings[] = {
            {name, "validate_core", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &settingValidateCore},
            {name, "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &settingValidateSync},
            {name, "thread_safety", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &settingThreadSafety},
            {name, "check_shaders", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &settingCheckShaders},
            {name,
             "syncval_submit_time_validation",
             VK_LAYER_SETTING_TYPE_BOOL32_EXT,
             1,
             &settingSyncvalSubmitTimeValidation},
            {name,
             "syncval_message_extra_properties",
             VK_LAYER_SETTING_TYPE_BOOL32_EXT,
             1,
             &settingSyncvalMessageExtraProperties},
    };
    VkLayerSettingsCreateInfoEXT layerSettingsCreateInfo = {};
    layerSettingsCreateInfo.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
    layerSettingsCreateInfo.settingCount = static_cast<uint32_t>(std::size(layerSettings));
    layerSettingsCreateInfo.pSettings = layerSettings;
    if (hasDebugExtension) {
        instance_create.pNext = &layerSettingsCreateInfo;
    }
#endif

    ACQUIRE_VK_INST_PROC(CreateInstance, VK_NULL_HANDLE);
    err = grVkCreateInstance(&instance_create, nullptr, &inst);
    if (err < 0) {
        SkDebugf("vkCreateInstance failed: %d\n", err);
        return false;
    }

    ACQUIRE_VK_INST_PROC(GetDeviceProcAddr, inst);
    auto getProc = [getInstProc, grVkGetDeviceProcAddr](const char* proc_name,
                                                        VkInstance instance, VkDevice device) {
        if (device != VK_NULL_HANDLE) {
            return grVkGetDeviceProcAddr(device, proc_name);
        }
        return getInstProc(instance, proc_name);
    };

#ifdef SK_ENABLE_VK_LAYERS
    if (hasDebugExtension) {
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo = {};

        constexpr VkDebugUtilsMessageSeverityFlagsEXT kSeveritiesToLog =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;

        constexpr VkDebugUtilsMessageTypeFlagsEXT kMessagesToLog =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

        messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messengerInfo.messageSeverity = kSeveritiesToLog;
        messengerInfo.messageType = kMessagesToLog;
        messengerInfo.pfnUserCallback = &DebugUtilsMessenger;

        ACQUIRE_VK_PROC(CreateDebugUtilsMessengerEXT, inst, VK_NULL_HANDLE);
        // Register the callback
        grVkCreateDebugUtilsMessengerEXT(inst, &messengerInfo, nullptr, debugMessenger);
    }
#endif

    ACQUIRE_VK_PROC(EnumeratePhysicalDevices, inst, VK_NULL_HANDLE);
    ACQUIRE_VK_PROC(GetPhysicalDeviceProperties, inst, VK_NULL_HANDLE);
    ACQUIRE_VK_PROC(GetPhysicalDeviceQueueFamilyProperties, inst, VK_NULL_HANDLE);
    ACQUIRE_VK_PROC(GetPhysicalDeviceFeatures, inst, VK_NULL_HANDLE);
    ACQUIRE_VK_PROC(CreateDevice, inst, VK_NULL_HANDLE);
    ACQUIRE_VK_PROC(GetDeviceQueue, inst, VK_NULL_HANDLE);
    ACQUIRE_VK_PROC(DeviceWaitIdle, inst, VK_NULL_HANDLE);
    ACQUIRE_VK_PROC(DestroyDevice, inst, VK_NULL_HANDLE);

    uint32_t gpuCount;
    err = grVkEnumeratePhysicalDevices(inst, &gpuCount, nullptr);
    if (err) {
        SkDebugf("vkEnumeratePhysicalDevices failed: %d\n", err);
        destroy_instance(getInstProc, inst, debugMessenger, hasDebugExtension);
        return false;
    }
    if (!gpuCount) {
        SkDebugf("vkEnumeratePhysicalDevices returned no supported devices.\n");
        destroy_instance(getInstProc, inst, debugMessenger, hasDebugExtension);
        return false;
    }
    // Allocate enough storage for all available physical devices. We should be able to just ask for
    // the first one, but a bug in RenderDoc (https://github.com/baldurk/renderdoc/issues/2766)
    // will smash the stack if we do that.
    physDevs.resize(gpuCount);
    err = grVkEnumeratePhysicalDevices(inst, &gpuCount, physDevs.data());
    if (err) {
        SkDebugf("vkEnumeratePhysicalDevices failed: %d\n", err);
        destroy_instance(getInstProc, inst, debugMessenger, hasDebugExtension);
        return false;
    }
    // We just use the first physical device.
    // TODO: find best match for our needs
    VkPhysicalDevice physDev = physDevs.front();

    VkPhysicalDeviceProperties physDeviceProperties;
    grVkGetPhysicalDeviceProperties(physDev, &physDeviceProperties);
    uint32_t physDeviceVersion = std::min(physDeviceProperties.apiVersion, apiVersion);

    // query to get the initial queue props size
    uint32_t queueCount;
    grVkGetPhysicalDeviceQueueFamilyProperties(physDev, &queueCount, nullptr);
    if (!queueCount) {
        SkDebugf("vkGetPhysicalDeviceQueueFamilyProperties returned no queues.\n");
        destroy_instance(getInstProc, inst, debugMessenger, hasDebugExtension);
        return false;
    }

    SkAutoMalloc queuePropsAlloc(queueCount * sizeof(VkQueueFamilyProperties));
    // now get the actual queue props
    VkQueueFamilyProperties* queueProps = (VkQueueFamilyProperties*)queuePropsAlloc.get();

    grVkGetPhysicalDeviceQueueFamilyProperties(physDev, &queueCount, queueProps);

    // iterate to find the graphics queue
    uint32_t graphicsQueueIndex = queueCount;
    for (uint32_t i = 0; i < queueCount; i++) {
        if (queueProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsQueueIndex = i;
            break;
        }
    }
    if (graphicsQueueIndex == queueCount) {
        SkDebugf("Could not find any supported graphics queues.\n");
        destroy_instance(getInstProc, inst, debugMessenger, hasDebugExtension);
        return false;
    }

    // iterate to find the present queue, if needed
    uint32_t presentQueueIndex = queueCount;
    if (presentQueueIndexPtr && canPresent) {
        for (uint32_t i = 0; i < queueCount; i++) {
            if (canPresent(inst, physDev, i)) {
                presentQueueIndex = i;
                break;
            }
        }
        if (presentQueueIndex == queueCount) {
            SkDebugf("Could not find any supported present queues.\n");
            destroy_instance(getInstProc, inst, debugMessenger, hasDebugExtension);
            return false;
        }
        *presentQueueIndexPtr = presentQueueIndex;
    } else {
        // Just setting this so we end up make a single queue for graphics since there was no
        // request for a present queue.
        presentQueueIndex = graphicsQueueIndex;
    }

    TArray<VkLayerProperties> deviceLayers;
    TArray<VkExtensionProperties> deviceExtensions;
    if (!init_device_extensions_and_layers(getProc, physDeviceVersion,
                                           inst, physDev,
                                           &deviceExtensions,
                                           &deviceLayers)) {
        destroy_instance(getInstProc, inst, debugMessenger, hasDebugExtension);
        return false;
    }

    TArray<const char*> deviceLayerNames;
    TArray<const char*> deviceExtensionNames;
    for (int i = 0; i < deviceLayers.size(); ++i) {
        deviceLayerNames.push_back(deviceLayers[i].layerName);
    }

    for (int i = 0; i < deviceExtensions.size(); ++i) {
        deviceExtensionNames.push_back(deviceExtensions[i].extensionName);
    }

    extensions->init(getProc, inst, physDev,
                     (uint32_t) instanceExtensionNames.size(),
                     instanceExtensionNames.begin(),
                     (uint32_t) deviceExtensionNames.size(),
                     deviceExtensionNames.begin());

    memset(features, 0, sizeof(VkPhysicalDeviceFeatures2));
    features->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features->pNext = nullptr;

    if (!setup_features(getProc, inst, physDev, physDeviceVersion, extensions, features,
                        isProtected)) {
        destroy_instance(getInstProc, inst, debugMessenger, hasDebugExtension);
        return false;
    }

    // This looks like it would slow things down,
    // and we can't depend on it on all platforms
    features->features.robustBufferAccess = VK_FALSE;

    VkDeviceQueueCreateFlags flags = isProtected ? VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT : 0;
    float queuePriorities[1] = { 0.0 };
    // Here we assume no need for swapchain queue
    // If one is needed, the client will need its own setup code
    const VkDeviceQueueCreateInfo queueInfo[2] = {
        {
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, // sType
            nullptr,                                    // pNext
            flags,                                      // VkDeviceQueueCreateFlags
            graphicsQueueIndex,                         // queueFamilyIndex
            1,                                          // queueCount
            queuePriorities,                            // pQueuePriorities

        },
        {
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, // sType
            nullptr,                                    // pNext
            0,                                          // VkDeviceQueueCreateFlags
            presentQueueIndex,                          // queueFamilyIndex
            1,                                          // queueCount
            queuePriorities,                            // pQueuePriorities
        }
    };
    uint32_t queueInfoCount = (presentQueueIndex != graphicsQueueIndex) ? 2 : 1;

    const VkDeviceCreateInfo deviceInfo = {
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,        // sType
        features,                                    // pNext
        0,                                           // VkDeviceCreateFlags
        queueInfoCount,                              // queueCreateInfoCount
        queueInfo,                                   // pQueueCreateInfos
        (uint32_t) deviceLayerNames.size(),          // layerCount
        deviceLayerNames.begin(),                    // ppEnabledLayerNames
        (uint32_t) deviceExtensionNames.size(),      // extensionCount
        deviceExtensionNames.begin(),                // ppEnabledExtensionNames
        nullptr,                                     // ppEnabledFeatures
    };

    {
#if defined(SK_ENABLE_SCOPED_LSAN_SUPPRESSIONS)
        // skbug.com/40040003
        __lsan::ScopedDisabler lsanDisabler;
#endif
        err = grVkCreateDevice(physDev, &deviceInfo, nullptr, &device);
    }
    if (err) {
        SkDebugf("CreateDevice failed: %d\n", err);
        destroy_instance(getInstProc, inst, debugMessenger, hasDebugExtension);
        return false;
    }

    VkQueue queue;
    if (isProtected) {
        ACQUIRE_VK_PROC(GetDeviceQueue2, inst, device);
        SkASSERT(grVkGetDeviceQueue2 != nullptr);
        VkDeviceQueueInfo2 queue_info2 = {
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,          // sType
            nullptr,                                        // pNext
            VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT,           // flags
            graphicsQueueIndex,                             // queueFamilyIndex
            0                                               // queueIndex
        };
        grVkGetDeviceQueue2(device, &queue_info2, &queue);
    } else {
        grVkGetDeviceQueue(device, graphicsQueueIndex, 0, &queue);
    }

    skgpu::VulkanInterface interface = skgpu::VulkanInterface(
            getProc, inst, device, instanceVersion, physDeviceVersion, extensions);
    SkASSERT(interface.validate(instanceVersion, physDeviceVersion, extensions));

    sk_sp<skgpu::VulkanMemoryAllocator> memoryAllocator = VkTestMemoryAllocator::Make(
            inst, physDev, device, physDeviceVersion, extensions, &interface);

    ctx->fInstance = inst;
    ctx->fPhysicalDevice = physDev;
    ctx->fDevice = device;
    ctx->fQueue = queue;
    ctx->fGraphicsQueueIndex = graphicsQueueIndex;
    ctx->fMaxAPIVersion = apiVersion;
    ctx->fVkExtensions = extensions;
    ctx->fDeviceFeatures2 = features;
    ctx->fGetProc = getProc;
    ctx->fProtectedContext = skgpu::Protected(isProtected);
    ctx->fMemoryAllocator = memoryAllocator;

    return true;
}

void FreeVulkanFeaturesStructs(const VkPhysicalDeviceFeatures2* features) {
    // All Vulkan structs that could be part of the features chain will start with the
    // structure type followed by the pNext pointer. We cast to the CommonVulkanHeader
    // so we can get access to the pNext for the next struct.
    struct CommonVulkanHeader {
        VkStructureType sType;
        void*           pNext;
    };

    void* pNext = features->pNext;
    while (pNext) {
        void* current = pNext;
        pNext = static_cast<CommonVulkanHeader*>(current)->pNext;
        sk_free(current);
    }
}

}  // namespace sk_gpu_test

#endif
