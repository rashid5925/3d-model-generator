#include "render/vk_loader.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifdef SF_STATIC_MOLTENVK
// Exported by the statically linked MoltenVK library.
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName);
#endif

namespace sf {

#define SF_VK_DEFINE(name) PFN_##name name = nullptr;
SF_VK_DEFINE(vkGetInstanceProcAddr)
SF_VK_GLOBAL_FUNCTIONS(SF_VK_DEFINE)
SF_VK_INSTANCE_FUNCTIONS(SF_VK_DEFINE)
#undef SF_VK_DEFINE

#ifndef SF_STATIC_MOLTENVK
namespace {

void* openLibrary(std::string& which) {
#if defined(_WIN32)
    const char* names[] = {"vulkan-1.dll"};
    for (const char* n : names) {
        if (HMODULE h = LoadLibraryA(n)) {
            which = n;
            return reinterpret_cast<void*>(h);
        }
    }
#elif defined(__APPLE__)
    const char* names[] = {"libvulkan.1.dylib", "libvulkan.dylib", "/usr/local/lib/libvulkan.1.dylib",
                           "/opt/homebrew/lib/libvulkan.1.dylib", "libMoltenVK.dylib",
                           "/usr/local/lib/libMoltenVK.dylib", "/opt/homebrew/lib/libMoltenVK.dylib"};
    for (const char* n : names) {
        if (void* h = dlopen(n, RTLD_NOW | RTLD_LOCAL)) {
            which = n;
            return h;
        }
    }
#else
    const char* names[] = {"libvulkan.so.1", "libvulkan.so"};
    for (const char* n : names) {
        if (void* h = dlopen(n, RTLD_NOW | RTLD_LOCAL)) {
            which = n;
            return h;
        }
    }
#endif
    return nullptr;
}

void* findSymbol(void* lib, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

}  // namespace
#endif

bool loadVulkan(std::string& error, std::string& source) {
    static bool loaded = false;
    static std::string loadedFrom;
    if (loaded) {
        source = loadedFrom;
        return true;
    }
#ifdef SF_STATIC_MOLTENVK
    sf::vkGetInstanceProcAddr = ::vkGetInstanceProcAddr;
    loadedFrom = "built-in MoltenVK";
#else
    std::string which;
    void* lib = openLibrary(which);
    if (!lib) {
        error = "no Vulkan loader found (install GPU drivers with Vulkan support)";
        return false;
    }
    sf::vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(findSymbol(lib, "vkGetInstanceProcAddr"));
    if (!sf::vkGetInstanceProcAddr) {
        error = std::string(which) + " does not export vkGetInstanceProcAddr";
        return false;
    }
    loadedFrom = "system loader " + which;
#endif
#define SF_VK_LOAD_GLOBAL(name)                                                                  \
    name = reinterpret_cast<PFN_##name>(sf::vkGetInstanceProcAddr(VK_NULL_HANDLE, #name));       \
    if (!name) {                                                                                 \
        error = "missing Vulkan entry point " #name;                                            \
        return false;                                                                            \
    }
    SF_VK_GLOBAL_FUNCTIONS(SF_VK_LOAD_GLOBAL)
#undef SF_VK_LOAD_GLOBAL
    loaded = true;
    source = loadedFrom;
    return true;
}

bool loadVulkanInstanceFunctions(VkInstance instance, std::string& error) {
#define SF_VK_LOAD_INSTANCE(name)                                                         \
    name = reinterpret_cast<PFN_##name>(sf::vkGetInstanceProcAddr(instance, #name));      \
    if (!name) {                                                                          \
        error = "missing Vulkan entry point " #name;                                     \
        return false;                                                                     \
    }
    SF_VK_INSTANCE_FUNCTIONS(SF_VK_LOAD_INSTANCE)
#undef SF_VK_LOAD_INSTANCE
    return true;
}

}  // namespace sf
