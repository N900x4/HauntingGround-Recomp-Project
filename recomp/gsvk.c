/* GS on the GPU (Vulkan compute): a rasterizer (vk/gs_sprite.comp) that does per pixel exactly what gs.c does.
   GS local memory lives in video memory (dvram). Work is recorded into one command buffer that stays open until a
   result is needed (gsvk_finish / gsvk_download): primitives one at a time or in batches, uploads of local memory
   from the CPU (copied into a ring at once, so later CPU changes do not leak in), downloads.
   Two uses (gs.c): HG_GSVKCHECK compares the GPU's local memory with the software GS after every primitive or batch
   (a host-cached mirror, vram, holds the GPU's memory as last read back, so only changed 4 KB chunks go up);
   HG_GSVK=1 draws with the GPU only, gs.c keeping track of which pages each side has changed. */
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
static VkDescriptorSetLayout dsl; static VkPipelineLayout pl; static VkPipeline pipe1, pipeb; static VkDescriptorPool dpool; static VkDescriptorSet dset;
static VkCommandPool cpool; static VkCommandBuffer cmd; static VkFence fence; static VkQueryPool qpool; static float tsperiod;
typedef struct { VkBuffer b; VkDeviceMemory m; void *p; VkDeviceSize n; } Buf;
static Buf vram, dvram, xoffb, tabb, primb, clutb, upb, hrb;   /* hrb: the upscaled display read back */
static VkDeviceSize outbase, outbytes;   /* in dvram, after the copies */
enum { PRIM_MAX = 32768, CLUT_MAX = 4096, UP_BYTES = 32 << 20 };
static int state;   /* 0 untried, 1 ready, -1 unavailable */
/* recording */
static int open_, bound; static VkPipeline cur_pipe; static uint32_t nprim, nclut, upoff;
static uint32_t bfirst, bn; static int bx0, by0, bx1, by1;   /* the batch being collected */
static int vfresh = 1;   /* the mirror does not hold the GPU's memory */
static uint32_t ncopy = 1;   /* copies of local memory in dvram, 4 MB apart */
static uint64_t n_submits, n_dispatch; static double gpu_ms_all, wait_ms_all;

/* mode: 0 host coherent, 1 + cached, 2 device local, 3 device local and host visible (else host coherent) */
static int mkbuf_(Buf *b, VkDeviceSize n, VkBufferUsageFlags use, int mode) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = use, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (vkCreateBuffer(dev, &bi, 0, &b->b)) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b->b, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryPropertyFlags want[2] = { mode == 2 ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : mode == 3 ? hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT :
                                      hv | (mode == 1 ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0), hv };
    uint32_t t = UINT32_MAX;
    for (int k = 0; k < (mode == 3 ? 2 : 1) && t == UINT32_MAX; k++)
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want[k]) == want[k]) { t = i; break; }
    if (t == UINT32_MAX) return 0;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t };
    if (vkAllocateMemory(dev, &ai, 0, &b->m) || vkBindBufferMemory(dev, b->b, b->m, 0)) return 0;
    if (mode != 2 && vkMapMemory(dev, b->m, 0, VK_WHOLE_SIZE, 0, &b->p)) return 0;
    b->n = n; return 1;
}
static int mkbuf(Buf *b, VkDeviceSize n, VkBufferUsageFlags use) { return mkbuf_(b, n, use, 0); }

int gsvk_init(const uint32_t *xoff32, const uint32_t *xoff32z, const int32_t *tables, int ncopies) {
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
    const VkBufferUsageFlags xfer = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, sto = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const VkDeviceSize vbytes = ((1u << 20) + 4) * 4;
    ncopy = ncopies < 1 ? 1 : (uint32_t)ncopies;
    outbase = (VkDeviceSize)ncopy * (1u << 22) + 16;
    if (!mkbuf_(&vram, vbytes, xfer, 1)) { vkDestroyBuffer(dev, vram.b, 0); vram.b = 0; L("GSVK: no host-cached memory, mirror uncached\n"); }
    if ((!vram.p && !mkbuf(&vram, vbytes, xfer)) || !mkbuf_(&dvram, (VkDeviceSize)ncopy * (1u << 22) + 16 + (outbytes = ncopy > 1 ? (VkDeviceSize)(ncopy - 1) * 1024 * 1024 * 4 : 0), sto | xfer, 2) ||
        (outbytes && !mkbuf_(&hrb, outbytes, xfer, 1) && !mkbuf(&hrb, outbytes, xfer)) || !mkbuf(&xoffb, 66u * 2048 * 4, sto) ||
        !mkbuf(&tabb, GSVK_TABLES * 4, sto) || !mkbuf_(&primb, (VkDeviceSize)PRIM_MAX * sizeof(GsVkSprite), sto, 3) ||
        !mkbuf_(&clutb, (VkDeviceSize)CLUT_MAX * 1024, sto, 3) || !mkbuf(&upb, UP_BYTES, xfer)) { L("GSVK: buffers failed\n"); return 0; }
    memcpy(xoffb.p, xoff32, 33u * 2048 * 4); memcpy((uint32_t *)xoffb.p + 33u * 2048, xoff32z, 33u * 2048 * 4); memcpy(tabb.p, tables, GSVK_TABLES * 4);
    /* one layout for both pipelines: 0 local memory, 1 32-bit address tables, 2 CLUTs, 3 primitives, 4 other tables;
       push constants: n, bx0, by0, first (batch) or the primitive's index (one primitive) */
    VkDescriptorSetLayoutBinding bd[5];
    for (int i = 0; i < 5; i++) bd[i] = (VkDescriptorSetLayoutBinding){ .binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                                         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = bd };
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 20 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    if (vkCreateDescriptorSetLayout(dev, &dli, 0, &dsl) || vkCreatePipelineLayout(dev, &pli, 0, &pl)) { L("GSVK: pipeline layout failed\n"); return 0; }
    for (int k = 0; k < 2; k++) {
        VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = k ? sizeof gs_batch_spv : sizeof gs_sprite_spv, .pCode = k ? gs_batch_spv : gs_sprite_spv };
        VkShaderModule sm;
        if (vkCreateShaderModule(dev, &smi, 0, &sm)) { L("GSVK: shader failed\n"); return 0; }
        VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
            .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" } };
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, 0, k ? &pipeb : &pipe1)) { L("GSVK: pipeline failed\n"); return 0; }
    }
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    if (vkCreateDescriptorPool(dev, &dpi, 0, &dpool)) return 0;
    dai.descriptorPool = dpool; if (vkAllocateDescriptorSets(dev, &dai, &dset)) return 0;
    VkDescriptorBufferInfo bi[5] = { { dvram.b, 0, VK_WHOLE_SIZE }, { xoffb.b, 0, VK_WHOLE_SIZE }, { clutb.b, 0, VK_WHOLE_SIZE }, { primb.b, 0, VK_WHOLE_SIZE }, { tabb.b, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet wr[5];
    for (int i = 0; i < 5; i++) wr[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = (uint32_t)i, .descriptorCount = 1,
                                                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[i] };
    vkUpdateDescriptorSets(dev, 5, wr, 0, 0);
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam };
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateCommandPool(dev, &cpci, 0, &cpool)) return 0;
    cbai.commandPool = cpool;
    if (vkAllocateCommandBuffers(dev, &cbai, &cmd) || vkCreateFence(dev, &fci, 0, &fence)) return 0;
    VkQueryPoolCreateInfo qpi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 };
    if (vkCreateQueryPool(dev, &qpi, 0, &qpool)) return 0;
    tsperiod = pr.limits.timestampPeriod;
    L("GSVK: Vulkan compute on %s\n", pr.deviceName);
    state = 1; return 1;
}

/* ------------------------------------------------------------------ recording */
static void barrier(void) {   /* everything before (transfers, dispatches) is visible to everything after */
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, 0, 0, 0);
}
static void rec_open(void) {
    if (open_) return;
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(cmd, 0); vkBeginCommandBuffer(cmd, &cbi);
    vkCmdResetQueryPool(cmd, qpool, 0, 2);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, 0);
    open_ = 1; bound = 0; cur_pipe = VK_NULL_HANDLE;
}
static void use(VkPipeline p) {
    if (!bound) { vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset, 0, 0); bound = 1; }
    if (cur_pipe != p) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p); cur_pipe = p; }
}
static void close_batch(void) {
    if (!bn) return;
    int x0 = bx0 < 0 ? 0 : bx0, y0 = by0 < 0 ? 0 : by0, x1 = bx1 > 2048 ? 2048 : bx1, y1 = by1 > 2048 ? 2048 : by1;
    if (x1 > x0 && y1 > y0) {
        rec_open(); barrier(); use(pipeb);
        int32_t pc[5] = { (int32_t)bn, x0, y0, (int32_t)bfirst, (int32_t)ncopy };
        vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vkCmdDispatch(cmd, (uint32_t)(x1 - x0 + 7) / 8, (uint32_t)(y1 - y0 + 7) / 8, ncopy);   /* z: the copy */
        n_dispatch++;
    }
    bn = 0;
}
void gsvk_break(void) { if (state > 0) close_batch(); }
/* submit what is recorded and wait for it; the rings start over */
void gsvk_finish(void) {
    if (state <= 0) return;
    close_batch();
    if (!open_) return;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, 1);
    vkEndCommandBuffer(cmd); open_ = 0;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    vkResetFences(dev, 1, &fence);
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    if (vkQueueSubmit(queue, 1, &si, fence) || vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull)) { L("GSVK: submit failed\n"); state = -1; return; }
    clock_gettime(CLOCK_MONOTONIC, &t1); wait_ms_all += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    uint64_t ts[2];
    if (!vkGetQueryPoolResults(dev, qpool, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)) gpu_ms_all += (double)(ts[1] - ts[0]) * tsperiod / 1e6;
    n_submits++; nprim = nclut = 0; upoff = 0;
}
/* local memory words [w, w+n) from src, at this point of the stream */
void gsvk_upload(uint32_t w, uint32_t n, const uint32_t *src) {
    if (state <= 0 || !n) return;
    close_batch();
    if (upoff + n * 4 > UP_BYTES) gsvk_finish();
    memcpy((uint8_t *)upb.p + upoff, src, (size_t)n * 4);
    rec_open(); barrier();
    VkBufferCopy c[65];
    for (uint32_t k = 0; k < ncopy && k < 65; k++) c[k] = (VkBufferCopy){ upoff, (VkDeviceSize)k * (1u << 22) + (VkDeviceSize)w * 4, (VkDeviceSize)n * 4 };
    vkCmdCopyBuffer(cmd, upb.b, dvram.b, ncopy < 65 ? ncopy : 65, c);
    upoff += (n * 4 + 255) & ~255u;
}
/* one primitive: alone (lines, serial ones) or added to the batch being collected */
void gsvk_prim(const GsVkSprite *s, const uint32_t *clut, int alone) {
    if (state <= 0) return;
    if (s->mbase == 0 && (nprim + ncopy > PRIM_MAX || nclut == CLUT_MAX)) gsvk_finish();   /* a primitive's copies stay together */
    static uint32_t last[256];   /* the ring may be video memory mapped over the bus: never read it back */
    if (!nclut || memcmp(last, clut, 1024)) { memcpy(last, clut, 1024); memcpy((uint32_t *)clutb.p + nclut++ * 256, clut, 1024); }
    GsVkSprite *d = (GsVkSprite *)primb.p + nprim; *d = *s; d->clutoff = (nclut - 1) * 256;
    if (alone) {
        close_batch(); rec_open(); barrier(); use(pipe1);
        int32_t pc[5] = { (int32_t)nprim, 0, 0, 0, (int32_t)ncopy };
        vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        if (s->serial) vkCmdDispatch(cmd, 1, 1, 1);
        else vkCmdDispatch(cmd, (uint32_t)(s->px1 - s->px0 + 7) / 8, (uint32_t)(s->py1 - s->py0 + 7) / 8, 1);
        n_dispatch++;
    } else {
        if (!bn) { bfirst = nprim; bx0 = by0 = 1 << 30; bx1 = by1 = -(1 << 30); }
        bn++;
        if (s->px1 > s->px0 && s->py1 > s->py0) {   /* (a copy's empty place holder adds nothing) */
            if (s->px0 < bx0) bx0 = s->px0; if (s->py0 < by0) by0 = s->py0; if (s->px1 > bx1) bx1 = s->px1; if (s->py1 > by1) by1 = s->py1; }
    }
    nprim++;
}
/* the 8 KB pages set in `pages` (512 bits) from the GPU (copy k) into dst */
void gsvk_download(const uint64_t pages[8], uint32_t *dst) { gsvk_download_copy(0, pages, dst); }
void gsvk_download_copy(int k, const uint64_t pages[8], uint32_t *dst) {
    if (state <= 0 || k < 0 || (uint32_t)k >= ncopy) return;
    close_batch();
    VkBufferCopy c[512]; uint32_t nc = 0;
    for (uint32_t p = 0; p < 512; p++) if (pages[p >> 6] >> (p & 63) & 1) {
        if (nc && c[nc - 1].srcOffset + c[nc - 1].size == (VkDeviceSize)p * 8192) c[nc - 1].size += 8192;
        else c[nc++] = (VkBufferCopy){ (VkDeviceSize)p * 8192, (VkDeviceSize)p * 8192, 8192 };
    }
    if (!nc) return;
    for (uint32_t i = 0; i < nc; i++) c[i].srcOffset += (VkDeviceSize)k * (1u << 22);
    rec_open(); barrier();
    vkCmdCopyBuffer(cmd, dvram.b, vram.b, nc, c);
    for (uint32_t i = 0; i < nc; i++) c[i].srcOffset -= (VkDeviceSize)k * (1u << 22);
    VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, 0, 0, 0);
    gsvk_finish();
    for (uint32_t i = 0; i < nc; i++) memcpy((uint8_t *)dst + c[i].srcOffset, (uint8_t *)vram.p + c[i].srcOffset, c[i].size);
    vfresh = 1;   /* the mirror no longer holds the whole of the GPU's memory */
}
int gsvk_compose(const GsVkSprite *s0, uint32_t *dst) {
    if (state <= 0 || !outbytes) return 0;
    GsVkSprite s = *s0;
    const VkDeviceSize n = (VkDeviceSize)s.px1 * s.py1 * 4;
    if (n > outbytes || s.px1 <= 0 || s.py1 <= 0) return 0;
    s.prim = 4; s.px0 = s.py0 = 0; s.serial = 0; s.z = (uint32_t)(outbase / 4);
    static const uint32_t noclut[256];
    gsvk_prim(&s, noclut, 1);
    rec_open(); barrier();
    VkBufferCopy c = { outbase, 0, n };
    vkCmdCopyBuffer(cmd, dvram.b, hrb.b, 1, &c);
    VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, 0, 0, 0);
    gsvk_finish();
    if (state <= 0) return 0;
    memcpy(dst, hrb.p, n);
    return 1;
}
void gsvk_stats(uint64_t *submits, uint64_t *dispatches, double *gpu_ms, double *wait_ms) { *submits = n_submits; *dispatches = n_dispatch; *gpu_ms = gpu_ms_all; *wait_ms = wait_ms_all; }

/* ------------------------------------------------------------------ checking against the CPU */
/* the GPU's memory becomes `now`: the mirror takes it, the 4 KB chunks that changed go up */
void gsvk_check_begin(const uint32_t *now) {
    if (state <= 0) return;
    static VkBufferCopy regs[1024]; uint32_t nregs = 0;
    uint32_t *g = vram.p;
    for (uint32_t i = 0; i < (1u << 20); i += 1024) {
        if (!vfresh && !memcmp(g + i, now + i, 4096)) continue;
        memcpy(g + i, now + i, 4096);
        if (nregs && regs[nregs - 1].srcOffset + regs[nregs - 1].size == (VkDeviceSize)i * 4) regs[nregs - 1].size += 4096;
        else regs[nregs++] = (VkBufferCopy){ (VkDeviceSize)i * 4, (VkDeviceSize)i * 4, 4096 };   /* at most 512 runs */
    }
    vfresh = 0;
    if (nregs) { close_batch(); rec_open(); barrier(); vkCmdCopyBuffer(cmd, vram.b, dvram.b, nregs, regs); }
}
/* everything recorded is drawn; the GPU's whole memory, read back into the mirror, is returned (GPU ms in *ms) */
const uint32_t *gsvk_check_end(double *ms) {
    if (state <= 0) return 0;
    double before = gpu_ms_all;
    close_batch(); rec_open(); barrier();
    VkBufferCopy all = { 0, 0, (VkDeviceSize)(1u << 20) * 4 };
    vkCmdCopyBuffer(cmd, dvram.b, vram.b, 1, &all);
    VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, 0, 0, 0);
    gsvk_finish();
    if (state <= 0) return 0;
    if (ms) *ms = gpu_ms_all - before;
    return vram.p;
}
