// SwanStationPS5 - frame generation for the presenter (fg_bridge.h): PS5SX2's interpolator
// (third_party/framegen/ps5_framegen.cpp) behind a C interface, its Vulkan
// commands looked up from SwanStationPS5's device.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../../src/platform/vk/fg_bridge.h"
#include "OrbisFrameGenPacing.h"
#include "ps5_framegen.h"

#include <cstdio>
#include <new>
#include <string>

#define FG_VK_DEFINE(name) PFN_##name name;
FG_VK_INSTANCE_FUNCS(FG_VK_DEFINE)
FG_VK_DEVICE_FUNCS(FG_VK_DEFINE)
#undef FG_VK_DEFINE

struct FgInterpolator
{
    ps5::framegen::interpolator fg;
};

extern "C" FgInterpolator *ssfg_create(PFN_vkVoidFunction (*get_instance_proc)(VkInstance, const char *),
                                           VkInstance instance, VkPhysicalDevice gpu, VkDevice device, uint32_t width,
                                           uint32_t height, VkFormat format, char *error, size_t error_size)
{
    bool loaded = true;
#define FG_VK_LOAD_INSTANCE(name)                                                                  \
    name = reinterpret_cast<PFN_##name>(get_instance_proc(instance, #name));                       \
    loaded = loaded && name;
    FG_VK_INSTANCE_FUNCS(FG_VK_LOAD_INSTANCE)
#undef FG_VK_LOAD_INSTANCE
#define FG_VK_LOAD_DEVICE(name)                                                                    \
    name = vkGetDeviceProcAddr ? reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name)) : nullptr; \
    loaded = loaded && name;
    FG_VK_DEVICE_FUNCS(FG_VK_LOAD_DEVICE)
#undef FG_VK_LOAD_DEVICE
    if (!loaded)
    {
        std::snprintf(error, error_size, "a Vulkan command is missing");
        return nullptr;
    }
    FgInterpolator *fg = new (std::nothrow) FgInterpolator;
    if (!fg)
    {
        std::snprintf(error, error_size, "no memory");
        return nullptr;
    }
    std::string why;
    if (!fg->fg.create(gpu, device, width, height, why, format))
    {
        std::snprintf(error, error_size, "%s", why.c_str());
        delete fg;
        return nullptr;
    }
    return fg;
}

extern "C" void ssfg_destroy(FgInterpolator *fg)
{
    if (fg)
        fg->fg.destroy();
    delete fg;
}

extern "C" void ssfg_prepare(FgInterpolator *fg, VkCommandBuffer cmd)
{
    fg->fg.prepare(cmd);
}

extern "C" VkImage ssfg_frame_image(FgInterpolator *fg)
{
    return fg->fg.frame_image();
}

extern "C" VkImageView ssfg_frame_view(FgInterpolator *fg)
{
    return fg->fg.frame_view();
}

extern "C" bool ssfg_record(FgInterpolator *fg, VkCommandBuffer cmd, bool reset)
{
    return fg->fg.record(cmd, reset);
}

extern "C" VkImageView ssfg_output_view(FgInterpolator *fg)
{
    return fg->fg.output_view();
}

extern "C" void ssfg_advance(FgInterpolator *fg)
{
    fg->fg.advance();
}

struct FgPacing
{
    orbis_fg::Pacing pacing;
};

extern "C" FgPacing *ssfg_pacing_create(void)
{
    return new (std::nothrow) FgPacing;
}

extern "C" void ssfg_pacing_destroy(FgPacing *pacing)
{
    delete pacing;
}

extern "C" FgDecision ssfg_pacing_frame(FgPacing *pacing, uint32_t vsyncs, uint32_t already, double core_hz,
                                        double display_hz, double now, double speed, bool nominal)
{
    const orbis_fg::Decision d = pacing->pacing.Frame(vsyncs, already, core_hz, display_hz, now, speed, nominal);
    FgDecision out;
    out.engaged = d.engaged;
    out.reset = d.reset;
    out.presents = d.presents;
    out.state = static_cast<int>(pacing->pacing.GetState());
    out.vsyncs = pacing->pacing.MedianVsyncs();
    out.refreshes = pacing->pacing.Refreshes();
    return out;
}

extern "C" const char *ssfg_pacing_state_name(int state)
{
    static const char *const names[] = {"warming up", "no room (the game's frames fill every refresh)",
                                        "unsteady (the game's frames are not of one length)",
                                        "not at normal speed", "paused (the game slowed)", "generating"};
    return state >= 0 && state < 6 ? names[state] : "?";
}

extern "C" const char *ssfg_pacing_event(FgPacing *pacing)
{
    switch (pacing->pacing.TakeEvent())
    {
        case orbis_fg::Event::Paused: return "the game slowed while frames were generated: none for a while";
        case orbis_fg::Event::ResumedCostly: return "the game ran at full speed without generated frames: the next pause is longer";
        case orbis_fg::Event::ResumedSlowGame: return "the game is as slow without generated frames: generating again";
        default: return nullptr;
    }
}
