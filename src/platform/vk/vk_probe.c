/*
 * SwanStationPS5 v2 - Vulkan bring-up: proves the linked RADV driver works on the
 * console by creating an instance and listing the GPU, without drawing.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * RADV (Mihawk-99's PS5 port of Mesa's AMD Vulkan driver) is linked into the
 * title; a PS5 title can't load a driver library at run time. It exports the
 * ICD entry point, vk_icdGetInstanceProcAddr, and every command is looked up
 * through it (as PS5 RetroArch's src/radv_icd_ps5.c does).
 */
#include "vk_probe.h"

#if defined(SwanStationPS5_VULKAN)

#include "../../SwanStationPS5.h"

#include <stdio.h>
#include <vulkan/vulkan.h>

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance,
                                                                   const char *name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                               const char *name)
{
    return vk_icdGetInstanceProcAddr(instance, name);
}

#define LOAD(instance, name) PFN_##name name = (PFN_##name)vkGetInstanceProcAddr(instance, #name)

static void probe(void)
{
    LOAD(NULL, vkCreateInstance);
    LOAD(NULL, vkEnumerateInstanceVersion);
    if (!vkCreateInstance)
    {
        SwanStationPS5_log("vulkan: the driver has no vkCreateInstance");
        return;
    }
    uint32_t version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion)
        vkEnumerateInstanceVersion(&version);
    SwanStationPS5_log("vulkan: instance version %u.%u.%u", VK_API_VERSION_MAJOR(version),
              VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));

    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = SwanStationPS5_NAME;
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = SwanStationPS5_NAME;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&info, NULL, &instance);
    if (r != VK_SUCCESS)
    {
        SwanStationPS5_log("vulkan: vkCreateInstance failed (%d)", (int)r);
        return;
    }
    LOAD(instance, vkEnumeratePhysicalDevices);
    LOAD(instance, vkGetPhysicalDeviceProperties);
    LOAD(instance, vkGetPhysicalDeviceMemoryProperties);
    LOAD(instance, vkDestroyInstance);
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, NULL);
    VkPhysicalDevice devices[4];
    if (count > 4)
        count = 4;
    vkEnumeratePhysicalDevices(instance, &count, devices);
    SwanStationPS5_log("vulkan: %u GPU(s)", count);
    for (uint32_t i = 0; i < count; ++i)
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        VkPhysicalDeviceMemoryProperties m;
        vkGetPhysicalDeviceMemoryProperties(devices[i], &m);
        uint64_t mb = 0;
        for (uint32_t h = 0; h < m.memoryHeapCount; ++h)
            mb += m.memoryHeaps[h].size >> 20;
        SwanStationPS5_log("vulkan: GPU %u: %s, Vulkan %u.%u.%u, %u memory heaps, %llu MB", i, p.deviceName,
                  VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                  VK_API_VERSION_PATCH(p.apiVersion), m.memoryHeapCount, (unsigned long long)mb);
    }
    vkDestroyInstance(instance, NULL);
    SwanStationPS5_log("vulkan: probe finished");
}

void vk_probe(const char *root)
{
    /* If the previous probe took the app down, don't try again: delete the
     * marker to retry. */
    char marker[SwanStationPS5_PATH_MAX];
    path_join(marker, sizeof(marker), root, "vulkan_probe.crashed");
    if (path_exists(marker))
    {
        SwanStationPS5_log("vulkan: skipped, the last probe did not finish (%s)", marker);
        return;
    }
    FILE *f = fopen(marker, "w");
    if (f)
        fclose(f);
    probe();
    remove(marker);
}

#else

void vk_probe(const char *root)
{
    (void)root;
}

#endif
