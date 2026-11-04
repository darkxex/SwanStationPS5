/*
 * SwanStationPS5 v2 - the screen through Vulkan on the PC (SDL_Vulkan surface and swapchain).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The PC build compiles this file where the PS5 build compiles vk_present.c: it is the same presenter (the canvas,
 * the GPU interface, the game's picture, the shaders), with the window system part (vk_present_desktop.inc) in
 * place of VK_KHR_display. vk_present.h is the interface both share.
 */
#define SS5_VKP_DESKTOP 1
#include "vk_present.c"

/* false: skip this frame (minimised, or the swapchain could not be made again) */
static bool desktop_swapchain_ready(void)
{
    uint32_t w, h;
    desktop_drawable(&w, &h);
    if (w == 0 || h == 0)
        return false; /* minimised */
    if (w == V.extent.width && h == V.extent.height)
        return true;
    char error[200];
    SwanStationPS5_log("vulkan: window now %ux%u (was %ux%u), swapchain made again", w, h, V.extent.width,
                       V.extent.height);
    destroy_device_objects();
    bool ok = create_swapchain(error, sizeof(error)) && create_pipeline(error, sizeof(error)) &&
              create_canvas(error, sizeof(error)) && (!U.enabled || ui_create(error, sizeof(error)));
    if (!ok)
        SwanStationPS5_log("vulkan: the swapchain could not be made again: %s", error);
    return ok;
}

/* Frame generation (FSR 3's interpolator, third_party/framegen) is built for the PS5 only: here it is "not available",
 * and the presenter shows the game's own frames. */
FgInterpolator *ssfg_create(PFN_vkVoidFunction (*get_instance_proc)(VkInstance, const char *), VkInstance instance,
                            VkPhysicalDevice gpu, VkDevice device, uint32_t width, uint32_t height, VkFormat format,
                            char *error, size_t error_size)
{
    (void)get_instance_proc, (void)instance, (void)gpu, (void)device, (void)width, (void)height, (void)format;
    snprintf(error, error_size, "frame generation is not built for the PC");
    return NULL;
}
void ssfg_destroy(FgInterpolator *fg) { (void)fg; }
void ssfg_prepare(FgInterpolator *fg, VkCommandBuffer cmd) { (void)fg, (void)cmd; }
VkImage ssfg_frame_image(FgInterpolator *fg) { (void)fg; return VK_NULL_HANDLE; }
VkImageView ssfg_frame_view(FgInterpolator *fg) { (void)fg; return VK_NULL_HANDLE; }
bool ssfg_record(FgInterpolator *fg, VkCommandBuffer cmd, bool reset) { (void)fg, (void)cmd, (void)reset; return false; }
VkImageView ssfg_output_view(FgInterpolator *fg) { (void)fg; return VK_NULL_HANDLE; }
bool ssfg_record_n(FgInterpolator *fg, VkCommandBuffer cmd, bool reset, uint32_t generated)
{
    (void)fg, (void)cmd, (void)reset, (void)generated;
    return false;
}
VkImageView ssfg_generated_view(FgInterpolator *fg, uint32_t i) { (void)fg, (void)i; return VK_NULL_HANDLE; }
void ssfg_advance(FgInterpolator *fg) { (void)fg; }

FgPacing *ssfg_pacing_create(void) { return NULL; }
void ssfg_pacing_destroy(FgPacing *pacing) { (void)pacing; }
FgDecision ssfg_pacing_frame(FgPacing *pacing, uint32_t vsyncs, uint32_t already, double core_hz, double display_hz,
                             double now, double speed, bool nominal, uint32_t max_generated)
{
    (void)pacing, (void)vsyncs, (void)already, (void)core_hz, (void)display_hz, (void)now, (void)speed, (void)nominal,
        (void)max_generated;
    FgDecision none = {0};
    return none;
}
const char *ssfg_pacing_state_name(int state) { (void)state; return "off"; }
const char *ssfg_pacing_event(FgPacing *pacing) { (void)pacing; return NULL; }
