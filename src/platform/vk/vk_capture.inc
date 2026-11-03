/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Queue submission and fence polling stay on the renderer's thread. */
static struct
{
    VkImage image;
    VkBuffer buffer;
    VkDeviceMemory image_memory, buffer_memory;
    VkCommandBuffer command;
    VkFence fence;
    void *map;
    int w, h;
    bool pending;
} capture;

/* The owning device is idle, or no capture is in flight. */
static void capture_destroy(void)
{
    if (capture.command)
        vkFreeCommandBuffers(V.device, V.commands, 1, &capture.command);
    if (capture.fence)
        vkDestroyFence(V.device, capture.fence, NULL);
    if (capture.buffer)
        vkDestroyBuffer(V.device, capture.buffer, NULL);
    if (capture.buffer_memory)
        vkFreeMemory(V.device, capture.buffer_memory, NULL);
    if (capture.image)
        vkDestroyImage(V.device, capture.image, NULL);
    if (capture.image_memory)
        vkFreeMemory(V.device, capture.image_memory, NULL);
    memset(&capture, 0, sizeof(capture));
}

static bool capture_create(int w, int h)
{
    if (capture.w == w && capture.h == h)
        return true;
    capture_destroy();
    VkImageCreateInfo image = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = (VkExtent3D){(uint32_t)w, (uint32_t)h, 1};
    image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkBufferCreateInfo buffer = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = (VkDeviceSize)w * h * 4;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkCommandBufferAllocateInfo command = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command.commandPool = V.commands;
    command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command.commandBufferCount = 1;
    VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkMemoryRequirements req;
    if (vkCreateImage(V.device, &image, NULL, &capture.image) != VK_SUCCESS)
        goto fail;
    vkGetImageMemoryRequirements(V.device, capture.image, &req);
    if (!allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &capture.image_memory) ||
        vkBindImageMemory(V.device, capture.image, capture.image_memory, 0) != VK_SUCCESS ||
        vkCreateBuffer(V.device, &buffer, NULL, &capture.buffer) != VK_SUCCESS)
        goto fail;
    vkGetBufferMemoryRequirements(V.device, capture.buffer, &req);
    if (!allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  &capture.buffer_memory) ||
        vkBindBufferMemory(V.device, capture.buffer, capture.buffer_memory, 0) != VK_SUCCESS ||
        vkMapMemory(V.device, capture.buffer_memory, 0, VK_WHOLE_SIZE, 0, &capture.map) != VK_SUCCESS ||
        vkAllocateCommandBuffers(V.device, &command, &capture.command) != VK_SUCCESS ||
        vkCreateFence(V.device, &fence, NULL, &capture.fence) != VK_SUCCESS)
        goto fail;
    capture.w = w;
    capture.h = h;
    return true;
fail:
    capture_destroy();
    return false;
}

bool vkp_capture_begin(int w, int h, int src_w, int src_h)
{
    if (!V.device || !V.commands || !V.game_image || capture.pending ||
        w <= 0 || h <= 0 || src_w <= 0 || src_h <= 0 || !capture_create(w, h))
        return false;
    if (vkResetCommandBuffer(capture.command, 0) != VK_SUCCESS ||
        vkResetFences(V.device, 1, &capture.fence) != VK_SUCCESS)
        return false;
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkCommandBuffer cb = capture.command;
    if (vkBeginCommandBuffer(cb, &begin) != VK_SUCCESS)
        return false;
    barrier(cb, V.game_image, V.game_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(cb, capture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit blit = {0};
    blit.srcSubresource.aspectMask = blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = blit.dstSubresource.layerCount = 1;
    blit.srcOffsets[1] = (VkOffset3D){src_w, src_h, 1};
    blit.dstOffsets[1] = (VkOffset3D){w, h, 1};
    vkCmdBlitImage(cb, V.game_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    barrier(cb, V.game_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.game_layout, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    barrier(cb, capture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {0};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = (VkExtent3D){(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyImageToBuffer(cb, capture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture.buffer, 1, &copy);
    VkMemoryBarrier host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         1, &host, 0, NULL, 0, NULL);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS)
        return false;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    if (vkQueueSubmit(V.queue, 1, &submit, capture.fence) != VK_SUCCESS)
        return false;
    capture.pending = true;
    return true;
}

int vkp_capture_poll(uint8_t *rgba, int w, int h, bool wait)
{
    if (!capture.pending)
        return -1;
    VkResult result = wait ? vkWaitForFences(V.device, 1, &capture.fence, VK_TRUE, UINT64_MAX)
                           : vkGetFenceStatus(V.device, capture.fence);
    if (!wait && result == VK_NOT_READY)
        return 0;
    /* A failed wait does not establish that these resources can be reused. */
    if (result != VK_SUCCESS)
        return -1;
    capture.pending = false;
    if (capture.w != w || capture.h != h)
        return -1;
    memcpy(rgba, capture.map, (size_t)w * h * 4);
    return 1;
}
