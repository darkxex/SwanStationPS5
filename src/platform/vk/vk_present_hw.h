/*
 * SwanStationPS5 v2 - the presenter's side of libretro's Vulkan interface (host.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_VK_PRESENT_HW_H
#define SwanStationPS5_VK_PRESENT_HW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

/* What a core needs to create its device (negotiation's create_device). */
void vkp_hw_context(VkInstance *instance, VkPhysicalDevice *gpu, VkSurfaceKHR *surface,
                    PFN_vkGetInstanceProcAddr *get_instance_proc_addr);
void vkp_hw_device(VkDevice *device, VkQueue *queue, uint32_t *family,
                   PFN_vkGetDeviceProcAddr *get_device_proc_addr);
/* Moves the screen onto the core's device; the old one is destroyed and
 * this one is owned by the presenter from now on. */
bool vkp_adopt_device(VkDevice device, VkQueue queue, uint32_t family, char *error, size_t size);
/* retro_hw_render_interface_vulkan */
void vkp_set_game_image(VkImage image, VkImageView view, VkImageLayout layout); /* VK_NULL_HANDLE: none */
/* The game picture (src_w x src_h of it) scaled to w x h RGBA; waits for the GPU. */
bool vkp_capture_game(uint8_t *rgba, int w, int h, int src_w, int src_h);
uint32_t vkp_sync_index(void);
uint32_t vkp_sync_index_mask(void);
void vkp_wait_sync_index(void);

#endif
