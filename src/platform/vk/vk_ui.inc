/*
 * SwanStationPS5 - the interface drawn by the GPU (included by vk_present.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The interface code draws through a few platform calls (plat_draw_texture, plat_draw_mesh, plat_fill_rect...).
 * With this layer those calls add textured, tinted triangles to a list instead of painting into the CPU canvas;
 * at the end of the frame the list is drawn over the game picture in one pass, grouped by texture and clip.
 *
 * Textures keep their pixels in memory and go to the GPU when first drawn (and again after the screen moves to
 * another device: the core's), with a mip chain so covers shrink without shimmering. A texture that is freed
 * is destroyed a few frames later, when no frame in flight can still read it.
 */

#define UI_MAX_VERTS (1u << 18)
#define UI_MAX_BATCHES 8192
#define UI_GARBAGE 1024
#define UI_MAX_LEVELS 12

typedef struct
{
    float x, y, u, v;
    uint32_t abgr; /* R, G, B, A bytes */
} UiVertex;

typedef struct
{
    uint8_t *rgba;
    bool owned; /* rgba is ours to free; else the platform keeps it for as long as the texture lives */
    int w, h;
    bool smooth, mips;
    uint32_t gen; /* U.gen when the GPU objects were made */
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet set;
} UiTex;

typedef struct
{
    VkDescriptorSet set; /* the texture as it was when drawn: a texture freed later in the frame still draws */
    bool opaque;         /* drawn without blending: the texture's alpha is ignored */
    int clip[4];         /* x, y, w, h in canvas pixels; w <= 0: none */
    uint32_t first, count;
} UiBatch;

typedef struct
{
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet set;
    uint32_t stamp;
} UiGarbage;

static struct
{
    bool enabled; /* asked for (the flag file) and the pipeline was made once */
    bool ready;   /* the objects below exist on the current device */
    uint32_t gen; /* changes with the device */
    VkPipelineLayout layout;
    VkPipeline pipeline, pipeline_opaque;
    bool opaque; /* the mode the next draws are in */
    VkDescriptorPool pool;
    VkSampler smooth, nearest;
    VkBuffer vbuf[FRAMES];
    VkDeviceMemory vmem[FRAMES];
    void *vmap[FRAMES];
    UiTex **tex;
    int tex_cap;
    UiVertex *verts;
    uint32_t nverts;
    UiBatch *batches;
    int nbatches;
    int clip[4];
    float clear[4];
    int game_at; /* the batches before this one are drawn under the game's picture, the rest over it; -1: no picture */
    bool force_new;
    int white;
    bool warned;
    UiGarbage garbage[UI_GARBAGE];
    int ngarbage;
    uint32_t stamp; /* presents so far */
} U = {.white = -1};

static void ui_destroy_handles(VkImage image, VkDeviceMemory memory, VkImageView view, VkDescriptorSet set,
                               bool pool_alive)
{
    if (set && pool_alive && U.pool)
        vkFreeDescriptorSets(V.device, U.pool, 1, &set);
    if (view)
        vkDestroyImageView(V.device, view, NULL);
    if (image)
        vkDestroyImage(V.device, image, NULL);
    if (memory)
        vkFreeMemory(V.device, memory, NULL);
}

/* The GPU side of a texture goes when no frame in flight reads it any more. */
static void ui_gpu_release(UiTex *t)
{
    if (!t->image && !t->view && !t->memory)
        return;
    if (t->gen == U.gen && U.ready && V.device)
    {
        if (U.ngarbage >= UI_GARBAGE)
        {
            vkDeviceWaitIdle(V.device);
            ui_destroy_handles(t->image, t->memory, t->view, t->set, true);
        }
        else
        {
            UiGarbage *g = &U.garbage[U.ngarbage++];
            g->image = t->image;
            g->memory = t->memory;
            g->view = t->view;
            g->set = t->set;
            g->stamp = U.stamp;
        }
    }
    t->image = VK_NULL_HANDLE;
    t->memory = VK_NULL_HANDLE;
    t->view = VK_NULL_HANDLE;
    t->set = VK_NULL_HANDLE;
}

static void ui_collect(bool all)
{
    int kept = 0;
    for (int i = 0; i < U.ngarbage; ++i)
    {
        UiGarbage g = U.garbage[i];
        if (all || U.stamp - g.stamp > FRAMES + 1)
            ui_destroy_handles(g.image, g.memory, g.view, g.set, !all);
        else
            U.garbage[kept++] = g;
    }
    U.ngarbage = kept;
}

static bool ui_upload(UiTex *t)
{
    const int w = t->w, h = t->h;
    uint32_t levels = 1;
    if (t->smooth && t->mips)
        while (levels < UI_MAX_LEVELS && ((w >> levels) > 0 || (h >> levels) > 0))
            ++levels;
    int lw[UI_MAX_LEVELS], lh[UI_MAX_LEVELS];
    for (uint32_t l = 0; l < levels; ++l)
    {
        lw[l] = (w >> l) > 0 ? w >> l : 1;
        lh[l] = (h >> l) > 0 ? h >> l : 1;
    }
    const size_t total = (size_t)w * h * 4; /* the staging buffer holds the picture; the GPU makes the smaller levels */

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void *map = NULL;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = total;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkMemoryRequirements req;
    bool ok = vkCreateBuffer(V.device, &bi, NULL, &staging) == VK_SUCCESS;
    if (ok)
    {
        vkGetBufferMemoryRequirements(V.device, staging, &req);
        ok = allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &staging_memory) &&
             vkBindBufferMemory(V.device, staging, staging_memory, 0) == VK_SUCCESS &&
             vkMapMemory(V.device, staging_memory, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS;
    }
    if (ok)
    {
        memcpy(map, t->rgba, (size_t)w * h * 4);
    }

    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = (VkExtent3D){(uint32_t)w, (uint32_t)h, 1};
    ii.mipLevels = levels;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               (levels > 1 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ok = ok && vkCreateImage(V.device, &ii, NULL, &t->image) == VK_SUCCESS;
    if (ok)
    {
        vkGetImageMemoryRequirements(V.device, t->image, &req);
        ok = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &t->memory) &&
             vkBindImageMemory(V.device, t->image, t->memory, 0) == VK_SUCCESS;
    }
    if (ok)
    {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t->image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = levels;
        vi.subresourceRange.layerCount = 1;
        ok = vkCreateImageView(V.device, &vi, NULL, &t->view) == VK_SUCCESS;
    }
    if (ok)
    {
        VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = U.pool;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &V.set_layout;
        ok = vkAllocateDescriptorSets(V.device, &da, &t->set) == VK_SUCCESS;
    }
    if (ok)
    {
        VkCommandBuffer cb;
        VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = V.commands;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        ok = vkAllocateCommandBuffers(V.device, &ca, &cb) == VK_SUCCESS;
        if (ok)
        {
            VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cb, &begin);
            VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = t->image;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.levelCount = levels;
            b.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                                 NULL, 1, &b);
            VkBufferImageCopy copy = {0};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = (VkExtent3D){(uint32_t)w, (uint32_t)h, 1};
            vkCmdCopyBufferToImage(cb, staging, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            /* the smaller levels, each a filtered blit of the one above (on the GPU: no CPU pass over the picture) */
            for (uint32_t l = 1; l < levels; ++l)
            {
                VkImageMemoryBarrier up = b;
                up.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                up.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                up.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                up.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                up.subresourceRange.baseMipLevel = l - 1;
                up.subresourceRange.levelCount = 1;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                                     NULL, 1, &up);
                VkImageBlit blit = {0};
                blit.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1};
                blit.srcOffsets[1] = (VkOffset3D){lw[l - 1], lh[l - 1], 1};
                blit.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
                blit.dstOffsets[1] = (VkOffset3D){lw[l], lh[l], 1};
                vkCmdBlitImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            }
            /* to the shader: the levels above the last are in the source layout, the last in the destination one */
            if (levels > 1)
            {
                VkImageMemoryBarrier rest = b;
                rest.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                rest.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                rest.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                rest.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                rest.subresourceRange.baseMipLevel = 0;
                rest.subresourceRange.levelCount = levels - 1;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                     NULL, 0, NULL, 1, &rest);
            }
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.subresourceRange.baseMipLevel = levels - 1;
            b.subresourceRange.levelCount = 1;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                 NULL, 0, NULL, 1, &b);
            vkEndCommandBuffer(cb);
            VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cb;
            ok = vkQueueSubmit(V.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
            if (ok)
                vkQueueWaitIdle(V.queue);
            vkFreeCommandBuffers(V.device, V.commands, 1, &cb);
        }
    }
    if (staging)
        vkDestroyBuffer(V.device, staging, NULL);
    if (staging_memory)
        vkFreeMemory(V.device, staging_memory, NULL);
    if (!ok)
    {
        ui_destroy_handles(t->image, t->memory, t->view, t->set, true);
        t->image = VK_NULL_HANDLE;
        t->memory = VK_NULL_HANDLE;
        t->view = VK_NULL_HANDLE;
        t->set = VK_NULL_HANDLE;
        return false;
    }
    VkDescriptorImageInfo di = {t->smooth ? U.smooth : U.nearest, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = t->set;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &di;
    vkUpdateDescriptorSets(V.device, 1, &wr, 0, NULL);
    t->gen = U.gen;
    return true;
}

static bool ui_texture_ready(UiTex *t)
{
    if (t->image && t->gen == U.gen)
        return true;
    return ui_upload(t);
}

/* ---------------------------------------------------------------- the device's objects */

static void ui_destroy(void)
{
    if (!U.ready && !U.pipeline && !U.pool)
        return;
    /* the caller has waited for the device to be idle */
    for (int i = 0; i < U.tex_cap; ++i)
    {
        UiTex *t = U.tex[i];
        if (!t)
            continue;
        ui_destroy_handles(t->image, t->memory, t->view, VK_NULL_HANDLE, false);
        t->image = VK_NULL_HANDLE;
        t->memory = VK_NULL_HANDLE;
        t->view = VK_NULL_HANDLE;
        t->set = VK_NULL_HANDLE;
    }
    ui_collect(true);
    /* a frame being drawn when the device changes (a game starting) keeps nothing of the old one's textures */
    U.nverts = 0;
    U.nbatches = 0;
    U.game_at = -1;
    for (int f = 0; f < FRAMES; ++f)
    {
        if (U.vbuf[f])
            vkDestroyBuffer(V.device, U.vbuf[f], NULL);
        if (U.vmem[f])
            vkFreeMemory(V.device, U.vmem[f], NULL);
        U.vbuf[f] = VK_NULL_HANDLE;
        U.vmem[f] = VK_NULL_HANDLE;
        U.vmap[f] = NULL;
    }
    if (U.pipeline)
        vkDestroyPipeline(V.device, U.pipeline, NULL);
    if (U.pipeline_opaque)
        vkDestroyPipeline(V.device, U.pipeline_opaque, NULL);
    U.pipeline_opaque = VK_NULL_HANDLE;
    if (U.layout)
        vkDestroyPipelineLayout(V.device, U.layout, NULL);
    if (U.smooth)
        vkDestroySampler(V.device, U.smooth, NULL);
    if (U.nearest)
        vkDestroySampler(V.device, U.nearest, NULL);
    if (U.pool)
        vkDestroyDescriptorPool(V.device, U.pool, NULL);
    U.pipeline = VK_NULL_HANDLE;
    U.layout = VK_NULL_HANDLE;
    U.smooth = U.nearest = VK_NULL_HANDLE;
    U.pool = VK_NULL_HANDLE;
    U.ready = false;
    ++U.gen;
}

static bool ui_create(char *error, size_t size)
{
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &V.set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    CHECK(vkCreatePipelineLayout(V.device, &pl, NULL, &U.layout), "interface pipeline layout");

    VkShaderModule vert, frag;
    VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof(SPV_UI_VERT);
    sm.pCode = SPV_UI_VERT;
    CHECK(vkCreateShaderModule(V.device, &sm, NULL, &vert), "interface vertex shader");
    sm.codeSize = sizeof(SPV_UI_FRAG);
    sm.pCode = SPV_UI_FRAG;
    CHECK(vkCreateShaderModule(V.device, &sm, NULL, &frag), "interface fragment shader");
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    VkVertexInputBindingDescription binding = {0, sizeof(UiVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 8}, {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 16}};
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &binding;
    vin.vertexAttributeDescriptionCount = 3;
    vin.pVertexAttributeDescriptions = attributes;
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
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
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
    gp.layout = U.layout;
    gp.renderPass = V.pass;
    VkResult r = vkCreateGraphicsPipelines(V.device, VK_NULL_HANDLE, 1, &gp, NULL, &U.pipeline);
    if (r == VK_SUCCESS)
    {
        blend.blendEnable = VK_FALSE; /* for pictures drawn without blending: the alpha stays out of it */
        r = vkCreateGraphicsPipelines(V.device, VK_NULL_HANDLE, 1, &gp, NULL, &U.pipeline_opaque);
    }
    vkDestroyShaderModule(V.device, vert, NULL);
    vkDestroyShaderModule(V.device, frag, NULL);
    CHECK(r, "interface pipeline");

    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 16.0f;
    CHECK(vkCreateSampler(V.device, &si, NULL, &U.smooth), "interface sampler");
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.maxLod = 0.0f;
    CHECK(vkCreateSampler(V.device, &si, NULL, &U.nearest), "interface nearest sampler");

    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096};
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = 4096;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    CHECK(vkCreateDescriptorPool(V.device, &dp, NULL, &U.pool), "interface descriptor pool");

    for (int f = 0; f < FRAMES; ++f)
    {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = (VkDeviceSize)UI_MAX_VERTS * sizeof(UiVertex);
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        CHECK(vkCreateBuffer(V.device, &bi, NULL, &U.vbuf[f]), "interface vertex buffer");
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(V.device, U.vbuf[f], &req);
        if (!allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &U.vmem[f]))
        {
            snprintf(error, size, "no memory for the interface's vertices");
            return false;
        }
        CHECK(vkBindBufferMemory(V.device, U.vbuf[f], U.vmem[f], 0), "bind interface vertices");
        CHECK(vkMapMemory(V.device, U.vmem[f], 0, VK_WHOLE_SIZE, 0, &U.vmap[f]), "map interface vertices");
    }
    U.ready = true;
    return true;
}

/* ---------------------------------------------------------------- the calls the platform layer makes */

bool vkp_ui_open(char *error, size_t size)
{
    if (!V.swapchain)
        return false;
    if (!U.verts)
        U.verts = malloc((size_t)UI_MAX_VERTS * sizeof(UiVertex));
    if (!U.batches)
        U.batches = malloc((size_t)UI_MAX_BATCHES * sizeof(UiBatch));
    if (!U.verts || !U.batches)
    {
        snprintf(error, size, "no memory for the interface's lists");
        return false;
    }
    if (!ui_create(error, size))
    {
        ui_destroy();
        return false;
    }
    U.enabled = true;
    U.nverts = 0;
    U.nbatches = 0;
    return true;
}

bool vkp_ui_active(void)
{
    return U.enabled;
}

int vkp_ui_texture_create(const uint8_t *rgba, int w, int h, bool smooth, bool mips, bool copy)
{
    if (!U.enabled || w < 1 || h < 1)
        return -1;
    int id = -1;
    for (int i = 0; i < U.tex_cap; ++i)
        if (!U.tex[i])
        {
            id = i;
            break;
        }
    if (id < 0)
    {
        int cap = U.tex_cap ? U.tex_cap * 2 : 256;
        UiTex **grown = realloc(U.tex, (size_t)cap * sizeof(*grown));
        if (!grown)
            return -1;
        memset(grown + U.tex_cap, 0, (size_t)(cap - U.tex_cap) * sizeof(*grown));
        id = U.tex_cap;
        U.tex = grown;
        U.tex_cap = cap;
    }
    UiTex *t = calloc(1, sizeof(*t));
    if (!t)
        return -1;
    if (copy)
    {
        t->rgba = malloc((size_t)w * h * 4);
        if (!t->rgba)
        {
            free(t);
            return -1;
        }
        memcpy(t->rgba, rgba, (size_t)w * h * 4);
        t->owned = true;
    }
    else
        t->rgba = (uint8_t *)rgba;
    t->w = w;
    t->h = h;
    t->smooth = smooth;
    t->mips = mips;
    U.tex[id] = t;
    return id;
}

void vkp_ui_texture_free(int id)
{
    if (id < 0 || id >= U.tex_cap || !U.tex[id])
        return;
    UiTex *t = U.tex[id];
    ui_gpu_release(t);
    if (t->owned)
        free(t->rgba);
    free(t);
    U.tex[id] = NULL;
}

/* New pixels for a texture of the same size (the software renderer's picture). */
void vkp_ui_texture_update(int id, const uint8_t *rgba)
{
    if (id < 0 || id >= U.tex_cap || !U.tex[id])
        return;
    UiTex *t = U.tex[id];
    if (t->owned)
        memcpy(t->rgba, rgba, (size_t)t->w * t->h * 4);
    ui_gpu_release(t);
}

void vkp_ui_begin(uint32_t clear_argb)
{
    const bool set = (clear_argb >> 24) != 0;
    U.clear[0] = set ? ((clear_argb >> 16) & 0xff) / 255.0f : 0.0f;
    U.clear[1] = set ? ((clear_argb >> 8) & 0xff) / 255.0f : 0.0f;
    U.clear[2] = set ? (clear_argb & 0xff) / 255.0f : 0.0f;
    U.clear[3] = 1.0f;
    U.nverts = 0;
    U.nbatches = 0;
    U.game_at = -1;
    U.force_new = false;
    U.opaque = false;
    U.clip[0] = U.clip[1] = U.clip[2] = U.clip[3] = 0;
}

/* The game's picture goes here in the list: what was drawn so far is under it (the border, the TV), what comes
 * next over it. It is what the canvas's hole did. */
/* Draws after this are blended (the default) or copied as they are, ignoring the texture's alpha. */
void vkp_ui_blend(bool on)
{
    U.opaque = !on;
}

void vkp_ui_game_here(void)
{
    U.game_at = U.nbatches;
    U.force_new = true;
}

void vkp_ui_clip(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
    {
        U.clip[0] = U.clip[1] = U.clip[2] = U.clip[3] = 0;
        return;
    }
    U.clip[0] = x;
    U.clip[1] = y;
    U.clip[2] = w;
    U.clip[3] = h;
}

/* Triangles (3 vertices each) with a texture, or the white pixel when id < 0. */
void vkp_ui_draw(int id, const VkpUiVertex *v, int count)
{
    if (!U.ready || count <= 0)
        return;
    if (id < 0)
    {
        if (U.white < 0)
        {
            static const uint8_t white[4] = {255, 255, 255, 255};
            U.white = vkp_ui_texture_create(white, 1, 1, false, false, true);
        }
        id = U.white;
    }
    if (id < 0 || id >= U.tex_cap || !U.tex[id] || !ui_texture_ready(U.tex[id]))
        return;
    if (U.nverts + (uint32_t)count > UI_MAX_VERTS)
    {
        if (!U.warned++)
            SwanStationPS5_log("vulkan: the interface list is full, drawing is cut short");
        return;
    }
    const VkDescriptorSet set = U.tex[id]->set;
    UiBatch *b = U.nbatches ? &U.batches[U.nbatches - 1] : NULL;
    if (!b || U.force_new || b->set != set || b->opaque != U.opaque || memcmp(b->clip, U.clip, sizeof(U.clip)) != 0)
    {
        U.force_new = false;
        if (U.nbatches >= UI_MAX_BATCHES)
            return;
        b = &U.batches[U.nbatches++];
        b->set = set;
        b->opaque = U.opaque;
        memcpy(b->clip, U.clip, sizeof(U.clip));
        b->first = U.nverts;
        b->count = 0;
    }
    UiVertex *out = U.verts + U.nverts;
    for (int i = 0; i < count; ++i)
    {
        uint32_t c = v[i].argb;
        out[i].x = v[i].x;
        out[i].y = v[i].y;
        out[i].u = v[i].u;
        out[i].v = v[i].v;
        out[i].abgr = (c & 0xff00ff00u) | ((c >> 16) & 0xffu) | ((c & 0xffu) << 16);
    }
    b->count += (uint32_t)count;
    U.nverts += (uint32_t)count;
}

/* Inside the swapchain's render pass, over the game picture. */
static void ui_prepare(int f)
{
    if (U.ready && U.nbatches)
        memcpy(U.vmap[f], U.verts, (size_t)U.nverts * sizeof(UiVertex));
}

/* The batches from..to-1 */
static void ui_record(VkCommandBuffer cb, int f, int from, int to)
{
    if (!U.ready || from >= to)
        return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, U.pipeline);
    bool opaque_bound = false;
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &U.vbuf[f], &zero);
    float screen[2] = {(float)V.cw, (float)V.ch};
    vkCmdPushConstants(cb, U.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(screen), screen);
    const float kx = (float)V.extent.width / V.cw, ky = (float)V.extent.height / V.ch;
    VkDescriptorSet bound = VK_NULL_HANDLE;
    int clip[4] = {-1, 0, 0, 0};
    for (int i = from; i < to; ++i)
    {
        const UiBatch *b = &U.batches[i];
        if (memcmp(clip, b->clip, sizeof(clip)) != 0)
        {
            memcpy(clip, b->clip, sizeof(clip));
            VkRect2D scissor;
            if (clip[2] > 0)
            {
                int x0 = (int)(clip[0] * kx), y0 = (int)(clip[1] * ky);
                int x1 = (int)((clip[0] + clip[2]) * kx + 0.5f), y1 = (int)((clip[1] + clip[3]) * ky + 0.5f);
                if (x0 < 0)
                    x0 = 0;
                if (y0 < 0)
                    y0 = 0;
                if (x1 > (int)V.extent.width)
                    x1 = (int)V.extent.width;
                if (y1 > (int)V.extent.height)
                    y1 = (int)V.extent.height;
                scissor.offset.x = x0;
                scissor.offset.y = y0;
                scissor.extent.width = x1 > x0 ? (uint32_t)(x1 - x0) : 0;
                scissor.extent.height = y1 > y0 ? (uint32_t)(y1 - y0) : 0;
            }
            else
            {
                scissor.offset.x = scissor.offset.y = 0;
                scissor.extent = V.extent;
            }
            vkCmdSetScissor(cb, 0, 1, &scissor);
        }
        if (opaque_bound != b->opaque)
        {
            opaque_bound = b->opaque;
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque_bound ? U.pipeline_opaque : U.pipeline);
        }
        if (bound != b->set)
        {
            bound = b->set;
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, U.layout, 0, 1, &bound, 0, NULL);
        }
        if (b->count)
            vkCmdDraw(cb, b->count, 1, b->first, 0);
    }
    /* whatever draws next (the game's picture) starts with the whole screen */
    VkRect2D full = {{0, 0}, V.extent};
    vkCmdSetScissor(cb, 0, 1, &full);
}
