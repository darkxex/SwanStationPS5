/*
 * SwanStationPS5 v2 - the PC's side of the presenter: an SDL window's Vulkan surface instead of VK_KHR_display.
 * Included by vk_present.c in place of its create_instance/create_surface when built as vk_present_desktop.c.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The swapchain follows the window: vkp_begin_frame calls desktop_swapchain_ready(), which makes the swapchain and
 * what hangs from it again when the window's size changed (as vkp_set_mode does for another TV mode).
 */
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

static SDL_Window *desktop_window;

void vkp_desktop_window(SDL_Window *window)
{
    desktop_window = window;
}

static void desktop_drawable(uint32_t *w, uint32_t *h)
{
    int dw = 0, dh = 0;
    if (desktop_window)
        SDL_Vulkan_GetDrawableSize(desktop_window, &dw, &dh);
    *w = dw > 0 ? (uint32_t)dw : 0;
    *h = dh > 0 ? (uint32_t)dh : 0;
}

/* The swapchain's size when the surface leaves it to us (currentExtent undefined). */
static VkExtent2D fallback_extent(const VkSurfaceCapabilitiesKHR *caps)
{
    uint32_t w, h;
    desktop_drawable(&w, &h);
    if (w < caps->minImageExtent.width)
        w = caps->minImageExtent.width;
    if (h < caps->minImageExtent.height)
        h = caps->minImageExtent.height;
    if (w > caps->maxImageExtent.width)
        w = caps->maxImageExtent.width;
    if (h > caps->maxImageExtent.height)
        h = caps->maxImageExtent.height;
    return (VkExtent2D){w, h};
}

static bool create_instance(char *error, size_t size)
{
    if (!desktop_window)
    {
        snprintf(error, size, "no window");
        return false;
    }
    vkCreateInstance_ = (PFN_vkCreateInstance)vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    if (!vkCreateInstance_)
    {
        snprintf(error, size, "the Vulkan driver is missing");
        return false;
    }
    const char *extensions[16];
    unsigned extension_count = 16;
    if (!SDL_Vulkan_GetInstanceExtensions(desktop_window, &extension_count, extensions))
    {
        snprintf(error, size, "SDL has no Vulkan surface: %s", SDL_GetError());
        return false;
    }
    /* SwanStationPS5_VK_VALIDATE=1: the Khronos validation layer, for development */
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char *validate = SDL_getenv("SwanStationPS5_VK_VALIDATE");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = SwanStationPS5_NAME;
    app.pEngineName = SwanStationPS5_NAME;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = extension_count;
    info.ppEnabledExtensionNames = extensions;
    if (validate && validate[0] == '1')
    {
        info.enabledLayerCount = 1;
        info.ppEnabledLayerNames = layers;
    }
    CHECK(vkCreateInstance_(&info, NULL, &V.instance), "vkCreateInstance");
    /* the VK_KHR_display commands stay NULL here: the window's surface needs none of them */
#define LOAD_INSTANCE(name) name = (PFN_##name)vkGetInstanceProcAddr(V.instance, #name);
    VK_INSTANCE_FUNCS(LOAD_INSTANCE)
    if (!vkCreateDevice || !vkGetDeviceProcAddr || !vkDestroySurfaceKHR || !vkGetPhysicalDeviceSurfaceSupportKHR ||
        !vkGetPhysicalDeviceSurfaceCapabilitiesKHR || !vkGetPhysicalDeviceSurfaceFormatsKHR)
    {
        snprintf(error, size, "the Vulkan driver lacks the surface commands");
        return false;
    }
    VkPhysicalDevice gpus[8];
    uint32_t n = 8;
    VkResult r = vkEnumeratePhysicalDevices(V.instance, &n, gpus);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0)
    {
        snprintf(error, size, "no GPU");
        return false;
    }
    /* the first discrete GPU; SwanStationPS5_GPU=<index> picks another */
    uint32_t pick = 0;
    bool found_discrete = false;
    const char *wanted = SDL_getenv("SwanStationPS5_GPU");
    for (uint32_t i = 0; i < n; ++i)
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(gpus[i], &p);
        SwanStationPS5_log("vulkan: GPU %u: %s (type %d, Vulkan %u.%u)", i, p.deviceName, (int)p.deviceType,
                           VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion));
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && !found_discrete)
        {
            found_discrete = true;
            pick = i;
        }
    }
    if (wanted && atoi(wanted) >= 0 && (uint32_t)atoi(wanted) < n)
        pick = (uint32_t)atoi(wanted);
    V.gpu = gpus[pick];
    return true;
}

static bool create_surface(char *error, size_t size)
{
    if (!SDL_Vulkan_CreateSurface(desktop_window, V.instance, &V.surface))
    {
        snprintf(error, size, "SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        return false;
    }
    int display = SDL_GetWindowDisplayIndex(desktop_window);
    SDL_DisplayMode mode;
    V.display_hz = display >= 0 && SDL_GetCurrentDisplayMode(display, &mode) == 0 ? mode.refresh_rate : 0.0;
    uint32_t w, h;
    desktop_drawable(&w, &h);
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(V.gpu, &p);
    snprintf(V.description, sizeof(V.description), "%ux%u window on %s @ %.2f Hz", w, h, p.deviceName, V.display_hz);
    return true;
}

/* false: skip this frame (minimised, or the swapchain could not be made again); defined in vk_present_desktop.c */
static bool desktop_swapchain_ready(void);
