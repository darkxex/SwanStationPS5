/*
 * SwanStationPS5 v2 - the screen through Vulkan (RADV, VK_KHR_display on VideoOut).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * RADV's PS5 window system presents through VK_KHR_display: a display plane
 * surface on the TV, a FIFO swapchain flipped at vblank. Its images may only
 * be colour attachments, so the frame is drawn with a small pipeline that
 * samples textures (shaders/quad.*): first the game picture (once a core
 * renders through Vulkan), then the interface canvas, blended on top.
 *
 * The canvas is still drawn by the CPU (SDL's software renderer); each frame
 * it is copied into a mapped staging buffer and then into a sampled image.
 *
 * A core that renders through Vulkan (Beetle PSX HW) creates the VkDevice
 * itself, through libretro's context negotiation: the presenter then moves
 * onto that device (vkp_adopt_device) and draws the core's image under the
 * canvas, which leaves a transparent hole where the game shows.
 */
#include "vk_present.h"
#include "vk_present_hw.h"

#if defined(SwanStationPS5_VULKAN)

#include "../../SwanStationPS5.h"
#include "shaders_royale.h"
#include "shaders_spv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES /* every command is a pointer looked up below */
#include <vulkan/vulkan.h>

/* defined in vk_probe.c, forwarding to RADV's ICD entry point */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name);

#define FRAMES 2 /* frames in flight */
#define MAX_IMAGES 8

/* ---------------------------------------------------------------- functions */

#define VK_INSTANCE_FUNCS(X)                                                                       \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)          \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties)           \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceDisplayPropertiesKHR)          \
    X(vkGetDisplayModePropertiesKHR) X(vkGetPhysicalDeviceDisplayPlanePropertiesKHR)             \
    X(vkGetDisplayPlaneSupportedDisplaysKHR) X(vkGetDisplayPlaneCapabilitiesKHR)                 \
    X(vkCreateDisplayPlaneSurfaceKHR) X(vkDestroySurfaceKHR)                                       \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)         \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)

#define VK_DEVICE_FUNCS(X)                                                                         \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateSwapchainKHR)           \
    X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR)                 \
    X(vkQueuePresentKHR) X(vkQueueSubmit) X(vkCreateImageView) X(vkDestroyImageView)             \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)     \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkCreateRenderPass)                     \
    X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer)                        \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout)                   \
    X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline)                 \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool)     \
    X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)             \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCreateCommandPool) X(vkDestroyCommandPool)        \
    X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)                    \
    X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage)                    \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline)                           \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdSetViewport)            \
    X(vkCmdSetScissor) X(vkCmdBlitImage) X(vkCmdCopyImageToBuffer) X(vkQueueWaitIdle) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences)    \
    X(vkCreateSemaphore) X(vkDestroySemaphore)

#define DECLARE(name) static PFN_##name name;
VK_INSTANCE_FUNCS(DECLARE)
VK_DEVICE_FUNCS(DECLARE)
static PFN_vkCreateInstance vkCreateInstance_;

/* ---------------------------------------------------------------- state */

static struct
{
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage images[MAX_IMAGES];
    VkImageView views[MAX_IMAGES];
    VkFramebuffer framebuffers[MAX_IMAGES];
    VkSemaphore rendered[MAX_IMAGES];
    VkRenderPass pass;
    VkPipelineLayout layout;
    VkPipeline opaque, blended;
    VkPipeline shaded[4]; /* the game through sharp.frag, crt.frag, lcd3x.frag, downsample.frag */
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkSampler sampler, sampler_nearest; /* the game picture uses _nearest when the final scale is not smoothed */
    bool game_nearest;
    /* the interface canvas */
    int cw, ch;
    VkImage canvas;
    VkDeviceMemory canvas_memory;
    VkImageView canvas_view;
    VkDescriptorSet canvas_set;
    VkBuffer staging[FRAMES];
    VkDeviceMemory staging_memory[FRAMES];
    void *staging_map[FRAMES];
    VkCommandPool commands;
    VkCommandBuffer cmd[FRAMES];
    VkFence done[FRAMES];
    VkSemaphore acquired[FRAMES];
    int frame;
    bool canvas_ready;
    char description[64];
    /* the game picture of a core rendering through Vulkan */
    VkDescriptorSet game_sets[FRAMES];
    VkImageView game_view;
    VkImage game_image; /* for thumbnails: blitted down and read back */
    VkImageLayout game_layout;
    bool game_shown; /* plat asked for the picture this frame */
    float game_rect[4];
    float game_crop; /* share of the picture's height hidden at top and bottom */
    int game_shader;  /* 0 none, 1 LCD3x, 2 CRT Royale, 3 CRT, 4 sharp bilinear, 5 supersampling */
    float game_tex[2], game_lines;
} V;

const char *vkp_describe(void)
{
    return V.description;
}

/* ---------------------------------------------------------------- helpers */

#define CHECK(call, what)                                                                          \
    do                                                                                             \
    {                                                                                              \
        VkResult r_ = (call);                                                                      \
        if (r_ != VK_SUCCESS)                                                                      \
        {                                                                                          \
            snprintf(error, size, "%s failed (%d)", what, (int)r_);                                \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

static int memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties m;
    vkGetPhysicalDeviceMemoryProperties(V.gpu, &m);
    for (uint32_t i = 0; i < m.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static bool allocate(VkMemoryRequirements req, VkMemoryPropertyFlags want, VkDeviceMemory *out)
{
    int type = memory_type(req.memoryTypeBits, want);
    if (type < 0)
        return false;
    VkMemoryAllocateInfo info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = req.size;
    info.memoryTypeIndex = (uint32_t)type;
    return vkAllocateMemory(V.device, &info, NULL, out) == VK_SUCCESS;
}

static void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src,
                    VkPipelineStageFlags dst)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, src, dst, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ---------------------------------------------------------------- setup steps */

static bool create_instance(char *error, size_t size)
{
    vkCreateInstance_ = (PFN_vkCreateInstance)vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    if (!vkCreateInstance_)
    {
        snprintf(error, size, "the Vulkan driver is missing");
        return false;
    }
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = SwanStationPS5_NAME;
    app.pEngineName = SwanStationPS5_NAME;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = extensions;
    CHECK(vkCreateInstance_(&info, NULL, &V.instance), "vkCreateInstance");
#define LOAD_INSTANCE(name)                                                                        \
    name = (PFN_##name)vkGetInstanceProcAddr(V.instance, #name);                                   \
    if (!name)                                                                                     \
    {                                                                                              \
        snprintf(error, size, "missing %s", #name);                                               \
        return false;                                                                              \
    }
    VK_INSTANCE_FUNCS(LOAD_INSTANCE)
    uint32_t n = 1;
    VkResult r = vkEnumeratePhysicalDevices(V.instance, &n, &V.gpu);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0)
    {
        snprintf(error, size, "no GPU");
        return false;
    }
    return true;
}

/* The TV: a 1920x1080 mode at about 60 Hz (the canvas size: no scaling), and
 * a plane that can show it. */
static bool create_surface(char *error, size_t size)
{
    uint32_t n = 1;
    VkDisplayPropertiesKHR display;
    VkResult r = vkGetPhysicalDeviceDisplayPropertiesKHR(V.gpu, &n, &display);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0)
    {
        snprintf(error, size, "no display");
        return false;
    }
    VkDisplayModePropertiesKHR modes[16];
    uint32_t mode_count = 16;
    vkGetDisplayModePropertiesKHR(V.gpu, display.display, &mode_count, modes);
    int best = -1;
    for (uint32_t i = 0; i < mode_count; ++i)
    {
        const VkDisplayModeParametersKHR *p = &modes[i].parameters;
        SwanStationPS5_log("vulkan: display mode %ux%u @ %.2f Hz", p->visibleRegion.width,
                  p->visibleRegion.height, p->refreshRate / 1000.0);
        bool size_ok = p->visibleRegion.width == (uint32_t)V.cw && p->visibleRegion.height == (uint32_t)V.ch;
        bool rate_ok = p->refreshRate >= 59000 && p->refreshRate <= 61000;
        if (size_ok && rate_ok)
            best = (int)i;
        else if (size_ok && best < 0)
            best = (int)i;
    }
    if (best < 0)
    {
        if (mode_count == 0)
        {
            snprintf(error, size, "no display modes");
            return false;
        }
        best = 0;
    }
    const VkDisplayModePropertiesKHR *mode = &modes[best];

    VkDisplayPlanePropertiesKHR planes[8];
    uint32_t plane_count = 8;
    vkGetPhysicalDeviceDisplayPlanePropertiesKHR(V.gpu, &plane_count, planes);
    uint32_t plane = 0;
    for (uint32_t i = 0; i < plane_count; ++i)
    {
        VkDisplayKHR supported[4];
        uint32_t count = 4;
        vkGetDisplayPlaneSupportedDisplaysKHR(V.gpu, i, &count, supported);
        bool ok = false;
        for (uint32_t k = 0; k < count; ++k)
            ok |= supported[k] == display.display;
        if (ok)
        {
            plane = i;
            break;
        }
    }
    VkDisplayPlaneCapabilitiesKHR caps;
    vkGetDisplayPlaneCapabilitiesKHR(V.gpu, mode->displayMode, plane, &caps);
    VkDisplayPlaneAlphaFlagBitsKHR alpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedAlpha & alpha))
        for (uint32_t bit = 1; bit <= VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR; bit <<= 1)
            if (caps.supportedAlpha & bit)
            {
                alpha = (VkDisplayPlaneAlphaFlagBitsKHR)bit;
                break;
            }

    VkDisplaySurfaceCreateInfoKHR info = {VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR};
    info.displayMode = mode->displayMode;
    info.planeIndex = plane;
    info.planeStackIndex = plane < plane_count ? planes[plane].currentStackIndex : 0;
    info.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    info.globalAlpha = 1.0f;
    info.alphaMode = alpha;
    info.imageExtent = mode->parameters.visibleRegion;
    CHECK(vkCreateDisplayPlaneSurfaceKHR(V.instance, &info, NULL, &V.surface),
          "vkCreateDisplayPlaneSurfaceKHR");
    snprintf(V.description, sizeof(V.description), "%ux%u @ %.2f Hz",
             mode->parameters.visibleRegion.width, mode->parameters.visibleRegion.height,
             mode->parameters.refreshRate / 1000.0);
    return true;
}

static bool load_device(char *error, size_t size)
{
#define LOAD_DEVICE(name)                                                                          \
    name = (PFN_##name)vkGetDeviceProcAddr(V.device, #name);                                       \
    if (!name)                                                                                     \
    {                                                                                              \
        snprintf(error, size, "missing %s", #name);                                               \
        return false;                                                                              \
    }
    VK_DEVICE_FUNCS(LOAD_DEVICE)
    return true;
}

static bool create_device(char *error, size_t size)
{
    VkQueueFamilyProperties families[8];
    uint32_t n = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(V.gpu, &n, families);
    V.family = UINT32_MAX;
    for (uint32_t i = 0; i < n; ++i)
    {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(V.gpu, i, V.surface, &present);
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
        {
            V.family = i;
            break;
        }
    }
    if (V.family == UINT32_MAX)
    {
        snprintf(error, size, "no queue can draw and present");
        return false;
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = V.family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = extensions;
    CHECK(vkCreateDevice(V.gpu, &info, NULL, &V.device), "vkCreateDevice");
    if (!load_device(error, size))
        return false;
    vkGetDeviceQueue(V.device, V.family, 0, &V.queue);
    return true;
}

static bool create_swapchain(char *error, size_t size)
{
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.gpu, V.surface, &caps), "surface capabilities");
    VkSurfaceFormatKHR formats[16];
    uint32_t n = 16;
    vkGetPhysicalDeviceSurfaceFormatsKHR(V.gpu, V.surface, &n, formats);
    if (n == 0)
    {
        snprintf(error, size, "no surface formats");
        return false;
    }
    VkSurfaceFormatKHR chosen = formats[0];
    for (uint32_t i = 0; i < n; ++i)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM)
        {
            chosen = formats[i];
            break;
        }
    V.format = chosen.format;
    V.extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent
                                                      : (VkExtent2D){(uint32_t)V.cw, (uint32_t)V.ch};
    uint32_t images = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && images > caps.maxImageCount)
        images = caps.maxImageCount;

    VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = V.surface;
    info.minImageCount = images;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = V.extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                              ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                              : (VkCompositeAlphaFlagBitsKHR)caps.supportedCompositeAlpha;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    CHECK(vkCreateSwapchainKHR(V.device, &info, NULL, &V.swapchain), "vkCreateSwapchainKHR");
    V.image_count = MAX_IMAGES;
    CHECK(vkGetSwapchainImagesKHR(V.device, V.swapchain, &V.image_count, V.images), "swapchain images");
    SwanStationPS5_log("vulkan: swapchain %ux%u, %u images, format %d", V.extent.width, V.extent.height,
              V.image_count, (int)V.format);
    return true;
}

static bool create_pipeline(char *error, size_t size)
{
    /* render pass: clear to black, draw, present */
    VkAttachmentDescription color = {0};
    color.format = V.format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {0};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    VkSubpassDependency dep = {0};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &color;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    CHECK(vkCreateRenderPass(V.device, &rp, NULL, &V.pass), "vkCreateRenderPass");

    for (uint32_t i = 0; i < V.image_count; ++i)
    {
        VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = V.images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = V.format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        CHECK(vkCreateImageView(V.device, &view, NULL, &V.views[i]), "swapchain view");
        VkFramebufferCreateInfo fb = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = V.pass;
        fb.attachmentCount = 1;
        fb.pAttachments = &V.views[i];
        fb.width = V.extent.width;
        fb.height = V.extent.height;
        fb.layers = 1;
        CHECK(vkCreateFramebuffer(V.device, &fb, NULL, &V.framebuffers[i]), "framebuffer");
        VkSemaphoreCreateInfo sem = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        CHECK(vkCreateSemaphore(V.device, &sem, NULL, &V.rendered[i]), "semaphore");
    }

    /* one texture per draw */
    VkDescriptorSetLayoutBinding binding = {0};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dsl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl.bindingCount = 1;
    dsl.pBindings = &binding;
    CHECK(vkCreateDescriptorSetLayout(V.device, &dsl, NULL, &V.set_layout), "set layout");
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &V.set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    CHECK(vkCreatePipelineLayout(V.device, &pl, NULL, &V.layout), "pipeline layout");

    VkShaderModule vert, frag;
    VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof(SPV_QUAD_VERT);
    sm.pCode = SPV_QUAD_VERT;
    CHECK(vkCreateShaderModule(V.device, &sm, NULL, &vert), "vertex shader");
    sm.codeSize = sizeof(SPV_QUAD_FRAG);
    sm.pCode = SPV_QUAD_FRAG;
    CHECK(vkCreateShaderModule(V.device, &sm, NULL, &frag), "fragment shader");
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend = {0};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = V.layout;
    gp.renderPass = V.pass;
    VkResult r = vkCreateGraphicsPipelines(V.device, VK_NULL_HANDLE, 1, &gp, NULL, &V.opaque);
    /* the game's shaders: the same opaque pipeline with another fragment stage */
    const uint32_t *shader_code[4] = {SPV_SHARP_FRAG, SPV_CRT_FRAG, SPV_LCD3X_FRAG, SPV_DOWNSAMPLE_FRAG};
    const size_t shader_size[4] = {sizeof(SPV_SHARP_FRAG), sizeof(SPV_CRT_FRAG), sizeof(SPV_LCD3X_FRAG),
                                   sizeof(SPV_DOWNSAMPLE_FRAG)};
    for (int k = 0; k < 4 && r == VK_SUCCESS; ++k)
    {
        VkShaderModule fm;
        sm.codeSize = shader_size[k];
        sm.pCode = shader_code[k];
        r = vkCreateShaderModule(V.device, &sm, NULL, &fm);
        if (r != VK_SUCCESS)
            break;
        stages[1].module = fm;
        r = vkCreateGraphicsPipelines(V.device, VK_NULL_HANDLE, 1, &gp, NULL, &V.shaded[k]);
        vkDestroyShaderModule(V.device, fm, NULL);
    }
    stages[1].module = frag;
    /* the interface over the game: its alpha is already multiplied in by
     * SDL's blending onto a transparent canvas */
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    if (r == VK_SUCCESS)
        r = vkCreateGraphicsPipelines(V.device, VK_NULL_HANDLE, 1, &gp, NULL, &V.blended);
    vkDestroyShaderModule(V.device, vert, NULL);
    vkDestroyShaderModule(V.device, frag, NULL);
    CHECK(r, "vkCreateGraphicsPipelines");

    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    CHECK(vkCreateSampler(V.device, &si, NULL, &V.sampler), "sampler");
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    CHECK(vkCreateSampler(V.device, &si, NULL, &V.sampler_nearest), "nearest sampler");
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 8;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    CHECK(vkCreateDescriptorPool(V.device, &dp, NULL, &V.pool), "descriptor pool");
    return true;
}

static bool create_canvas(char *error, size_t size)
{
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = (VkExtent3D){(uint32_t)V.cw, (uint32_t)V.ch, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkCreateImage(V.device, &ii, NULL, &V.canvas), "canvas image");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(V.device, V.canvas, &req);
    if (!allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &V.canvas_memory))
    {
        snprintf(error, size, "no GPU memory for the canvas");
        return false;
    }
    CHECK(vkBindImageMemory(V.device, V.canvas, V.canvas_memory, 0), "bind canvas");
    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = V.canvas;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    CHECK(vkCreateImageView(V.device, &view, NULL, &V.canvas_view), "canvas view");

    VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = V.pool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &V.set_layout;
    CHECK(vkAllocateDescriptorSets(V.device, &da, &V.canvas_set), "canvas descriptor");
    VkDescriptorImageInfo di = {V.sampler, V.canvas_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = V.canvas_set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &di;
    vkUpdateDescriptorSets(V.device, 1, &w, 0, NULL);

    for (int f = 0; f < FRAMES; ++f)
    {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = (VkDeviceSize)V.cw * V.ch * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        CHECK(vkCreateBuffer(V.device, &bi, NULL, &V.staging[f]), "staging buffer");
        vkGetBufferMemoryRequirements(V.device, V.staging[f], &req);
        if (!allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &V.staging_memory[f]))
        {
            snprintf(error, size, "no memory for the staging buffer");
            return false;
        }
        CHECK(vkBindBufferMemory(V.device, V.staging[f], V.staging_memory[f], 0), "bind staging");
        CHECK(vkMapMemory(V.device, V.staging_memory[f], 0, VK_WHOLE_SIZE, 0, &V.staging_map[f]),
              "map staging");
    }

    VkDescriptorSetLayout layouts[FRAMES];
    for (int f = 0; f < FRAMES; ++f)
        layouts[f] = V.set_layout;
    da.descriptorSetCount = FRAMES;
    da.pSetLayouts = layouts;
    CHECK(vkAllocateDescriptorSets(V.device, &da, V.game_sets), "game descriptors");

    VkCommandPoolCreateInfo cp = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = V.family;
    CHECK(vkCreateCommandPool(V.device, &cp, NULL, &V.commands), "command pool");
    VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = V.commands;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = FRAMES;
    CHECK(vkAllocateCommandBuffers(V.device, &ca, V.cmd), "command buffers");
    for (int f = 0; f < FRAMES; ++f)
    {
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        CHECK(vkCreateFence(V.device, &fi, NULL, &V.done[f]), "fence");
        VkSemaphoreCreateInfo sem = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        CHECK(vkCreateSemaphore(V.device, &sem, NULL, &V.acquired[f]), "semaphore");
    }
    return true;
}

/* ---------------------------------------------------------------- API */

bool vkp_open(int canvas_w, int canvas_h, char *error, size_t size)
{
    memset(&V, 0, sizeof(V));
    V.cw = canvas_w;
    V.ch = canvas_h;
    bool ok = create_instance(error, size) && create_surface(error, size) &&
              create_device(error, size) && create_swapchain(error, size) &&
              create_pipeline(error, size) && create_canvas(error, size);
    if (!ok)
    {
        SwanStationPS5_log("vulkan: screen not available: %s", error);
        vkp_close();
        return false;
    }
    SwanStationPS5_log("vulkan: the screen is drawn through Vulkan, %s", V.description);
    return true;
}

/* A rectangle in screen pixels -> clip space for the quad shader. */
static float colour_k[4] = {1.0f, 1.0f, 0.0f, 0.0f}; /* the game picture: brightness, saturation, warmth */

void vkp_set_colour(float brightness, float saturation, float warmth, float sharpen)
{
    colour_k[0] = brightness;
    colour_k[1] = saturation;
    colour_k[2] = warmth;
    colour_k[3] = sharpen;
}

static void draw_quad(VkCommandBuffer cb, VkDescriptorSet set, float x, float y, float w, float h, float v0,
                      float v1, const float info[4], const float colour[4])
{
    float k[16] = {x / V.extent.width * 2.0f - 1.0f, y / V.extent.height * 2.0f - 1.0f,
                   (x + w) / V.extent.width * 2.0f - 1.0f, (y + h) / V.extent.height * 2.0f - 1.0f,
                   0.0f, v0, 1.0f, v1, info[0], info[1], info[2], info[3],
                   colour[0], colour[1], colour[2], colour[3]};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, V.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(k), k);
    vkCmdDraw(cb, 6, 1, 0, 0);
}


/* ---------------------------------------------------------------- crt-royale */

/* The "CRT Royale" shader (crt-royale-fast by TroggleMonkey and Hyllian): a
 * chain of passes, each drawing into an image the next one reads. Their
 * SPIR-V, push constants and samplers come from shaders_royale.h
 * (tools/make-royale.py). The first pass is given the PS1's own picture size
 * (the game is drawn bigger, at the internal resolution); the last one is
 * drawn straight into the swapchain, inside the game's rectangle. */

#define ROYALE_N ((int)(sizeof(ROYALE_PASSES) / sizeof(ROYALE_PASSES[0])))
#define ROYALE_TEX_N ((int)(sizeof(ROYALE_TEXTURES) / sizeof(ROYALE_TEXTURES[0])))
#define SHADER_ROYALE 2 /* the value of Settings > Display > Shader */

static struct
{
    bool tried, ok;
    VkRenderPass rp[2]; /* an intermediate image: [0] linear, [1] sRGB */
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipe[8];
    VkSampler sampler[2][3]; /* [linear][border, edge, repeat] */
    VkDescriptorPool pool;
    VkDescriptorSet sets[FRAMES][8];
    VkBuffer ubo;
    VkDeviceMemory ubo_memory;
    VkImage tex_image[4];
    VkDeviceMemory tex_memory[4];
    VkImageView tex_view[4];
    struct
    {
        VkImage image;
        VkDeviceMemory memory;
        VkImageView view;
        VkFramebuffer fb;
    } out[8]; /* the images of every pass but the last */
    int size[8][2];    /* each pass's output size */
    int native[2];     /* the PS1's picture */
    int built[4];      /* what the images were made for: native size, viewport size */
    bool have_images;
    uint32_t frames;
} R;

static void royale_free_images(void)
{
    for (int i = 0; i < 8; ++i)
    {
        if (R.out[i].fb)
            vkDestroyFramebuffer(V.device, R.out[i].fb, NULL);
        if (R.out[i].view)
            vkDestroyImageView(V.device, R.out[i].view, NULL);
        if (R.out[i].image)
            vkDestroyImage(V.device, R.out[i].image, NULL);
        if (R.out[i].memory)
            vkFreeMemory(V.device, R.out[i].memory, NULL);
        memset(&R.out[i], 0, sizeof(R.out[i]));
    }
    R.have_images = false;
}

static void royale_free(void)
{
    if (!V.device)
        return;
    royale_free_images();
    for (int i = 0; i < 8; ++i)
        if (R.pipe[i])
            vkDestroyPipeline(V.device, R.pipe[i], NULL);
    for (int t = 0; t < 4; ++t)
    {
        if (R.tex_view[t])
            vkDestroyImageView(V.device, R.tex_view[t], NULL);
        if (R.tex_image[t])
            vkDestroyImage(V.device, R.tex_image[t], NULL);
        if (R.tex_memory[t])
            vkFreeMemory(V.device, R.tex_memory[t], NULL);
    }
    if (R.ubo)
        vkDestroyBuffer(V.device, R.ubo, NULL);
    if (R.ubo_memory)
        vkFreeMemory(V.device, R.ubo_memory, NULL);
    if (R.pool)
        vkDestroyDescriptorPool(V.device, R.pool, NULL);
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 3; ++b)
            if (R.sampler[a][b])
                vkDestroySampler(V.device, R.sampler[a][b], NULL);
    if (R.layout)
        vkDestroyPipelineLayout(V.device, R.layout, NULL);
    if (R.set_layout)
        vkDestroyDescriptorSetLayout(V.device, R.set_layout, NULL);
    for (int s = 0; s < 2; ++s)
        if (R.rp[s])
            vkDestroyRenderPass(V.device, R.rp[s], NULL);
    memset(&R, 0, sizeof(R));
}

static VkPipeline royale_pipeline(const RoyalePass *p, VkRenderPass pass)
{
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = p->vert_size;
    sm.pCode = p->vert;
    if (vkCreateShaderModule(V.device, &sm, NULL, &vert) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    sm.codeSize = p->frag_size;
    sm.pCode = p->frag;
    if (vkCreateShaderModule(V.device, &sm, NULL, &frag) != VK_SUCCESS)
    {
        vkDestroyShaderModule(V.device, vert, NULL);
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend = {0};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = R.layout;
    gp.renderPass = pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(V.device, VK_NULL_HANDLE, 1, &gp, NULL, &pipeline) != VK_SUCCESS)
        pipeline = VK_NULL_HANDLE;
    vkDestroyShaderModule(V.device, vert, NULL);
    vkDestroyShaderModule(V.device, frag, NULL);
    return pipeline;
}

/* The three phosphor masks, uploaded once. */
static bool royale_textures(void)
{
    VkBuffer staging[4] = {0};
    VkDeviceMemory staging_memory[4] = {0};
    VkCommandBuffer cb = VK_NULL_HANDLE;
    bool ok = false;
    VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = V.commands;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(V.device, &ca, &cb) != VK_SUCCESS)
        return false;
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);
    for (int t = 0; t < ROYALE_TEX_N; ++t)
    {
        const RoyaleTexture *tex = &ROYALE_TEXTURES[t];
        VkDeviceSize bytes = (VkDeviceSize)tex->width * tex->height * 4;
        VkMemoryRequirements req;
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = (VkExtent3D){tex->width, tex->height, 1};
        ii.mipLevels = ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (vkCreateImage(V.device, &ii, NULL, &R.tex_image[t]) != VK_SUCCESS)
            goto done;
        vkGetImageMemoryRequirements(V.device, R.tex_image[t], &req);
        if (!allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &R.tex_memory[t]) ||
            vkBindImageMemory(V.device, R.tex_image[t], R.tex_memory[t], 0) != VK_SUCCESS)
            goto done;
        VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = R.tex_image[t];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (vkCreateImageView(V.device, &view, NULL, &R.tex_view[t]) != VK_SUCCESS)
            goto done;
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        void *map = NULL;
        if (vkCreateBuffer(V.device, &bi, NULL, &staging[t]) != VK_SUCCESS)
            goto done;
        vkGetBufferMemoryRequirements(V.device, staging[t], &req);
        if (!allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &staging_memory[t]) ||
            vkBindBufferMemory(V.device, staging[t], staging_memory[t], 0) != VK_SUCCESS ||
            vkMapMemory(V.device, staging_memory[t], 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS)
            goto done;
        memcpy(map, tex->rgba, (size_t)bytes);
        barrier(cb, R.tex_image[t], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {0};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = ii.extent;
        vkCmdCopyBufferToImage(cb, staging[t], R.tex_image[t], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(cb, R.tex_image[t], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    vkEndCommandBuffer(cb);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    ok = vkQueueSubmit(V.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(V.queue) == VK_SUCCESS;
done:
    vkFreeCommandBuffers(V.device, V.commands, 1, &cb);
    for (int t = 0; t < 4; ++t)
    {
        if (staging[t])
            vkDestroyBuffer(V.device, staging[t], NULL);
        if (staging_memory[t])
            vkFreeMemory(V.device, staging_memory[t], NULL);
    }
    return ok;
}

static bool royale_init(void)
{
    R.tried = true;
    for (int s = 0; s < 2; ++s)
    {
        VkAttachmentDescription a = {0};
        a.format = s ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; /* every pass covers its whole image */
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub = {0};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep[2] = {{0}, {0}};
        /* the image may still be read by the previous frame's last pass */
        dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        /* and what is drawn is read by the passes after it */
        dep[1].srcSubpass = 0;
        dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1;
        rp.pAttachments = &a;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        rp.dependencyCount = 2;
        rp.pDependencies = dep;
        if (vkCreateRenderPass(V.device, &rp, NULL, &R.rp[s]) != VK_SUCCESS)
            return false;
    }

    /* binding 0: the parameters; 2 to 5: the pictures (1 is the push constants' in the .slang convention) */
    VkDescriptorSetLayoutBinding binding[5] = {{0}};
    binding[0].binding = 0;
    binding[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding[0].descriptorCount = 1;
    binding[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (int k = 1; k < 5; ++k)
    {
        binding[k].binding = (uint32_t)k + 1;
        binding[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding[k].descriptorCount = 1;
        binding[k].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dsl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl.bindingCount = 5;
    dsl.pBindings = binding;
    if (vkCreateDescriptorSetLayout(V.device, &dsl, NULL, &R.set_layout) != VK_SUCCESS)
        return false;
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &R.set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(V.device, &pl, NULL, &R.layout) != VK_SUCCESS)
        return false;

    static const VkSamplerAddressMode wraps[3] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                                                  VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                  VK_SAMPLER_ADDRESS_MODE_REPEAT};
    for (int linear = 0; linear < 2; ++linear)
        for (int w = 0; w < 3; ++w)
        {
            VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            si.magFilter = si.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            si.addressModeU = si.addressModeV = si.addressModeW = wraps[w];
            si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            if (vkCreateSampler(V.device, &si, NULL, &R.sampler[linear][w]) != VK_SUCCESS)
                return false;
        }

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAMES * 8},
                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FRAMES * 8 * 4}};
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = FRAMES * 8;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = ps;
    if (vkCreateDescriptorPool(V.device, &dp, NULL, &R.pool) != VK_SUCCESS)
        return false;
    VkDescriptorSetLayout layouts[FRAMES * 8];
    for (int k = 0; k < FRAMES * 8; ++k)
        layouts[k] = R.set_layout;
    VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = R.pool;
    da.descriptorSetCount = FRAMES * 8;
    da.pSetLayouts = layouts;
    VkDescriptorSet all[FRAMES * 8];
    if (vkAllocateDescriptorSets(V.device, &da, all) != VK_SUCCESS)
        return false;
    for (int f = 0; f < FRAMES; ++f)
        for (int i = 0; i < 8; ++i)
            R.sets[f][i] = all[f * 8 + i];

    /* the parameters: set once, from the defaults */
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = 256;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VkMemoryRequirements req;
    void *map = NULL;
    if (vkCreateBuffer(V.device, &bi, NULL, &R.ubo) != VK_SUCCESS)
        return false;
    vkGetBufferMemoryRequirements(V.device, R.ubo, &req);
    if (!allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &R.ubo_memory) ||
        vkBindBufferMemory(V.device, R.ubo, R.ubo_memory, 0) != VK_SUCCESS ||
        vkMapMemory(V.device, R.ubo_memory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS)
        return false;
    memset(map, 0, 256);
    memcpy(map, ROYALE_UBO, sizeof(ROYALE_UBO));

    if (!royale_textures())
        return false;
    for (int i = 0; i < ROYALE_N; ++i)
    {
        R.pipe[i] = royale_pipeline(&ROYALE_PASSES[i], i == ROYALE_N - 1 ? V.pass : R.rp[ROYALE_PASSES[i].srgb]);
        if (!R.pipe[i])
            return false;
    }
    return true;
}

static int royale_scale(int type, float scale, int source, int viewport)
{
    float v = type == ROYALE_SCALE_SOURCE ? source * scale : type == ROYALE_SCALE_VIEWPORT ? viewport * scale : scale;
    int n = (int)(v + 0.5f);
    return n < 1 ? 1 : n;
}

/* Sizes of every pass for a PS1 picture of native_w x native_h shown in a
 * viewport_w x viewport_h rectangle; the images are made again when they change. */
static bool royale_prepare(int native_w, int native_h, int viewport_w, int viewport_h)
{
    if (!R.tried)
    {
        R.ok = royale_init();
        if (!R.ok)
        {
            SwanStationPS5_log("vulkan: CRT Royale could not be set up");
            royale_free();
            R.tried = true;
        }
    }
    if (!R.ok)
        return false;
    R.native[0] = native_w;
    R.native[1] = native_h;
    int in_w = native_w, in_h = native_h;
    for (int i = 0; i < ROYALE_N; ++i)
    {
        const RoyalePass *p = &ROYALE_PASSES[i];
        int w = i == ROYALE_N - 1 ? viewport_w : royale_scale(p->scale_x_type, p->scale_x, in_w, viewport_w);
        int h = i == ROYALE_N - 1 ? viewport_h : royale_scale(p->scale_y_type, p->scale_y, in_h, viewport_h);
        R.size[i][0] = in_w = w;
        R.size[i][1] = in_h = h;
    }
    int key[4] = {native_w, native_h, viewport_w, viewport_h};
    if (R.have_images && memcmp(key, R.built, sizeof(key)) == 0)
        return true;
    vkDeviceWaitIdle(V.device);
    royale_free_images();
    for (int i = 0; i < ROYALE_N - 1; ++i)
    {
        const RoyalePass *p = &ROYALE_PASSES[i];
        VkFormat format = p->srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format;
        ii.extent = (VkExtent3D){(uint32_t)R.size[i][0], (uint32_t)R.size[i][1], 1};
        ii.mipLevels = ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VkMemoryRequirements req;
        bool ok = vkCreateImage(V.device, &ii, NULL, &R.out[i].image) == VK_SUCCESS;
        if (ok)
        {
            vkGetImageMemoryRequirements(V.device, R.out[i].image, &req);
            ok = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &R.out[i].memory) &&
                 vkBindImageMemory(V.device, R.out[i].image, R.out[i].memory, 0) == VK_SUCCESS;
        }
        if (ok)
        {
            VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = R.out[i].image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format;
            view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view.subresourceRange.levelCount = 1;
            view.subresourceRange.layerCount = 1;
            ok = vkCreateImageView(V.device, &view, NULL, &R.out[i].view) == VK_SUCCESS;
        }
        if (ok)
        {
            VkFramebufferCreateInfo fb = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb.renderPass = R.rp[p->srgb];
            fb.attachmentCount = 1;
            fb.pAttachments = &R.out[i].view;
            fb.width = (uint32_t)R.size[i][0];
            fb.height = (uint32_t)R.size[i][1];
            fb.layers = 1;
            ok = vkCreateFramebuffer(V.device, &fb, NULL, &R.out[i].fb) == VK_SUCCESS;
        }
        if (!ok)
        {
            SwanStationPS5_log("vulkan: CRT Royale: no memory for its images");
            royale_free_images();
            R.ok = false;
            return false;
        }
    }
    memcpy(R.built, key, sizeof(key));
    R.have_images = true;
    SwanStationPS5_log("vulkan: CRT Royale: PS1 %dx%d, viewport %dx%d", native_w, native_h, viewport_w, viewport_h);
    return true;
}

static void royale_size_vec(float *v, int w, int h)
{
    v[0] = (float)w;
    v[1] = (float)h;
    v[2] = 1.0f / (float)w;
    v[3] = 1.0f / (float)h;
}

/* A pass's push constants. */
static void royale_push(int i, uint8_t *data)
{
    const RoyalePass *p = &ROYALE_PASSES[i];
    memset(data, 0, 128);
    int in_w = i == 0 ? R.native[0] : R.size[i - 1][0], in_h = i == 0 ? R.native[1] : R.size[i - 1][1];
    for (int k = 0; k < p->push_count; ++k)
    {
        const RoyalePush *m = &p->push[k];
        float *v = (float *)(data + m->offset);
        switch (m->kind)
        {
        case ROYALE_PUSH_SOURCE_SIZE: royale_size_vec(v, in_w, in_h); break;
        case ROYALE_PUSH_ORIGINAL_SIZE: royale_size_vec(v, R.native[0], R.native[1]); break;
        case ROYALE_PUSH_OUTPUT_SIZE: royale_size_vec(v, R.size[i][0], R.size[i][1]); break;
        case ROYALE_PUSH_FINAL_SIZE: royale_size_vec(v, R.size[ROYALE_N - 1][0], R.size[ROYALE_N - 1][1]); break;
        case ROYALE_PUSH_FRAME: memcpy(v, &R.frames, sizeof(uint32_t)); break;
        case ROYALE_PUSH_PASS_SIZE: royale_size_vec(v, R.size[m->index][0], R.size[m->index][1]); break;
        case ROYALE_PUSH_TEXTURE_SIZE:
            royale_size_vec(v, ROYALE_TEXTURES[m->index].width, ROYALE_TEXTURES[m->index].height);
            break;
        case ROYALE_PUSH_PARAM: *v = m->value; break;
        default: break;
        }
    }
}

/* What a pass reads, written into this frame's descriptor set for it. */
static void royale_set(int f, int i)
{
    const RoyalePass *p = &ROYALE_PASSES[i];
    VkWriteDescriptorSet w[5];
    VkDescriptorImageInfo di[4];
    VkDescriptorBufferInfo bi = {R.ubo, 0, sizeof(ROYALE_UBO) <= 256 ? 256 : sizeof(ROYALE_UBO)};
    int n = 0;
    memset(w, 0, sizeof(w));
    w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[n].dstSet = R.sets[f][i];
    w[n].dstBinding = 0;
    w[n].descriptorCount = 1;
    w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[n].pBufferInfo = &bi;
    ++n;
    for (int s = 0; s < p->sampler_count && s < 4; ++s)
    {
        const RoyaleSampler *sm = &p->samplers[s];
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkSampler sampler = R.sampler[p->linear][p->wrap];
        switch (sm->kind)
        {
        case ROYALE_SAMPLER_SOURCE:
            if (i == 0)
            {
                view = V.game_view; /* the PS1's picture is made from the bigger one, smoothly */
                layout = V.game_layout;
                sampler = R.sampler[1][1];
            }
            else
                view = R.out[i - 1].view;
            break;
        case ROYALE_SAMPLER_ORIGINAL:
            view = V.game_view;
            layout = V.game_layout;
            sampler = R.sampler[1][1];
            break;
        case ROYALE_SAMPLER_PASS: view = R.out[sm->index].view; break;
        default:
            view = R.tex_view[sm->index];
            sampler = R.sampler[ROYALE_TEXTURES[sm->index].linear][ROYALE_TEXTURES[sm->index].wrap];
            break;
        }
        di[s] = (VkDescriptorImageInfo){sampler, view, layout};
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet = R.sets[f][i];
        w[n].dstBinding = sm->binding;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[n].pImageInfo = &di[s];
        ++n;
    }
    vkUpdateDescriptorSets(V.device, (uint32_t)n, w, 0, NULL);
}

static void royale_draw(VkCommandBuffer cb, int f, int i)
{
    uint8_t push[128];
    royale_push(i, push);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, R.pipe[i]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout, 0, 1, &R.sets[f][i], 0, NULL);
    vkCmdPushConstants(cb, R.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       ROYALE_PASSES[i].push_size, push);
    vkCmdDraw(cb, 4, 1, 0, 0);
}

/* Every pass but the last, each into its own image (outside the swapchain's render pass). */
static void royale_record(VkCommandBuffer cb, int f)
{
    for (int i = 0; i < ROYALE_N; ++i)
        royale_set(f, i);
    for (int i = 0; i < ROYALE_N - 1; ++i)
    {
        VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = R.rp[ROYALE_PASSES[i].srgb];
        rp.framebuffer = R.out[i].fb;
        rp.renderArea.extent = (VkExtent2D){(uint32_t)R.size[i][0], (uint32_t)R.size[i][1]};
        vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport = {0, 0, (float)R.size[i][0], (float)R.size[i][1], 0, 1};
        VkRect2D scissor = {{0, 0}, rp.renderArea.extent};
        vkCmdSetViewport(cb, 0, 1, &viewport);
        vkCmdSetScissor(cb, 0, 1, &scissor);
        royale_draw(cb, f, i);
        vkCmdEndRenderPass(cb);
    }
    ++R.frames;
}

/* The last pass, inside the swapchain's render pass: the game's rectangle. */
static void royale_final(VkCommandBuffer cb, int f, int x, int y, int w, int h)
{
    VkViewport viewport = {(float)x, (float)y, (float)w, (float)h, 0, 1};
    VkRect2D scissor = {{x, y}, {(uint32_t)w, (uint32_t)h}};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    royale_draw(cb, f, ROYALE_N - 1);
    viewport = (VkViewport){0, 0, (float)V.extent.width, (float)V.extent.height, 0, 1};
    scissor = (VkRect2D){{0, 0}, V.extent};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &scissor);
}

void vkp_present(const uint32_t *pixels, size_t pitch_bytes)
{
    if (!V.swapchain)
        return;
    int f = V.frame;
    V.frame = (V.frame + 1) % FRAMES;
    vkWaitForFences(V.device, 1, &V.done[f], VK_TRUE, UINT64_MAX);

    /* the CPU's canvas into this frame's staging buffer */
    const size_t row = (size_t)V.cw * 4;
    uint8_t *dst = V.staging_map[f];
    const uint8_t *src = (const uint8_t *)pixels;
    if (pitch_bytes == row)
        memcpy(dst, src, row * V.ch);
    else
        for (int y = 0; y < V.ch; ++y)
            memcpy(dst + row * y, src + pitch_bytes * y, row);

    uint32_t index;
    VkResult r = vkAcquireNextImageKHR(V.device, V.swapchain, UINT64_MAX, V.acquired[f], VK_NULL_HANDLE,
                                       &index);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
    {
        static int warned;
        if (!warned++)
            SwanStationPS5_log("vulkan: acquire failed (%d)", (int)r);
        return;
    }
    vkResetFences(V.device, 1, &V.done[f]);

    VkCommandBuffer cb = V.cmd[f];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);
    barrier(cb, V.canvas, V.canvas_ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {0};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = (VkExtent3D){(uint32_t)V.cw, (uint32_t)V.ch, 1};
    vkCmdCopyBufferToImage(cb, V.staging[f], V.canvas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(cb, V.canvas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    V.canvas_ready = true;

    bool game = V.game_shown && V.game_view;
    V.game_shown = false;
    if (game)
    {
        /* the core's rendering on this queue, finished before we sample it */
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
        VkDescriptorImageInfo di = {V.game_nearest ? V.sampler_nearest : V.sampler, V.game_view, V.game_layout};
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = V.game_sets[f];
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &di;
        vkUpdateDescriptorSets(V.device, 1, &w, 0, NULL);
    }

    /* CRT Royale: its passes first, each into an image, before the swapchain's render pass */
    int royale_rect[4] = {0, 0, 0, 0};
    bool royale = false;
    if (game && V.game_shader == SHADER_ROYALE && V.game_tex[0] >= 1.0f && V.game_tex[1] >= 1.0f)
    {
        float kx = (float)V.extent.width / V.cw, ky = (float)V.extent.height / V.ch;
        royale_rect[0] = (int)(V.game_rect[0] * kx + 0.5f);
        royale_rect[1] = (int)(V.game_rect[1] * ky + 0.5f);
        royale_rect[2] = (int)(V.game_rect[2] * kx + 0.5f);
        royale_rect[3] = (int)(V.game_rect[3] * ky + 0.5f);
        /* the PS1's own picture: its lines, and as many columns as the game's shape gives */
        int native_h = V.game_lines >= 1.0f ? (int)V.game_lines : 240;
        int native_w = (int)(V.game_tex[0] * native_h / V.game_tex[1] + 0.5f);
        if (native_w < 1)
            native_w = 1;
        if (royale_rect[2] >= 1 && royale_rect[3] >= 1 && royale_prepare(native_w, native_h, royale_rect[2], royale_rect[3]))
        {
            royale_record(cb, f);
            royale = true;
        }
    }

    VkClearValue clear = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
    VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = V.pass;
    rp.framebuffer = V.framebuffers[index];
    rp.renderArea.extent = V.extent;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport = {0, 0, (float)V.extent.width, (float)V.extent.height, 0, 1};
    VkRect2D scissor = {{0, 0}, V.extent};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    if (game)
    {
        /* the game picture, then the interface over it: the canvas is
         * transparent (premultiplied) where the game shows */
        float kx = (float)V.extent.width / V.cw, ky = (float)V.extent.height / V.ch;
        int sh = V.game_shader;
        /* the pipeline of each value: shaded[] holds sharp, CRT, LCD3x, supersampling; 2 is CRT Royale */
        static const int slot_of[6] = {-1, 2, -1, 1, 0, 3};
        int slot = sh >= 0 && sh <= 5 ? slot_of[sh] : -1;
        VkPipeline pipe = slot >= 0 && V.shaded[slot] ? V.shaded[slot] : V.opaque;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        /* sharp: screen pixels per texel; crt and lcd3x: the PS1's line count */
        float shown = 1.0f - 2.0f * V.game_crop;
        float info[4] = {V.game_tex[0], V.game_tex[1],
                         sh == 1 || sh == 3 ? V.game_lines * shown : V.game_rect[2] * kx / (V.game_tex[0] > 0 ? V.game_tex[0] : 1),
                         V.game_rect[3] * ky / (V.game_tex[1] * shown > 0 ? V.game_tex[1] * shown : 1)};
        if (royale)
            royale_final(cb, f, royale_rect[0], royale_rect[1], royale_rect[2], royale_rect[3]);
        else
            draw_quad(cb, V.game_sets[f], V.game_rect[0] * kx, V.game_rect[1] * ky, V.game_rect[2] * kx,
                      V.game_rect[3] * ky, V.game_crop, 1.0f - V.game_crop, info, colour_k);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.blended);
    }
    else
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.opaque);
    static const float none[4] = {0, 0, 0, 0}, plain[4] = {1, 1, 0, 0};
    draw_quad(cb, V.canvas_set, 0, 0, (float)V.extent.width, (float)V.extent.height, 0.0f, 1.0f, none, plain);
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &V.acquired[f];
    submit.pWaitDstStageMask = &wait;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &V.rendered[index];
    vkQueueSubmit(V.queue, 1, &submit, V.done[f]);

    VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &V.rendered[index];
    present.swapchainCount = 1;
    present.pSwapchains = &V.swapchain;
    present.pImageIndices = &index;
    vkQueuePresentKHR(V.queue, &present);
}

/* Everything made on the device, but not the device itself. */
static void destroy_device_objects(void)
{
    if (!V.device)
        return;
    vkDeviceWaitIdle(V.device);
    for (int f = 0; f < FRAMES; ++f)
    {
        if (V.done[f])
            vkDestroyFence(V.device, V.done[f], NULL);
        if (V.acquired[f])
            vkDestroySemaphore(V.device, V.acquired[f], NULL);
        if (V.staging[f])
            vkDestroyBuffer(V.device, V.staging[f], NULL);
        if (V.staging_memory[f])
            vkFreeMemory(V.device, V.staging_memory[f], NULL);
        V.done[f] = VK_NULL_HANDLE;
        V.acquired[f] = VK_NULL_HANDLE;
        V.staging[f] = VK_NULL_HANDLE;
        V.staging_memory[f] = VK_NULL_HANDLE;
        V.game_sets[f] = VK_NULL_HANDLE;
    }
    if (V.commands)
        vkDestroyCommandPool(V.device, V.commands, NULL);
    if (V.canvas_view)
        vkDestroyImageView(V.device, V.canvas_view, NULL);
    if (V.canvas)
        vkDestroyImage(V.device, V.canvas, NULL);
    if (V.canvas_memory)
        vkFreeMemory(V.device, V.canvas_memory, NULL);
    royale_free();
    if (V.pool)
        vkDestroyDescriptorPool(V.device, V.pool, NULL);
    if (V.sampler)
        vkDestroySampler(V.device, V.sampler, NULL);
    if (V.sampler_nearest)
        vkDestroySampler(V.device, V.sampler_nearest, NULL);
    if (V.opaque)
        vkDestroyPipeline(V.device, V.opaque, NULL);
    if (V.blended)
        vkDestroyPipeline(V.device, V.blended, NULL);
    for (int k = 0; k < 3; ++k)
        if (V.shaded[k])
            vkDestroyPipeline(V.device, V.shaded[k], NULL);
    V.shaded[0] = V.shaded[1] = V.shaded[2] = V.shaded[3] = VK_NULL_HANDLE;
    if (V.layout)
        vkDestroyPipelineLayout(V.device, V.layout, NULL);
    if (V.set_layout)
        vkDestroyDescriptorSetLayout(V.device, V.set_layout, NULL);
    for (uint32_t i = 0; i < V.image_count; ++i)
    {
        if (V.framebuffers[i])
            vkDestroyFramebuffer(V.device, V.framebuffers[i], NULL);
        if (V.views[i])
            vkDestroyImageView(V.device, V.views[i], NULL);
        if (V.rendered[i])
            vkDestroySemaphore(V.device, V.rendered[i], NULL);
        V.framebuffers[i] = VK_NULL_HANDLE;
        V.views[i] = VK_NULL_HANDLE;
        V.rendered[i] = VK_NULL_HANDLE;
    }
    if (V.pass)
        vkDestroyRenderPass(V.device, V.pass, NULL);
    if (V.swapchain)
        vkDestroySwapchainKHR(V.device, V.swapchain, NULL);
    V.commands = VK_NULL_HANDLE;
    V.canvas_view = VK_NULL_HANDLE;
    V.canvas = VK_NULL_HANDLE;
    V.canvas_memory = VK_NULL_HANDLE;
    V.pool = VK_NULL_HANDLE;
    V.sampler = VK_NULL_HANDLE;
    V.sampler_nearest = VK_NULL_HANDLE;
    V.opaque = V.blended = VK_NULL_HANDLE;
    V.layout = VK_NULL_HANDLE;
    V.set_layout = VK_NULL_HANDLE;
    V.pass = VK_NULL_HANDLE;
    V.swapchain = VK_NULL_HANDLE;
    V.image_count = 0;
    V.canvas_ready = false;
    V.game_view = VK_NULL_HANDLE;
    V.game_image = VK_NULL_HANDLE;
    V.frame = 0;
}

void vkp_close(void)
{
    destroy_device_objects();
    if (V.device)
        vkDestroyDevice(V.device, NULL);
    if (V.surface && vkDestroySurfaceKHR)
        vkDestroySurfaceKHR(V.instance, V.surface, NULL);
    if (V.instance && vkDestroyInstance)
        vkDestroyInstance(V.instance, NULL);
    memset(&V, 0, sizeof(V));
}

/* ---------------------------------------------------------------- a core's device */

void vkp_hw_context(VkInstance *instance, VkPhysicalDevice *gpu, VkSurfaceKHR *surface,
                    PFN_vkGetInstanceProcAddr *get_instance_proc_addr)
{
    *instance = V.instance;
    *gpu = V.gpu;
    *surface = V.surface;
    *get_instance_proc_addr = vkGetInstanceProcAddr;
}

void vkp_hw_device(VkDevice *device, VkQueue *queue, uint32_t *family,
                   PFN_vkGetDeviceProcAddr *get_device_proc_addr)
{
    *device = V.device;
    *queue = V.queue;
    *family = V.family;
    *get_device_proc_addr = vkGetDeviceProcAddr;
}

bool vkp_adopt_device(VkDevice device, VkQueue queue, uint32_t family, char *error, size_t size)
{
    if (!V.swapchain || device == V.device)
        return V.swapchain != VK_NULL_HANDLE;
    destroy_device_objects();
    vkDestroyDevice(V.device, NULL);
    V.device = device;
    V.queue = queue;
    V.family = family;
    bool ok = load_device(error, size) && create_swapchain(error, size) &&
              create_pipeline(error, size) && create_canvas(error, size);
    SwanStationPS5_log("vulkan: the screen moved to the core's device%s%s", ok ? "" : ": ", ok ? "" : error);
    return ok;
}

void vkp_set_game_image(VkImage image, VkImageView view, VkImageLayout layout)
{
    V.game_image = image;
    V.game_view = view;
    V.game_layout = layout;
}

uint32_t vkp_sync_index(void)
{
    return (uint32_t)V.frame;
}

uint32_t vkp_sync_index_mask(void)
{
    return (1u << FRAMES) - 1;
}

void vkp_wait_sync_index(void)
{
    if (V.device && V.done[V.frame])
        vkWaitForFences(V.device, 1, &V.done[V.frame], VK_TRUE, UINT64_MAX);
}

bool vkp_game_image_ready(void)
{
    return V.game_view != VK_NULL_HANDLE;
}

/* The game picture scaled to w x h RGBA, for save-state thumbnails and the
 * quick-resume picture: a linear blit into a small image, copied into a
 * mapped buffer. Rare, so it simply waits for the GPU. */
bool vkp_capture_game(uint8_t *rgba, int w, int h, int src_w, int src_h)
{
    if (!V.device || !V.game_image || w <= 0 || h <= 0 || src_w <= 0 || src_h <= 0)
        return false;
    VkImage small = VK_NULL_HANDLE;
    VkDeviceMemory small_mem = VK_NULL_HANDLE, buf_mem = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    bool ok = false;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = (VkExtent3D){(uint32_t)w, (uint32_t)h, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkMemoryRequirements req;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = (VkDeviceSize)w * h * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = V.commands;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    if (vkCreateImage(V.device, &ii, NULL, &small) != VK_SUCCESS)
        goto done;
    vkGetImageMemoryRequirements(V.device, small, &req);
    if (!allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &small_mem) ||
        vkBindImageMemory(V.device, small, small_mem, 0) != VK_SUCCESS)
        goto done;
    if (vkCreateBuffer(V.device, &bi, NULL, &buf) != VK_SUCCESS)
        goto done;
    vkGetBufferMemoryRequirements(V.device, buf, &req);
    if (!allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &buf_mem) ||
        vkBindBufferMemory(V.device, buf, buf_mem, 0) != VK_SUCCESS)
        goto done;
    if (vkAllocateCommandBuffers(V.device, &ca, &cb) != VK_SUCCESS)
        goto done;
    vkDeviceWaitIdle(V.device);
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);
    barrier(cb, V.game_image, V.game_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(cb, small, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit blit = {0};
    blit.srcSubresource.aspectMask = blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = blit.dstSubresource.layerCount = 1;
    blit.srcOffsets[1] = (VkOffset3D){src_w, src_h, 1};
    blit.dstOffsets[1] = (VkOffset3D){w, h, 1};
    vkCmdBlitImage(cb, V.game_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, small,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    barrier(cb, V.game_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.game_layout, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    barrier(cb, small, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {0};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = ii.extent;
    vkCmdCopyImageToBuffer(cb, small, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
    vkEndCommandBuffer(cb);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    if (vkQueueSubmit(V.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(V.queue) == VK_SUCCESS)
    {
        void *map = NULL;
        if (vkMapMemory(V.device, buf_mem, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS)
        {
            memcpy(rgba, map, (size_t)w * h * 4);
            for (size_t i = 0; i < (size_t)w * h; ++i)
                rgba[i * 4 + 3] = 255; /* the PS1 mask bit isn't transparency */
            ok = true;
        }
    }
done:
    if (cb)
        vkFreeCommandBuffers(V.device, V.commands, 1, &cb);
    if (buf)
        vkDestroyBuffer(V.device, buf, NULL);
    if (buf_mem)
        vkFreeMemory(V.device, buf_mem, NULL);
    if (small)
        vkDestroyImage(V.device, small, NULL);
    if (small_mem)
        vkFreeMemory(V.device, small_mem, NULL);
    return ok;
}

void vkp_set_game_nearest(bool nearest)
{
    V.game_nearest = nearest;
}

void vkp_show_game(float x, float y, float w, float h, float crop, int shader, int tex_w, int tex_h, int lines)
{
    V.game_shown = true;
    V.game_crop = crop;
    V.game_shader = shader;
    V.game_tex[0] = (float)tex_w;
    V.game_tex[1] = (float)tex_h;
    V.game_lines = (float)lines;
    V.game_rect[0] = x;
    V.game_rect[1] = y;
    V.game_rect[2] = w;
    V.game_rect[3] = h;
}

#else

bool vkp_open(int canvas_w, int canvas_h, char *error, size_t size)
{
    (void)canvas_w;
    (void)canvas_h;
    snprintf(error, size, "built without Vulkan");
    return false;
}
void vkp_present(const uint32_t *pixels, size_t pitch_bytes)
{
    (void)pixels;
    (void)pitch_bytes;
}
void vkp_close(void) {}
const char *vkp_describe(void)
{
    return "";
}
bool vkp_game_image_ready(void)
{
    return false;
}
void vkp_set_colour(float brightness, float saturation, float warmth, float sharpen)
{
    (void)brightness, (void)saturation, (void)warmth, (void)sharpen;
}
void vkp_set_game_nearest(bool nearest)
{
    (void)nearest;
}
void vkp_show_game(float x, float y, float w, float h, float crop, int shader, int tex_w, int tex_h, int lines)
{
    (void)x, (void)y, (void)w, (void)h, (void)crop, (void)shader, (void)tex_w, (void)tex_h, (void)lines;
}

#endif
