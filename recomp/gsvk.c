/* GS on the GPU (Vulkan compute), first stage: a sprite rasterizer (vk/gs_sprite.comp) that does per pixel exactly
   what gs.c does, checked draw by draw against the software GS (HG_GSVKCHECK in gs.c). GS local memory lives in
   video memory (dvram), mirrored by a host-cached staging buffer (vram): before a checked draw or batch the 4 KB
   chunks where the CPU's memory differs from the mirror are copied up, after it the whole of it is copied back and
   the CPU compares it with its own result. GPU time is measured with timestamps around the dispatch. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "ee_runtime.h"
#include "hle.h"
#include "gsvk.h"
#include "gen/gs_sprite_spv.h"
#include "gen/gs_batch_spv.h"
#include <time.h>

static VkInstance inst; static VkPhysicalDevice phys; static VkDevice dev; static VkQueue queue; static uint32_t qfam;
static VkDescriptorSetLayout dsl; static VkPipelineLayout pl; static VkPipeline pipe; static VkDescriptorPool dpool; static VkDescriptorSet dset;
static VkCommandPool cpool; static VkCommandBuffer cmd; static VkFence fence;
typedef struct { VkBuffer b; VkDeviceMemory m; void *p; VkDeviceSize n; } Buf;
static Buf vram, dvram, xoffb, clutb, parb, tabb;
static VkQueryPool qpool; static float tsperiod; static VkBufferCopy regs[1024]; static uint32_t nregs; static int vfresh = 1;
enum { BATCH_MAX = 4096, BCLUT_MAX = 1024 };
static VkDescriptorSetLayout bdsl; static VkPipelineLayout bpl; static VkPipeline bpipe; static VkDescriptorPool bdpool; static VkDescriptorSet bdset;
static Buf primb, bclutb; static uint32_t nb, nbclut; static int bx0, by0, bx1, by1;
static int state;   /* 0 untried, 1 ready, -1 unavailable */

static int mkbuf_(Buf *b, VkDeviceSize n, VkBufferUsageFlags use, int mode) {   /* 0 host coherent, 1 + cached, 2 device local */
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = use, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (vkCreateBuffer(dev, &bi, 0, &b->b)) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b->b, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    const VkMemoryPropertyFlags want = mode == 2 ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT :
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | (mode ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0);
    uint32_t t = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) { t = i; break; }
    if (t == UINT32_MAX) return 0;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t };
    if (vkAllocateMemory(dev, &ai, 0, &b->m) || vkBindBufferMemory(dev, b->b, b->m, 0)) return 0;
    if (mode != 2 && vkMapMemory(dev, b->m, 0, VK_WHOLE_SIZE, 0, &b->p)) return 0;
    b->n = n; return 1;
}
static int mkbuf(Buf *b, VkDeviceSize n, VkBufferUsageFlags use) { return mkbuf_(b, n, use, 0); }

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
    VkPhysicalDeviceFeatures have, want = { 0 }; vkGetPhysicalDeviceFeatures(phys, &have);
    if (!have.shaderFloat64) { L("GSVK: no 64-bit floats in shaders (triangle Z)\n"); return 0; }
    want.shaderFloat64 = VK_TRUE;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &want };
    if (vkCreateDevice(phys, &dci, 0, &dev)) { L("GSVK: no device\n"); return 0; }
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    const VkBufferUsageFlags xfer = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!mkbuf_(&vram, ((1u << 20) + 4) * 4, xfer, 1)) { vkDestroyBuffer(dev, vram.b, 0); L("GSVK: no host-cached memory, staging uncached\n"); }
    if ((!vram.p && !mkbuf(&vram, ((1u << 20) + 4) * 4, xfer)) || !mkbuf_(&dvram, ((1u << 20) + 4) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | xfer, 2) || !mkbuf(&xoffb, 66u * 2048 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
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
    VkDescriptorBufferInfo bi[5] = { { dvram.b, 0, VK_WHOLE_SIZE }, { xoffb.b, 0, VK_WHOLE_SIZE }, { clutb.b, 0, VK_WHOLE_SIZE }, { parb.b, 0, VK_WHOLE_SIZE }, { tabb.b, 0, VK_WHOLE_SIZE } };
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
    /* the batch pipeline: binding 2 the batch's CLUTs, 3 its primitives, push constants (count, origin) */
    if (!mkbuf(&primb, (VkDeviceSize)BATCH_MAX * sizeof(GsVkSprite), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
        !mkbuf(&bclutb, (VkDeviceSize)BCLUT_MAX * 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) { L("GSVK: batch buffers failed\n"); return 0; }
    VkDescriptorSetLayoutBinding bbd[5];
    for (int i = 0; i < 5; i++) bbd[i] = (VkDescriptorSetLayoutBinding){ .binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                                          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo bdli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = bbd };
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 12 };
    VkPipelineLayoutCreateInfo bpli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &bdsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VkShaderModuleCreateInfo bsmi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof gs_batch_spv, .pCode = gs_batch_spv };
    VkShaderModule bsm;
    if (vkCreateDescriptorSetLayout(dev, &bdli, 0, &bdsl) || vkCreatePipelineLayout(dev, &bpli, 0, &bpl) || vkCreateShaderModule(dev, &bsmi, 0, &bsm)) { L("GSVK: batch layout failed\n"); return 0; }
    VkComputePipelineCreateInfo bcpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = bpl,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = bsm, .pName = "main" } };
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &bcpi, 0, &bpipe)) { L("GSVK: batch pipeline failed\n"); return 0; }
    VkDescriptorPoolSize bps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 };
    VkDescriptorPoolCreateInfo bdpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &bps };
    VkDescriptorSetAllocateInfo bdai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = 1, .pSetLayouts = &bdsl };
    if (vkCreateDescriptorPool(dev, &bdpi, 0, &bdpool)) return 0;
    bdai.descriptorPool = bdpool; if (vkAllocateDescriptorSets(dev, &bdai, &bdset)) return 0;
    VkDescriptorBufferInfo bbi[5] = { { dvram.b, 0, VK_WHOLE_SIZE }, { xoffb.b, 0, VK_WHOLE_SIZE }, { bclutb.b, 0, VK_WHOLE_SIZE }, { primb.b, 0, VK_WHOLE_SIZE }, { tabb.b, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet bwr[5];
    for (int i = 0; i < 5; i++) bwr[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = bdset, .dstBinding = (uint32_t)i, .descriptorCount = 1,
                                                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bbi[i] };
    vkUpdateDescriptorSets(dev, 5, bwr, 0, 0);
    VkQueryPoolCreateInfo qpi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 };
    if (vkCreateQueryPool(dev, &qpi, 0, &qpool)) return 0;
    tsperiod = pr.limits.timestampPeriod;
    L("GSVK: Vulkan compute on %s\n", pr.deviceName);
    state = 1; return 1;
}

/* the mirror takes the CPU's memory; the chunks that changed are remembered for the upload */
static void sync_from(const uint32_t *now) {
    uint32_t *g = vram.p; nregs = 0;
    for (uint32_t i = 0; i < (1u << 20); i += 1024) {
        if (!vfresh && !memcmp(g + i, now + i, 4096)) continue;
        memcpy(g + i, now + i, 4096);
        if (nregs && regs[nregs - 1].srcOffset + regs[nregs - 1].size == (VkDeviceSize)i * 4) regs[nregs - 1].size += 4096;
        else regs[nregs++] = (VkBufferCopy){ (VkDeviceSize)i * 4, (VkDeviceSize)i * 4, 4096 };
    }
    vfresh = 0;
}
/* command buffer: upload, timestamp, (caller's dispatch), timestamp, download */
static void rec_begin(void) {
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(cmd, 0); vkBeginCommandBuffer(cmd, &cbi);
    vkCmdResetQueryPool(cmd, qpool, 0, 2);
    if (nregs) {
        vkCmdCopyBuffer(cmd, vram.b, dvram.b, nregs, regs);
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, 0, 0, 0);
    }
    nregs = 0;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 0);
}
static int submit_wait(double *gpu_ms) {
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 1);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
    VkBufferCopy all = { 0, 0, (VkDeviceSize)(1u << 20) * 4 };
    vkCmdCopyBuffer(cmd, dvram.b, vram.b, 1, &all);
    VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, 0, 0, 0);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    vkResetFences(dev, 1, &fence);
    if (vkQueueSubmit(queue, 1, &si, fence) || vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull)) { L("GSVK: submit failed\n"); return 0; }
    uint64_t ts[2];
    if (gpu_ms) *gpu_ms = vkGetQueryPoolResults(dev, qpool, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) ? 0 : (double)(ts[1] - ts[0]) * tsperiod / 1e6;
    return 1;
}
void gsvk_batch_begin(const uint32_t *now) { if (state > 0) sync_from(now); nb = nbclut = 0; bx0 = by0 = 1 << 30; bx1 = by1 = -(1 << 30); }
int gsvk_batch_add(const GsVkSprite *s, const uint32_t *clut) {
    if (state <= 0) return 1;
    if (nb == BATCH_MAX) return 0;
    uint32_t *cl = bclutb.p;
    if (!nbclut || memcmp(cl + (nbclut - 1) * 256, clut, 1024)) { if (nbclut == BCLUT_MAX) return 0; memcpy(cl + nbclut++ * 256, clut, 1024); }
    GsVkSprite *d = (GsVkSprite *)primb.p + nb++; *d = *s; d->clutoff = (nbclut - 1) * 256;
    if (s->px0 < bx0) bx0 = s->px0; if (s->py0 < by0) by0 = s->py0; if (s->px1 > bx1) bx1 = s->px1; if (s->py1 > by1) by1 = s->py1;
    return 1;
}
const uint32_t *gsvk_batch_flush(double *ms) {
    *ms = 0;
    if (state <= 0) return 0;
    if (!nb) return vram.p;
    if (bx0 < 0) bx0 = 0; if (by0 < 0) by0 = 0; if (bx1 > 2048) bx1 = 2048; if (by1 > 2048) by1 = 2048;
    if (bx1 <= bx0 || by1 <= by0) return vram.p;
    rec_begin();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bpipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bpl, 0, 1, &bdset, 0, 0);
    int32_t pc[3] = { (int32_t)nb, bx0, by0 };
    vkCmdPushConstants(cmd, bpl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cmd, (uint32_t)(bx1 - bx0 + 7) / 8, (uint32_t)(by1 - by0 + 7) / 8, 1);
    if (!submit_wait(ms)) return 0;
    return vram.p;
}
const uint32_t *gsvk_draw(const GsVkSprite *s, const uint32_t *clut, const uint32_t *now) {
    if (state <= 0) return 0;
    uint32_t *g = vram.p;
    sync_from(now);
    memcpy(clutb.p, clut, 256 * 4); memcpy(parb.p, s, sizeof *s);
    rec_begin();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset, 0, 0);
    if (s->serial) vkCmdDispatch(cmd, 1, 1, 1);
    else vkCmdDispatch(cmd, (uint32_t)(s->px1 - s->px0 + 7) / 8, (uint32_t)(s->py1 - s->py0 + 7) / 8, 1);
    if (!submit_wait(0)) return 0;
    return g;
}
