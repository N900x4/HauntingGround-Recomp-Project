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
#include "gen/gs_batchc_spv.h"
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

static VkInstance inst; static VkPhysicalDevice phys; static VkDevice dev; static VkQueue queue; static uint32_t qfam;
static VkDescriptorSetLayout dsl; static VkPipelineLayout pl; static VkPipeline pipe1, pipeb, pipebc; static VkDescriptorPool dpool; static VkDescriptorSet dset;
/* command buffers in rotation: one is recorded while earlier ones may still run (gsvk_submit); gsvk_finish waits for all.
   A buffer is submitted every few batches (HG_GSVK_EAGER, default 2) so that the GPU works while the GS thread records
   the rest; otherwise it would sit idle until the next read-back and the GS thread would then wait for all of it. */
enum { NCMD = 32 };
static int eager = -1, nb_open;
static VkCommandPool cpool; static VkCommandBuffer cmds[NCMD]; static VkFence fences[NCMD]; static int busy[NCMD]; static uint64_t sub_id[NCMD], n_sub_id; static int ci;
#define cmd (cmds[ci])
static VkQueryPool qpool; static float tsperiod;
/* GPU time: a timestamp opens and closes each command buffer. With HG_GSVK_TIMING=1 one more is written wherever the
   kind of work changes (phase()), and the time between two is counted for the phase in between, separately for the
   replays of frame interpolation (target set != 0); the copies dispatch then waits for the exact one, which it
   otherwise overlaps, so the total grows a little. */
enum { QPER = 1024, PH_EXACT = 0, PH_COPIES, PH_SINGLE, PH_COMPOSE, PH_XFER, PH_N };
static int tim_on, cur_ph; static int qcnt[NCMD]; static int8_t qseg[NCMD][QPER]; static double ph_ms[2 * PH_N];
/* gsvk_tag(): command buffers opened while the tag is on count their GPU time separately too (frame interpolation's
   replays and snapshot, for the adaptive policy in gs.c) */
static int tag_cur, tag_of[NCMD]; static double gpu_ms_tag;
typedef struct { VkBuffer b; VkDeviceMemory m; void *p; VkDeviceSize n; } Buf;
static Buf vram, dvram, xoffb, tabb, primb, clutb, upb, hrb;   /* hrb: the upscaled display read back */
static VkDeviceSize outbase, outbytes;   /* in dvram, after the copies */
enum { PRIM_MAX = 65536, CLUT_MAX = 8192, UP_BYTES = 64 << 20 };
/* The primitive, CLUT and upload rings are used in two halves: when the current half is full the work recorded so far
   is submitted and recording goes on in the other half once the GPU is done with that half's last submission (a full
   wait, as before, left the GPU idle while the GS thread recorded the next work). */
static int rh, clut_ok; static uint64_t half_id[2];
static int state;   /* 0 untried, 1 ready, -1 unavailable */
/* presenting from video memory (the end of this file): the UI thread's queue (the family's second, or the same one
   under qmx), the timeline semaphore every GS submission signals with its id */
static VkQueue uqueue; static int shared_q, can_present; static pthread_mutex_t qmx = PTHREAD_MUTEX_INITIALIZER; static VkSemaphore tl;
static int skip_exact;   /* gsvk_skip_exact() */
/* recording */
static int open_, bound; static VkPipeline cur_pipe; static uint32_t nprim, nclut, upoff;
static uint32_t bfirst, bn; static int bx0, by0, bx1, by1;   /* the batch being collected */
static int vfresh = 1;   /* the mirror does not hold the GPU's memory */
static uint32_t ncopy = 1;   /* copies of local memory in dvram, 4 MB apart */
static uint32_t mbase_add, up_first, up_count = 1;   /* gsvk_set_target() */
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

/* local memory: ncopies copies of 4 MB, then the out region (6 picture slots), and its host-visible read-back hrb */
static int mk_local(int ncopies) {
    const VkBufferUsageFlags xfer = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, sto = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    ncopy = ncopies < 1 ? 1 : (uint32_t)ncopies; up_first = 0; up_count = ncopy; mbase_add = 0;
    outbase = (VkDeviceSize)ncopy * (1u << 22) + 16;
    outbytes = ncopy > 1 ? (VkDeviceSize)ncopy * 8 * 1024 * 1024 : (VkDeviceSize)32 << 20;
    if (!mkbuf_(&dvram, outbase + outbytes, sto | xfer, 2)) return 0;
    if (!mkbuf_(&hrb, outbytes, xfer, 1) && !mkbuf(&hrb, outbytes, xfer)) return 0;
    return 1;
}
static void free_buf(Buf *b) {
    if (b->b) vkDestroyBuffer(dev, b->b, 0);
    if (b->m) vkFreeMemory(dev, b->m, 0);   /* (unmaps it) */
    memset(b, 0, sizeof *b);
}
static _Atomic int ready = 0;   /* gsvk_ready(): state as other threads may see it, once gsvk_init() is over */
static int init_(const uint32_t *xoff32, const uint32_t *xoff32z, const int32_t *tables, int ncopies);
int gsvk_init(const uint32_t *xoff32, const uint32_t *xoff32z, const int32_t *tables, int ncopies) {
    if (state) return state > 0;
    const int r = init_(xoff32, xoff32z, tables, ncopies);
    atomic_store(&ready, r && state > 0 ? 1 : -1);
    return r;
}
static int init_(const uint32_t *xoff32, const uint32_t *xoff32z, const int32_t *tables, int ncopies) {
    state = -1;
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "hgboot", .apiVersion = VK_API_VERSION_1_2 };
    const char *iext[2] = { "VK_KHR_surface", "VK_KHR_win32_surface" }; uint32_t niext = 0;   /* for the window (gsvk_present_init) */
#ifdef _WIN32
    { uint32_t ne = 0; vkEnumerateInstanceExtensionProperties(0, &ne, 0); VkExtensionProperties *ep = calloc(ne + 1, sizeof *ep);
      if (ep) {
          vkEnumerateInstanceExtensionProperties(0, &ne, ep); int h = 0;
          for (uint32_t i = 0; i < ne; i++) h |= (strcmp(ep[i].extensionName, iext[0]) ? 0 : 1) | (strcmp(ep[i].extensionName, iext[1]) ? 0 : 2);
          if (h == 3) niext = 2;
          free(ep);
      } }
#endif
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .enabledExtensionCount = niext, .ppEnabledExtensionNames = iext };
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
    /* presenting needs the swapchain extension and timeline semaphores (Vulkan 1.2) */
    VkPhysicalDeviceTimelineSemaphoreFeatures tsf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &tsf }; vkGetPhysicalDeviceFeatures2(phys, &f2);
    { uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(phys, 0, &ne, 0); VkExtensionProperties *ep = calloc(ne + 1, sizeof *ep); int sw = 0;
      if (ep) { vkEnumerateDeviceExtensionProperties(phys, 0, &ne, ep); for (uint32_t i = 0; i < ne; i++) sw |= !strcmp(ep[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME); free(ep); }
      can_present = niext == 2 && sw && tsf.timelineSemaphore && pr.apiVersion >= VK_API_VERSION_1_2; }
    const float prio[2] = { 0.5f, 1.0f };   /* the window's few blits go before the GS's work */
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = can_present && qf[qfam].queueCount >= 2 ? 2 : 1, .pQueuePriorities = prio };
    VkPhysicalDeviceFeatures have, want = { 0 }; vkGetPhysicalDeviceFeatures(phys, &have);
    if (!have.shaderFloat64) { L("GSVK: no 64-bit floats in shaders (triangle Z)\n"); return 0; }
    want.shaderFloat64 = VK_TRUE;
    VkPhysicalDeviceTimelineSemaphoreFeatures tsw = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .timelineSemaphore = VK_TRUE };
    const char *dext[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = can_present ? &tsw : 0, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = can_present ? 1 : 0, .ppEnabledExtensionNames = dext, .pEnabledFeatures = &want };
    if (vkCreateDevice(phys, &dci, 0, &dev)) { L("GSVK: no device\n"); return 0; }
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    if (qci.queueCount == 2) vkGetDeviceQueue(dev, qfam, 1, &uqueue); else { uqueue = queue; shared_q = 1; }
    if (can_present) {
        VkSemaphoreTypeCreateInfo sti = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE };
        VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &sti };
        if (vkCreateSemaphore(dev, &sci, 0, &tl)) can_present = 0;
    }
    const VkBufferUsageFlags xfer = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, sto = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const VkDeviceSize vbytes = ((1u << 20) + 4) * 4;
    if (!mkbuf_(&vram, vbytes, xfer, 1)) { vkDestroyBuffer(dev, vram.b, 0); vram.b = 0; L("GSVK: no host-cached memory, mirror uncached\n"); }
    if ((!vram.p && !mkbuf(&vram, vbytes, xfer)) || !mk_local(ncopies) || !mkbuf(&xoffb, 66u * 2048 * 4, sto) ||
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
    for (int k = 0; k < 3; k++) {   /* one primitive; a batch; a batch's upscaling copies */
        VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = k == 2 ? sizeof gs_batchc_spv : k ? sizeof gs_batch_spv : sizeof gs_sprite_spv,
                                         .pCode = k == 2 ? gs_batchc_spv : k ? gs_batch_spv : gs_sprite_spv };
        VkShaderModule sm;
        if (vkCreateShaderModule(dev, &smi, 0, &sm)) { L("GSVK: shader failed\n"); return 0; }
        VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
            .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" } };
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, 0, k == 2 ? &pipebc : k ? &pipeb : &pipe1)) { L("GSVK: pipeline failed\n"); return 0; }
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
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = NCMD };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateCommandPool(dev, &cpci, 0, &cpool)) return 0;
    cbai.commandPool = cpool;
    if (vkAllocateCommandBuffers(dev, &cbai, cmds)) return 0;
    for (int k = 0; k < NCMD; k++) if (vkCreateFence(dev, &fci, 0, &fences[k])) return 0;
    VkQueryPoolCreateInfo qpi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = QPER * NCMD };
    tim_on = getenv("HG_GSVK_TIMING") && atoi(getenv("HG_GSVK_TIMING")) > 0;
    if (vkCreateQueryPool(dev, &qpi, 0, &qpool)) return 0;
    tsperiod = pr.limits.timestampPeriod;
    L("GSVK: Vulkan compute on %s%s\n", pr.deviceName, can_present ? (shared_q ? " (can present the window from video memory; one queue)" : " (can present the window from video memory)") : "");
    state = 1; return 1;
}

/* ------------------------------------------------------------------ recording */
static void barrier(void) {   /* everything before (transfers, dispatches) is visible to everything after */
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, 0, 0, 0);
}
static void slot_wait(int k) {   /* the submission in command buffer k is done; its GPU time counted */
    if (!busy[k]) return;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    if (vkWaitForFences(dev, 1, &fences[k], VK_TRUE, 10000000000ull)) { L("GSVK: wait failed\n"); state = -1; }
    clock_gettime(CLOCK_MONOTONIC, &t1); wait_ms_all += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    static uint64_t ts[QPER]; const int nq = qcnt[k];
    if (nq >= 2 && !vkGetQueryPoolResults(dev, qpool, QPER * k, (uint32_t)nq, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)) {
        gpu_ms_all += (double)(ts[nq - 1] - ts[0]) * tsperiod / 1e6;
        if (tag_of[k]) gpu_ms_tag += (double)(ts[nq - 1] - ts[0]) * tsperiod / 1e6;
        for (int i = 0; i + 1 < nq; i++) if (qseg[k][i] >= 0) ph_ms[qseg[k][i]] += (double)(ts[i + 1] - ts[i]) * tsperiod / 1e6;
    }
    busy[k] = 0;
}
static void rec_open(void) {
    if (open_) return;
    ci = (ci + 1) % NCMD; slot_wait(ci);
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(cmd, 0); vkBeginCommandBuffer(cmd, &cbi);
    vkCmdResetQueryPool(cmd, qpool, QPER * ci, tim_on ? QPER : 2);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, QPER * ci);
    qcnt[ci] = 1; qseg[ci][0] = -1; cur_ph = -1; tag_of[ci] = tag_cur;
    open_ = 1; bound = 0; cur_pipe = VK_NULL_HANDLE; nb_open = 0;
}
static void phase(int ph) {   /* HG_GSVK_TIMING: what is recorded next (see above) */
    if (!tim_on) return;
    ph += up_first ? PH_N : 0;
    if (ph == cur_ph || qcnt[ci] >= QPER - 1) return;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, QPER * ci + (uint32_t)qcnt[ci]);
    qseg[ci][qcnt[ci]++] = (int8_t)ph; cur_ph = ph;
}
static void use(VkPipeline p) {
    if (!bound) { vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset, 0, 0); bound = 1; }
    if (cur_pipe != p) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p); cur_pipe = p; }
}
static uint64_t submit(int wait);
static void close_batch(void) {
    if (!bn) return;
    int x0 = bx0 < 0 ? 0 : bx0, y0 = by0 < 0 ? 0 : by0, x1 = bx1 > 2048 ? 2048 : bx1, y1 = by1 > 2048 ? 2048 : by1;
    if (x1 > x0 && y1 > y0) {
        rec_open(); barrier();
        if (!skip_exact) {
            phase(PH_EXACT); use(pipeb);
            int32_t pc[5] = { (int32_t)bn, x0, y0, (int32_t)bfirst, (int32_t)up_count };   /* a primitive has a copy per copy of the set */
            vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
            vkCmdDispatch(cmd, (uint32_t)(x1 - x0 + 7) / 8, (uint32_t)(y1 - y0 + 7) / 8, 1);   /* the exact copy */
            n_dispatch++;
        }
        if (up_count > 1) {   /* the upscaling copies (their own memory: no barrier), moved by under a pixel: a margin of 2 */
            const int cx0 = x0 >= 2 ? x0 - 2 : 0, cy0 = y0 >= 2 ? y0 - 2 : 0, cx1 = x1 + 2 > 2048 ? 2048 : x1 + 2, cy1 = y1 + 2 > 2048 ? 2048 : y1 + 2;
            if (tim_on) barrier();
            phase(PH_COPIES); use(pipebc);
            int32_t pcc[5] = { (int32_t)bn, cx0, cy0, (int32_t)bfirst, (int32_t)up_count };
            vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pcc, pcc);
            uint32_t N = 1; while ((N + 1) * (N + 1) <= up_count - 1) N++;
            vkCmdDispatch(cmd, ((uint32_t)(cx1 - cx0) * N + 7) / 8, ((uint32_t)(cy1 - cy0) * N + 7) / 8, 1);   /* tiles of the upscaled picture */
            n_dispatch++;
        }
    }
    bn = 0;
    if (eager < 0) eager = getenv("HG_GSVK_EAGER") ? atoi(getenv("HG_GSVK_EAGER")) : 2;
    if (open_ && eager > 0 && ++nb_open >= eager) submit(0);
}
void gsvk_break(void) { if (state > 0) close_batch(); }
/* submit what is recorded; with wait, wait for everything submitted, and the rings start over. Returns the
   submission's id (for gsvk_wait), 0 if nothing was recorded. */
static uint64_t submit(int wait) {
    if (state <= 0) return 0;
    close_batch();
    uint64_t id = 0;
    if (open_) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, QPER * ci + (uint32_t)qcnt[ci]++);
        vkEndCommandBuffer(cmd); open_ = 0;
        id = n_sub_id + 1;
        VkTimelineSemaphoreSubmitInfo tsi = { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &id };
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = tl ? &tsi : 0, .commandBufferCount = 1, .pCommandBuffers = &cmd,
                            .signalSemaphoreCount = tl ? 1 : 0, .pSignalSemaphores = &tl };
        vkResetFences(dev, 1, &fences[ci]);
        if (shared_q) pthread_mutex_lock(&qmx);
        const VkResult r = vkQueueSubmit(queue, 1, &si, fences[ci]);
        if (shared_q) pthread_mutex_unlock(&qmx);
        if (r) { L("GSVK: submit failed\n"); state = -1; return 0; }
        busy[ci] = 1; sub_id[ci] = id; n_sub_id = id; n_submits++;
    }
    if (wait) { for (int k = 0; k < NCMD; k++) slot_wait(k); nprim = nclut = 0; upoff = 0; rh = 0; clut_ok = 0; }
    return id;
}
static void ring_switch(void) {
    submit(0); half_id[rh] = n_sub_id; rh ^= 1;
    gsvk_wait(half_id[rh]);
    nprim = rh * (PRIM_MAX / 2); nclut = rh * (CLUT_MAX / 2); upoff = rh * (UP_BYTES / 2); clut_ok = 0;
}
void gsvk_finish(void) { submit(1); }
uint64_t gsvk_submit(void) { return submit(0); }
void gsvk_wait(uint64_t id) { for (int k = 0; k < NCMD; k++) if (busy[k] && sub_id[k] <= id) slot_wait(k); }
int gsvk_poll(uint64_t id) {   /* that submission (and earlier) done? never waits (the finished ones are collected) */
    if (state <= 0) return 1;
    int done = 1;
    for (int k = 0; k < NCMD; k++) if (busy[k] && sub_id[k] <= id) { if (vkGetFenceStatus(dev, fences[k]) == VK_SUCCESS) slot_wait(k); else done = 0; }
    return done;
}
void gsvk_tag(int on) { tag_cur = on; }
double gsvk_tag_ms(void) { return gpu_ms_tag; }
static void ring_switch(void);
/* local memory words [w, w+n) from src, at this point of the stream */
void gsvk_upload(uint32_t w, uint32_t n, const uint32_t *src) {
    if (state <= 0 || !n) return;
    close_batch();
    if (upoff + n * 4 > (uint32_t)(rh + 1) * (UP_BYTES / 2)) ring_switch();
    memcpy((uint8_t *)upb.p + upoff, src, (size_t)n * 4);
    rec_open(); barrier(); phase(PH_XFER);
    VkBufferCopy c0 = { upoff, (VkDeviceSize)up_first * (1u << 22) + (VkDeviceSize)w * 4, (VkDeviceSize)n * 4 };
    vkCmdCopyBuffer(cmd, upb.b, dvram.b, 1, &c0);   /* over the bus once, into the set's first copy... */
    if (up_count > 1) {   /* ...then from there into the others, in video memory */
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, 0, 0, 0);
        VkBufferCopy c[64]; uint32_t nc = 0;
        for (uint32_t k = 1; k < up_count && nc < 64; k++) c[nc++] = (VkBufferCopy){ c0.dstOffset, (VkDeviceSize)(up_first + k) * (1u << 22) + (VkDeviceSize)w * 4, (VkDeviceSize)n * 4 };
        vkCmdCopyBuffer(cmd, dvram.b, dvram.b, nc, c);
    }
    upoff += (n * 4 + 255) & ~255u;
}
/* one primitive: alone (lines, serial ones) or added to the batch being collected */
void gsvk_prim(const GsVkSprite *s, const uint32_t *clut, int alone) {
    if (state <= 0) return;
    if (s->mbase == 0 && (nprim + up_count > (uint32_t)(rh + 1) * (PRIM_MAX / 2) || nclut == (uint32_t)(rh + 1) * (CLUT_MAX / 2))) ring_switch();   /* a primitive's copies stay together */
    static uint32_t last[256];   /* the ring may be video memory mapped over the bus: never read it back */
    if (!clut_ok || memcmp(last, clut, 1024)) { memcpy(last, clut, 1024); memcpy((uint32_t *)clutb.p + nclut++ * 256, clut, 1024); clut_ok = 1; }
    GsVkSprite r = *s; r.clutoff = (nclut - 1) * 256; r.mbase += mbase_add;
    ((GsVkSprite *)primb.p)[nprim] = r;   /* written once: the ring may be video memory over the bus, never read it */
    if (alone && skip_exact && !(s->mbase >> 20) && s->prim != 4) { nprim++; return; }   /* the set's exact copy is not drawn */
    if (alone) {
        close_batch(); rec_open(); barrier(); phase(s->prim == 4 ? PH_COMPOSE : PH_SINGLE); use(pipe1);
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
    rec_open(); barrier(); phase(PH_XFER);
    vkCmdCopyBuffer(cmd, dvram.b, vram.b, nc, c);
    for (uint32_t i = 0; i < nc; i++) c[i].srcOffset -= (VkDeviceSize)k * (1u << 22);
    VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, 0, 0, 0);
    gsvk_finish();
    for (uint32_t i = 0; i < nc; i++) memcpy((uint8_t *)dst + c[i].srcOffset, (uint8_t *)vram.p + c[i].srcOffset, c[i].size);
    vfresh = 1;   /* the mirror no longer holds the whole of the GPU's memory */
}
void gsvk_skip_exact(int on) { if (state > 0) { close_batch(); skip_exact = on; } }
void gsvk_set_target(uint32_t first, uint32_t count) {
    if (state <= 0) return;
    close_batch();   /* a batch draws into one set */
    if (first + count > ncopy) return;
    mbase_add = first << 20; up_first = first; up_count = count;
}
/* a different number of copies (the upscaling factor or frame interpolation changed while the game runs): all GPU
   work is finished and local memory is made again at the new size. Its contents are gone: the caller uploads local
   memory again. The window's picture images are separate and stay. */
int gsvk_resize(int ncopies) {
    if (state <= 0) return 0;
    submit(1);
    const uint32_t old = ncopy;
    free_buf(&dvram); free_buf(&hrb);
    if (!mk_local(ncopies)) {
        L("GSVK: no video memory for %d copies of local memory; back to %u\n", ncopies, old);
        free_buf(&dvram); free_buf(&hrb);
        if (!mk_local((int)old)) { L("GSVK: local memory lost\n"); state = -1; return 0; }
    }
    VkDescriptorBufferInfo bi = { dvram.b, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0, .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi };
    vkUpdateDescriptorSets(dev, 1, &wr, 0, 0);
    vfresh = 1;
    return ncopy == (uint32_t)ncopies;
}
void gsvk_copy_copies(uint32_t dst, uint32_t src, uint32_t count) {
    if (state <= 0 || dst + count > ncopy || src + count > ncopy || !count) return;
    close_batch(); rec_open(); barrier(); phase(PH_XFER);
    VkBufferCopy c = { (VkDeviceSize)src << 22, (VkDeviceSize)dst << 22, (VkDeviceSize)count << 22 };
    vkCmdCopyBuffer(cmd, dvram.b, dvram.b, 1, &c);
}
/* the out region holds 6 pictures (slots); compose_rec records one, compose_fetch reads it once its submission is done */
int gsvk_compose_rec(const GsVkSprite *s0, int slot) {
    if (state <= 0 || !outbytes || slot < 0 || slot > 5) return 0;
    GsVkSprite s = *s0;
    const VkDeviceSize n = (VkDeviceSize)s.px1 * s.py1 * 4, slotb = (outbytes / 6) & ~(VkDeviceSize)255;
    if (n > slotb || s.px1 <= 0 || s.py1 <= 0) return 0;
    s.prim = 4; s.px0 = s.py0 = 0; s.serial = 0; s.z = (uint32_t)((outbase + slot * slotb) / 4); s.mbase = 0;
    const uint32_t keep = mbase_add; mbase_add = 0;   /* the compose primitive reads copies by itself */
    static const uint32_t noclut[256];
    gsvk_prim(&s, noclut, 1); mbase_add = keep;
    rec_open(); barrier(); phase(PH_XFER);
    VkBufferCopy c = { outbase + slot * slotb, slot * slotb, n };
    vkCmdCopyBuffer(cmd, dvram.b, hrb.b, 1, &c);
    VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, 0, 0, 0);
    return 1;
}
void gsvk_compose_fetch(int slot, uint32_t *dst, size_t words) {
    const VkDeviceSize slotb = (outbytes / 6) & ~(VkDeviceSize)255;
    if (state > 0 && slot >= 0 && slot < 6 && words * 4 <= slotb) memcpy(dst, (uint8_t *)hrb.p + slot * slotb, words * 4);
}
int gsvk_compose(const GsVkSprite *s0, uint32_t *dst) {
    if (!gsvk_compose_rec(s0, 0)) return 0;
    gsvk_finish();
    if (state <= 0) return 0;
    gsvk_compose_fetch(0, dst, (size_t)s0->px1 * s0->py1);
    return 1;
}
void gsvk_stats(uint64_t *submits, uint64_t *dispatches, double *gpu_ms, double *wait_ms) { *submits = n_submits; *dispatches = n_dispatch; *gpu_ms = gpu_ms_all; *wait_ms = wait_ms_all; }
void gsvk_phase_stats(double ms[10]) { for (int i = 0; i < 2 * PH_N; i++) ms[i] = ph_ms[i]; }

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

/* ------------------------------------------------------------------ presenting from video memory
   The window's pictures stay on the GPU. gsvk_pic() (GS thread) composes a display picture, as gsvk_compose() does,
   and copies it into one of NPIC picture images; the UI thread's gsvk_present() blits a picture image into the
   window's swapchain (scaled, letterboxed like SDL_RenderSetLogicalSize(640, 448) did) after the GS submission that
   made it, which every GS submission signals on the timeline semaphore tl with its id. The UI thread records into its
   own command buffers and submits to its own queue. A picture image is free again (pic_free) once the UI thread has
   moved on to a newer picture and its blits of it are done, or when the picture is dropped unseen. No picture crosses
   the bus, and the GS thread never waits for the GPU because of the window. */
enum { NPIC = 12, NFR = 3, OVW = 176, OVH = 42 };
static struct { VkImage img; VkDeviceMemory mem; int w, h; } pic[NPIC];
static _Atomic uint32_t pic_free = (1u << NPIC) - 1;
static _Atomic int pres_ok;
static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
static int mkimg(VkImage *img, VkDeviceMemory *mem, int w, int h) {   /* RGBA, device local, a transfer source and target */
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { (uint32_t)w, (uint32_t)h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(dev, &ii, 0, img)) return 0;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, *img, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &ai, 0, mem)) { vkDestroyImage(dev, *img, 0); *img = 0; return 0; }
    vkBindImageMemory(dev, *img, *mem, 0); return 1;
}
static void imgbar(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sa, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, 0, 0, 0, 1, &b);
}
int gsvk_ready(void) { return atomic_load(&ready); }
int gsvk_present_ready(void) { return atomic_load(&pres_ok); }
uint64_t gsvk_done(void) { uint64_t v = 0; if (tl) vkGetSemaphoreCounterValue(dev, tl, &v); return v; }
uint64_t gsvk_last(void) { return n_sub_id; }
void gsvk_pic_release(int k) { if (k >= 0 && k < NPIC) atomic_fetch_or(&pic_free, 1u << k); }
/* GS thread: the picture s (as for gsvk_compose: px1 x py1) into a free picture image; its index and, in *id, the
   submission that makes it; -1 when there is none (no window, or no image came free within a second) */
int gsvk_pic(const GsVkSprite *s0, uint64_t *id) {
    if (state <= 0 || !atomic_load(&pres_ok)) return -1;
    const int W = s0->px1, H = s0->py1;
    const VkDeviceSize slotb = (outbytes / 6) & ~(VkDeviceSize)255, area = outbase + 3 * slotb;   /* the out region's slot 3 */
    if (W <= 0 || H <= 0 || (VkDeviceSize)W * H * 4 > slotb) return -1;
    int k = -1;
    for (int tries = 0; k < 0 && tries < 2000; tries++) {
        uint32_t f = atomic_load(&pic_free);
        if (!f) { struct timespec ts = { 0, 500000 }; nanosleep(&ts, 0); continue; }
        const int b = __builtin_ctz(f);
        if (atomic_compare_exchange_weak(&pic_free, &f, f & ~(1u << b))) k = b;
    }
    if (k < 0) return -1;
    if (pic[k].w != W || pic[k].h != H) {   /* free, so no UI work uses it */
        if (pic[k].img) { vkDestroyImage(dev, pic[k].img, 0); vkFreeMemory(dev, pic[k].mem, 0); pic[k].img = 0; }
        if (!mkimg(&pic[k].img, &pic[k].mem, W, H)) { pic[k].w = 0; gsvk_pic_release(k); return -1; }
        pic[k].w = W; pic[k].h = H;
    }
    GsVkSprite s = *s0; s.prim = 4; s.px0 = s.py0 = 0; s.serial = 0; s.z = (uint32_t)(area / 4); s.mbase = 0;
    const uint32_t keep = mbase_add; mbase_add = 0;   /* the compose primitive reads copies by itself */
    const int skip = skip_exact; skip_exact = 0;
    static const uint32_t noclut[256];
    gsvk_prim(&s, noclut, 1); mbase_add = keep; skip_exact = skip;
    rec_open(); barrier(); phase(PH_COMPOSE);
    imgbar(cmd, pic[k].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy r = { .bufferOffset = area, .bufferRowLength = (uint32_t)W, .bufferImageHeight = (uint32_t)H,
                            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { (uint32_t)W, (uint32_t)H, 1 } };
    vkCmdCopyBufferToImage(cmd, dvram.b, pic[k].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    imgbar(cmd, pic[k].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    *id = submit(0);
    if (!*id) { gsvk_pic_release(k); return -1; }
    return k;
}

/* the UI thread's side */
static VkSurfaceKHR surf; static VkSwapchainKHR sc; static VkFormat scfmt; static VkColorSpaceKHR sccs; static VkExtent2D scext; static VkPresentModeKHR pmode;
static int pm_has[4];                    /* present modes the surface offers */
static _Atomic int pm_want = 0, pm_set = -1;   /* gsvk_present_mode(): 0 mailbox, 1 FIFO (vsync), 2 immediate */
void gsvk_present_mode(int v) { atomic_store(&pm_want, v); }
static VkPresentModeKHR pick_mode(int v) {
    const VkPresentModeKHR want = v == 1 ? VK_PRESENT_MODE_FIFO_KHR : v == 2 ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_MAILBOX_KHR;
    return pm_has[want] ? want : pm_has[VK_PRESENT_MODE_MAILBOX_KHR] ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
}
static const char *mode_name(VkPresentModeKHR m) { return m == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : m == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate"; }
static uint32_t scn; static VkImage scimg[8]; static VkSemaphore scdone[8]; static int sc_stale;
static VkCommandPool upool; static VkCommandBuffer ucb[NFR]; static VkFence ufence[NFR]; static VkSemaphore uacq[NFR]; static int ufr, usub[NFR], uslot[NFR];
static int ucur = -1, uref[NPIC]; static uint64_t ucur_id;
static Buf ovbuf; static VkImage ovimg; static VkDeviceMemory ovmem;
static void ui_unref(int k) { if (k >= 0 && --uref[k] == 0) gsvk_pic_release(k); }
static int mk_swapchain(void) {
    VkSurfaceCapabilitiesKHR cap; if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &cap)) return 0;
    if (!cap.currentExtent.width || !cap.currentExtent.height) return 0;   /* minimized */
    if (!(cap.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { L("GSVK: the window's images cannot be blitted into\n"); return 0; }
    if (shared_q) pthread_mutex_lock(&qmx);
    vkQueueWaitIdle(uqueue);   /* the old swapchain's images and semaphores are no longer in use */
    if (shared_q) pthread_mutex_unlock(&qmx);
    uint32_t n = cap.minImageCount + 1; if (cap.maxImageCount && n > cap.maxImageCount) n = cap.maxImageCount;
    VkCompositeAlphaFlagBitsKHR ca = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(cap.supportedCompositeAlpha & ca)) for (uint32_t b = 1; b; b <<= 1) if (cap.supportedCompositeAlpha & b) { ca = (VkCompositeAlphaFlagBitsKHR)b; break; }
    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf, .minImageCount = n, .imageFormat = scfmt, .imageColorSpace = sccs,
        .imageExtent = cap.currentExtent, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = cap.currentTransform, .compositeAlpha = ca, .presentMode = pmode, .clipped = VK_TRUE, .oldSwapchain = sc };
    VkSwapchainKHR nsc;
    if (vkCreateSwapchainKHR(dev, &ci, 0, &nsc)) { L("GSVK: no swapchain\n"); return 0; }
    if (sc) vkDestroySwapchainKHR(dev, sc, 0);
    for (uint32_t i = 0; i < scn; i++) vkDestroySemaphore(dev, scdone[i], 0);
    sc = nsc; scext = cap.currentExtent; scn = 8; vkGetSwapchainImagesKHR(dev, sc, &scn, scimg);
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < scn; i++) vkCreateSemaphore(dev, &sci, 0, &scdone[i]);
    sc_stale = 0; return 1;
}
/* UI thread: a swapchain on the window (Windows: its HINSTANCE and HWND); 0 if presenting from video memory is not
   possible (the caller then draws with SDL) */
int gsvk_present_init(void *hinstance, void *hwnd) {
    if (gsvk_ready() <= 0 || !can_present) return 0;
#ifdef _WIN32
    typedef struct { VkStructureType sType; const void *pNext; VkFlags flags; void *hinstance, *hwnd; } W32SurfaceInfo;   /* VkWin32SurfaceCreateInfoKHR */
    typedef VkResult (VKAPI_PTR *CreateW32Surface)(VkInstance, const W32SurfaceInfo *, const VkAllocationCallbacks *, VkSurfaceKHR *);
    CreateW32Surface cs = (CreateW32Surface)vkGetInstanceProcAddr(inst, "vkCreateWin32SurfaceKHR");
    W32SurfaceInfo wi = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, 0, 0, hinstance, hwnd };
    if (!cs || cs(inst, &wi, 0, &surf)) { L("GSVK: no surface for the window\n"); return 0; }
#else
    (void)hinstance; (void)hwnd; return 0;
#endif
    VkBool32 sup = 0; vkGetPhysicalDeviceSurfaceSupportKHR(phys, qfam, surf, &sup);
    if (!sup) { L("GSVK: the queue cannot present to the window\n"); return 0; }
    VkSurfaceFormatKHR fm[64]; uint32_t nf = 64; vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nf, fm);
    if (!nf) return 0;
    scfmt = fm[0].format; sccs = fm[0].colorSpace;   /* a UNORM format: the picture's values go to the screen as they are */
    for (uint32_t i = 0; i < nf; i++) if (fm[i].format == VK_FORMAT_B8G8R8A8_UNORM || fm[i].format == VK_FORMAT_R8G8B8A8_UNORM) { scfmt = fm[i].format; sccs = fm[i].colorSpace; break; }
    /* Mailbox: the picture replaces any not yet shown and is shown at the display's next refresh, no tearing; the UI
       thread's own clock (59.94 Hz) decides when. FIFO (HG_VSYNC=1) stalled the UI thread's submissions 60-200 ms at a
       time in a window on the 120 Hz screen, even at native resolution. HG_VSYNC=0: immediate (may tear). */
    { VkPresentModeKHR pm[16]; uint32_t np = 16; vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surf, &np, pm);   /* (vsync: settings.c, HG_VSYNC) */
      for (uint32_t i = 0; i < np; i++) if ((uint32_t)pm[i] < 4) pm_has[pm[i]] = 1;
      pm_set = atomic_load(&pm_want); pmode = pick_mode(pm_set); }
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam };
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = NFR };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (vkCreateCommandPool(dev, &cpci, 0, &upool)) return 0;
    cbai.commandPool = upool; if (vkAllocateCommandBuffers(dev, &cbai, ucb)) return 0;
    for (int f = 0; f < NFR; f++) { if (vkCreateFence(dev, &fci, 0, &ufence[f]) || vkCreateSemaphore(dev, &sci, 0, &uacq[f])) return 0; uslot[f] = -1; }
    if (!mkbuf(&ovbuf, (VkDeviceSize)NFR * OVW * OVH * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) || !mkimg(&ovimg, &ovmem, OVW, OVH)) return 0;
    if (!mk_swapchain()) sc_stale = 1;
    L("GSVK: the window is presented from video memory (%s)\n", mode_name(pmode));
    atomic_store(&pres_ok, 1); return 1;
}
/* UI thread: show picture k (k < 0: the current one again) once GS submission id is done, with the overlay ov
   (OVW x OVH, 0x00BBGGRR, or 0) at the picture's top left */
void gsvk_present(int k, uint64_t id, const uint32_t *ov) {
    if (!atomic_load(&pres_ok)) return;
    if (k >= 0 && k < NPIC) { uref[k]++; ui_unref(ucur); ucur = k; ucur_id = id; }
    const int f = ufr;
    if (usub[f]) { vkWaitForFences(dev, 1, &ufence[f], VK_TRUE, UINT64_MAX); usub[f] = 0; ui_unref(uslot[f]); uslot[f] = -1; }
    { const int w = atomic_load(&pm_want);   /* vsync changed (the Options menu): a new swapchain in that mode */
      if (w != pm_set) { pm_set = w; const VkPresentModeKHR m = pick_mode(w); if (m != pmode) { pmode = m; sc_stale = 1; L("GSVK: presenting with %s\n", mode_name(m)); } } }
    if (sc_stale || !sc) { if (!mk_swapchain()) return; }
    uint32_t ii;
    VkResult r = vkAcquireNextImageKHR(dev, sc, 100000000ull, uacq[f], VK_NULL_HANDLE, &ii);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { sc_stale = 1; return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
    if (r == VK_SUBOPTIMAL_KHR) sc_stale = 1;
    VkCommandBuffer cb = ucb[f];
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(cb, 0); vkBeginCommandBuffer(cb, &cbi);
    const VkImage dst = scimg[ii];
    imgbar(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue black = { { 0, 0, 0, 1 } }; const VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    imgbar(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    /* the picture in a 640:448 rectangle as large as the window allows, centred */
    const double sx = scext.width / 640.0, sy = scext.height / 448.0, s = sx < sy ? sx : sy;
    const int rw = (int)(640 * s + 0.5), rh = (int)(448 * s + 0.5), x0 = ((int)scext.width - rw) / 2, y0 = ((int)scext.height - rh) / 2;
    if (ucur >= 0) {
        VkImageBlit b = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffsets = { { 0, 0, 0 }, { pic[ucur].w, pic[ucur].h, 1 } },
                          .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstOffsets = { { x0, y0, 0 }, { x0 + rw, y0 + rh, 1 } } };
        vkCmdBlitImage(cb, pic[ucur].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    }
    if (ov) {   /* the F3 overlay: 4 picture pixels from the corner, scaled with the picture */
        memcpy((uint8_t *)ovbuf.p + (size_t)f * OVW * OVH * 4, ov, (size_t)OVW * OVH * 4);
        imgbar(cb, ovimg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy r2 = { .bufferOffset = (VkDeviceSize)f * OVW * OVH * 4, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { OVW, OVH, 1 } };
        vkCmdCopyBufferToImage(cb, ovbuf.b, ovimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r2);
        imgbar(cb, ovimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        const double k2 = rw / 640.0; const int ox = x0 + (int)(4 * k2), oy = y0 + (int)(4 * k2);
        VkImageBlit b = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffsets = { { 0, 0, 0 }, { OVW, OVH, 1 } },
                          .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstOffsets = { { ox, oy, 0 }, { ox + (int)(OVW * k2), oy + (int)(OVH * k2), 1 } } };
        vkCmdBlitImage(cb, ovimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    }
    imgbar(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(cb);
    const VkSemaphore ws[2] = { uacq[f], tl }; const uint64_t wv[2] = { 0, ucur_id }; const uint64_t sv = 0;
    const VkPipelineStageFlags wst[2] = { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT };
    VkTimelineSemaphoreSubmitInfo tsi = { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = ucur >= 0 ? 2 : 1, .pWaitSemaphoreValues = wv,
                                          .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &sv };
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tsi, .waitSemaphoreCount = ucur >= 0 ? 2 : 1, .pWaitSemaphores = ws, .pWaitDstStageMask = wst,
                        .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &scdone[ii] };
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &scdone[ii], .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &ii };
    vkResetFences(dev, 1, &ufence[f]);
    if (shared_q) pthread_mutex_lock(&qmx);
    r = vkQueueSubmit(uqueue, 1, &si, ufence[f]);
    const VkResult pr = r ? r : vkQueuePresentKHR(uqueue, &pi);
    if (shared_q) pthread_mutex_unlock(&qmx);
    if (r) { L("GSVK: window submit failed (%d)\n", (int)r); atomic_store(&pres_ok, 0); return; }
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) sc_stale = 1;
    usub[f] = 1; uslot[f] = ucur; if (ucur >= 0) uref[ucur]++;
    ufr = (f + 1) % NFR;
}
/* UI thread, before the process ends: nothing more is presented, the GPU is done with the window */
void gsvk_present_shutdown(void) {
    if (!atomic_load(&pres_ok)) return;
    atomic_store(&pres_ok, 0);
    for (int f = 0; f < NFR; f++) if (usub[f]) vkWaitForFences(dev, 1, &ufence[f], VK_TRUE, 1000000000ull);
    if (shared_q) pthread_mutex_lock(&qmx);
    vkQueueWaitIdle(uqueue);
    if (shared_q) pthread_mutex_unlock(&qmx);
    if (sc) vkDestroySwapchainKHR(dev, sc, 0);
    vkDestroySurfaceKHR(inst, surf, 0); sc = 0; surf = 0;
}
