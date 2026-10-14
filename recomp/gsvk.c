/* GS on the GPU (Vulkan compute), first stage: a sprite rasterizer (vk/gs_sprite.comp) that does per pixel exactly
   what gs.c does, checked draw by draw against the software GS (HG_GSVKCHECK in gs.c). GS local memory lives in a
   GPU buffer; this stage uploads it before each checked draw and reads it back after, which is slow but makes every
   draw comparable on its own. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "ee_runtime.h"
#include "hle.h"
#include "gsvk.h"
#include "gen/gs_sprite_spv.h"

static VkInstance inst; static VkPhysicalDevice phys; static VkDevice dev; static VkQueue queue; static uint32_t qfam;
static VkDescriptorSetLayout dsl; static VkPipelineLayout pl; static VkPipeline pipe; static VkDescriptorPool dpool; static VkDescriptorSet dset;
static VkCommandPool cpool; static VkCommandBuffer cmd; static VkFence fence;
typedef struct { VkBuffer b; VkDeviceMemory m; void *p; VkDeviceSize n; } Buf;
static Buf vram, xoffb, clutb, parb, tabb;
static int state;   /* 0 untried, 1 ready, -1 unavailable */

static int mkbuf(Buf *b, VkDeviceSize n, VkBufferUsageFlags use) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = use, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (vkCreateBuffer(dev, &bi, 0, &b->b)) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b->b, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t t = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) { t = i; break; }
    if (t == UINT32_MAX) return 0;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t };
    if (vkAllocateMemory(dev, &ai, 0, &b->m) || vkBindBufferMemory(dev, b->b, b->m, 0) || vkMapMemory(dev, b->m, 0, VK_WHOLE_SIZE, 0, &b->p)) return 0;
    b->n = n; return 1;
}

int gsvk_init(const uint32_t *xoff32, const uint32_t *xoff32z, const int32_t *tables) {
    if (state) return state > 0;
    state = -1;
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "hgboot", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    if (vkCreateInstance(&ici, 0, &inst)) { L("GSVK: no Vulkan instance\n"); return 0; }
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, 0);
    VkPhysicalDevice devs[8]; if (n > 8) n = 8; vkEnumeratePhysicalDevices(inst, &n, devs);
    for (uint32_t i = 0; i < n && !phys; i++) { VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(devs[i], &pr); if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) phys = devs[i]; }
    if (!phys && n) phys = devs[0];
    if (!phys) { L("GSVK: no GPU\n"); return 0; }
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(phys, &pr);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, 0);
    VkQueueFamilyProperties qf[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    qfam = UINT32_MAX; for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfam = i; break; }
    if (qfam == UINT32_MAX) { L("GSVK: no compute queue\n"); return 0; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    if (vkCreateDevice(phys, &dci, 0, &dev)) { L("GSVK: no device\n"); return 0; }
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    if (!mkbuf(&vram, ((1u << 20) + 4) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !mkbuf(&xoffb, 66u * 2048 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !mkbuf(&clutb, 256 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || !mkbuf(&parb, sizeof(GsVkSprite), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !mkbuf(&tabb, GSVK_TABLES * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) { L("GSVK: buffers failed\n"); return 0; }
    memcpy(xoffb.p, xoff32, 33u * 2048 * 4); memcpy((uint32_t *)xoffb.p + 33u * 2048, xoff32z, 33u * 2048 * 4); memcpy(tabb.p, tables, GSVK_TABLES * 4);
    VkDescriptorSetLayoutBinding bd[5];
    for (int i = 0; i < 5; i++) bd[i] = (VkDescriptorSetLayoutBinding){ .binding = (uint32_t)i, .descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                                         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = bd };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof gs_sprite_spv, .pCode = gs_sprite_spv };
    VkShaderModule sm;
    if (vkCreateDescriptorSetLayout(dev, &dli, 0, &dsl) || vkCreatePipelineLayout(dev, &pli, 0, &pl) || vkCreateShaderModule(dev, &smi, 0, &sm)) { L("GSVK: pipeline layout failed\n"); return 0; }
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" } };
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, 0, &pipe)) { L("GSVK: pipeline failed\n"); return 0; }
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps };
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    if (vkCreateDescriptorPool(dev, &dpi, 0, &dpool)) return 0;
    dai.descriptorPool = dpool; if (vkAllocateDescriptorSets(dev, &dai, &dset)) return 0;
    VkDescriptorBufferInfo bi[5] = { { vram.b, 0, VK_WHOLE_SIZE }, { xoffb.b, 0, VK_WHOLE_SIZE }, { clutb.b, 0, VK_WHOLE_SIZE }, { parb.b, 0, VK_WHOLE_SIZE }, { tabb.b, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet wr[5];
    for (int i = 0; i < 5; i++) wr[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = (uint32_t)i, .descriptorCount = 1,
                                                               .descriptorType = bd[i].descriptorType, .pBufferInfo = &bi[i] };
    vkUpdateDescriptorSets(dev, 5, wr, 0, 0);
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam };
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateCommandPool(dev, &cpci, 0, &cpool)) return 0;
    cbai.commandPool = cpool;
    if (vkAllocateCommandBuffers(dev, &cbai, &cmd) || vkCreateFence(dev, &fci, 0, &fence)) return 0;
    L("GSVK: Vulkan compute on %s\n", pr.deviceName);
    state = 1; return 1;
}

/* draw one sprite on the GPU, from local memory `before`, into `out` (both 1M words + padding) */
int gsvk_sprite(const GsVkSprite *s, const uint32_t *clut, const uint32_t *before, uint32_t *out) {
    if (state <= 0) return 0;
    memcpy(vram.p, before, vram.n); memcpy(clutb.p, clut, 256 * 4); memcpy(parb.p, s, sizeof *s);
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(cmd, 0); vkBeginCommandBuffer(cmd, &cbi);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset, 0, 0);
    vkCmdDispatch(cmd, (uint32_t)(s->px1 - s->px0 + 7) / 8, (uint32_t)(s->py1 - s->py0 + 7) / 8, 1);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, 0, 0, 0);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    vkResetFences(dev, 1, &fence);
    if (vkQueueSubmit(queue, 1, &si, fence) || vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull)) { L("GSVK: submit failed\n"); return 0; }
    memcpy(out, vram.p, vram.n);
    return 1;
}
