#include "vulkan_isp.h"

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>
#include <vector>

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

// SPIR-V binaries embedded by the build system (see CMakeLists.txt).
// glslc -mfmt=c emits the surrounding `{...}` itself.
static const uint32_t kUnpackSpv[] =
#include "shaders/unpack.comp.h"
;
static const uint32_t kDemosaicSpv[] =
#include "shaders/demosaic.comp.h"
;

#define VK_CHECK(expr) do {                                                 \
    VkResult _r = (expr);                                                   \
    if (_r != VK_SUCCESS) {                                                 \
        fprintf(stderr, "VulkanIsp: %s -> VkResult %d\n", #expr, (int)_r);  \
        return false;                                                       \
    }                                                                       \
} while (0)

namespace {

struct PushUnpack {
    int32_t raw_w, raw_h, stride, ds, out_w, out_h;
    float   r_gain, b_gain;
    int32_t bayer;
};
struct PushDemosaic {
    int32_t width, height, bayer;
};

struct Buffer {
    VkBuffer        buf  = VK_NULL_HANDLE;
    VkDeviceMemory  mem  = VK_NULL_HANDLE;
    VkDeviceSize    size = 0;
    void*           mapped = nullptr; // null if not host-visible
};

uint32_t find_memory_type(VkPhysicalDeviceMemoryProperties const& mp,
                          uint32_t type_bits,
                          VkMemoryPropertyFlags required,
                          VkMemoryPropertyFlags preferred = 0) {
    uint32_t best = UINT32_MAX;
    int best_score = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(type_bits & (1u << i))) continue;
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & required) != required) continue;
        int score = __builtin_popcount(f & preferred);
        if (score > best_score) { best = i; best_score = score; }
    }
    return best;
}

#if defined(__aarch64__) || defined(__ARM_NEON)
void rgba_to_rgb_neon(const uint8_t* src, uint8_t* dst, size_t pixels) {
    size_t i = 0;
    for (; i + 16 <= pixels; i += 16) {
        uint8x16x4_t rgba = vld4q_u8(src + i * 4);
        uint8x16x3_t rgb = { { rgba.val[0], rgba.val[1], rgba.val[2] } };
        vst3q_u8(dst + i * 3, rgb);
    }
    for (; i < pixels; i++) {
        dst[i * 3 + 0] = src[i * 4 + 0];
        dst[i * 3 + 1] = src[i * 4 + 1];
        dst[i * 3 + 2] = src[i * 4 + 2];
    }
}
#else
void rgba_to_rgb_neon(const uint8_t* src, uint8_t* dst, size_t pixels) {
    for (size_t i = 0; i < pixels; i++) {
        dst[i * 3 + 0] = src[i * 4 + 0];
        dst[i * 3 + 1] = src[i * 4 + 1];
        dst[i * 3 + 2] = src[i * 4 + 2];
    }
}
#endif

} // namespace

struct VulkanIsp::Impl {
    VkInstance       instance        = VK_NULL_HANDLE;
    VkPhysicalDevice phys            = VK_NULL_HANDLE;
    VkDevice         device          = VK_NULL_HANDLE;
    VkQueue          queue           = VK_NULL_HANDLE;
    uint32_t         queue_family    = 0;

    VkPhysicalDeviceMemoryProperties mem_props{};

    VkCommandPool    cmd_pool        = VK_NULL_HANDLE;
    VkCommandBuffer  cmd_buf         = VK_NULL_HANDLE;
    VkFence          fence           = VK_NULL_HANDLE;

    VkShaderModule   sm_unpack       = VK_NULL_HANDLE;
    VkShaderModule   sm_demosaic     = VK_NULL_HANDLE;

    VkDescriptorSetLayout dsl        = VK_NULL_HANDLE;
    VkPipelineLayout      pl_unpack  = VK_NULL_HANDLE;
    VkPipelineLayout      pl_demosaic= VK_NULL_HANDLE;
    VkPipeline            pp_unpack  = VK_NULL_HANDLE;
    VkPipeline            pp_demosaic= VK_NULL_HANDLE;

    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet  descriptor_set  = VK_NULL_HANDLE;

    Buffer raw_buf{};   // input (host-visible)
    Buffer bayer_buf{}; // intermediate (device-local OK; we use UMA)
    Buffer rgba_buf{};  // output  (host-visible)

    // ---- helpers ----
    bool create_buffer(VkDeviceSize size,
                       VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags req,
                       VkMemoryPropertyFlags pref,
                       Buffer& out) {
        VkBufferCreateInfo bi{};
        bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size        = size;
        bi.usage       = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(device, &bi, nullptr, &out.buf));

        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(device, out.buf, &mr);

        uint32_t type = find_memory_type(mem_props, mr.memoryTypeBits, req, pref);
        if (type == UINT32_MAX) {
            fprintf(stderr, "VulkanIsp: no suitable memory type\n");
            return false;
        }

        VkMemoryAllocateInfo ai{};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = mr.size;
        ai.memoryTypeIndex = type;
        VK_CHECK(vkAllocateMemory(device, &ai, nullptr, &out.mem));
        VK_CHECK(vkBindBufferMemory(device, out.buf, out.mem, 0));

        out.size = size;
        if (mem_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            VK_CHECK(vkMapMemory(device, out.mem, 0, VK_WHOLE_SIZE, 0, &out.mapped));
        }
        return true;
    }

    void destroy_buffer(Buffer& b) {
        if (b.mapped) { vkUnmapMemory(device, b.mem); b.mapped = nullptr; }
        if (b.buf)    { vkDestroyBuffer(device, b.buf, nullptr); b.buf = VK_NULL_HANDLE; }
        if (b.mem)    { vkFreeMemory(device, b.mem, nullptr);   b.mem = VK_NULL_HANDLE; }
        b.size = 0;
    }

    bool create_shader(const uint32_t* code, size_t bytes, VkShaderModule& sm) {
        VkShaderModuleCreateInfo ci{};
        ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = bytes;
        ci.pCode    = code;
        VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &sm));
        return true;
    }
};

// ---------------------------------------------------------------------------
// VulkanIsp: lifecycle
// ---------------------------------------------------------------------------

VulkanIsp::VulkanIsp() : d_(new Impl) {}

VulkanIsp::~VulkanIsp() { shutdown(); delete d_; d_ = nullptr; }

bool VulkanIsp::init() {
    if (available_) return true;

    // --- Instance ---
    VkApplicationInfo app{};
    app.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "fajita-webcam";
    app.apiVersion       = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ici{};
    ici.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, nullptr, &d_->instance) != VK_SUCCESS) {
        fprintf(stderr, "VulkanIsp: vkCreateInstance failed\n");
        return false;
    }

    // --- Pick first device with a compute queue ---
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(d_->instance, &n, nullptr);
    if (n == 0) { fprintf(stderr, "VulkanIsp: no Vulkan devices\n"); shutdown(); return false; }
    std::vector<VkPhysicalDevice> phys(n);
    vkEnumeratePhysicalDevices(d_->instance, &n, phys.data());

    for (auto p : phys) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, qf.data());
        for (uint32_t i = 0; i < qn; i++) {
            if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                d_->phys         = p;
                d_->queue_family = i;
                break;
            }
        }
        if (d_->phys) break;
    }
    if (!d_->phys) {
        fprintf(stderr, "VulkanIsp: no compute queue\n");
        shutdown();
        return false;
    }

    VkPhysicalDeviceProperties pprops;
    vkGetPhysicalDeviceProperties(d_->phys, &pprops);
    vkGetPhysicalDeviceMemoryProperties(d_->phys, &d_->mem_props);

    // --- Logical device + queue ---
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = d_->queue_family;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos    = &qci;
    if (vkCreateDevice(d_->phys, &dci, nullptr, &d_->device) != VK_SUCCESS) {
        fprintf(stderr, "VulkanIsp: vkCreateDevice failed\n");
        shutdown();
        return false;
    }
    vkGetDeviceQueue(d_->device, d_->queue_family, 0, &d_->queue);

    // --- Command pool / buffer / fence ---
    VkCommandPoolCreateInfo cpi{};
    cpi.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = d_->queue_family;
    cpi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(d_->device, &cpi, nullptr, &d_->cmd_pool) != VK_SUCCESS) {
        fprintf(stderr, "VulkanIsp: vkCreateCommandPool failed\n");
        shutdown();
        return false;
    }
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = d_->cmd_pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(d_->device, &cbai, &d_->cmd_buf) != VK_SUCCESS) {
        fprintf(stderr, "VulkanIsp: vkAllocateCommandBuffers failed\n");
        shutdown();
        return false;
    }
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(d_->device, &fci, nullptr, &d_->fence) != VK_SUCCESS) {
        fprintf(stderr, "VulkanIsp: vkCreateFence failed\n");
        shutdown();
        return false;
    }

    // --- Shader modules ---
    if (!d_->create_shader(kUnpackSpv,   sizeof(kUnpackSpv),   d_->sm_unpack))   { shutdown(); return false; }
    if (!d_->create_shader(kDemosaicSpv, sizeof(kDemosaicSpv), d_->sm_demosaic)) { shutdown(); return false; }

    // --- Descriptor set layout: 3 storage buffers ---
    std::array<VkDescriptorSetLayoutBinding, 3> binds{};
    for (uint32_t i = 0; i < 3; i++) {
        binds[i].binding         = i;
        binds[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dsli{};
    dsli.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsli.bindingCount = static_cast<uint32_t>(binds.size());
    dsli.pBindings    = binds.data();
    if (vkCreateDescriptorSetLayout(d_->device, &dsli, nullptr, &d_->dsl) != VK_SUCCESS) {
        fprintf(stderr, "VulkanIsp: vkCreateDescriptorSetLayout failed\n");
        shutdown();
        return false;
    }

    // --- Pipeline layouts (different push-constant sizes) ---
    auto make_pl = [&](size_t pc_size, VkPipelineLayout& out) -> bool {
        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset     = 0;
        pcr.size       = static_cast<uint32_t>(pc_size);

        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &d_->dsl;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pcr;
        return vkCreatePipelineLayout(d_->device, &pli, nullptr, &out) == VK_SUCCESS;
    };
    if (!make_pl(sizeof(PushUnpack),   d_->pl_unpack))   { shutdown(); return false; }
    if (!make_pl(sizeof(PushDemosaic), d_->pl_demosaic)) { shutdown(); return false; }

    // --- Compute pipelines ---
    auto make_pp = [&](VkShaderModule sm, VkPipelineLayout pl, VkPipeline& out) -> bool {
        VkPipelineShaderStageCreateInfo ss{};
        ss.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ss.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        ss.module = sm;
        ss.pName  = "main";

        VkComputePipelineCreateInfo cpi{};
        cpi.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage  = ss;
        cpi.layout = pl;
        return vkCreateComputePipelines(d_->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &out) == VK_SUCCESS;
    };
    if (!make_pp(d_->sm_unpack,   d_->pl_unpack,   d_->pp_unpack))   { shutdown(); return false; }
    if (!make_pp(d_->sm_demosaic, d_->pl_demosaic, d_->pp_demosaic)) { shutdown(); return false; }

    // --- Descriptor pool + set ---
    VkDescriptorPoolSize ps{};
    ps.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = 3;
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes    = &ps;
    dpi.maxSets       = 1;
    if (vkCreateDescriptorPool(d_->device, &dpi, nullptr, &d_->descriptor_pool) != VK_SUCCESS) {
        fprintf(stderr, "VulkanIsp: vkCreateDescriptorPool failed\n");
        shutdown();
        return false;
    }
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = d_->descriptor_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &d_->dsl;
    if (vkAllocateDescriptorSets(d_->device, &dsai, &d_->descriptor_set) != VK_SUCCESS) {
        fprintf(stderr, "VulkanIsp: vkAllocateDescriptorSets failed\n");
        shutdown();
        return false;
    }

    printf("VulkanIsp: initialized on '%s' (driver %u.%u.%u, API %u.%u)\n",
           pprops.deviceName,
           VK_VERSION_MAJOR(pprops.driverVersion),
           VK_VERSION_MINOR(pprops.driverVersion),
           VK_VERSION_PATCH(pprops.driverVersion),
           VK_VERSION_MAJOR(pprops.apiVersion),
           VK_VERSION_MINOR(pprops.apiVersion));

    available_ = true;
    return true;
}

void VulkanIsp::shutdown() {
    if (!d_) return;
    if (d_->device) vkDeviceWaitIdle(d_->device);

    d_->destroy_buffer(d_->raw_buf);
    d_->destroy_buffer(d_->bayer_buf);
    d_->destroy_buffer(d_->rgba_buf);

    if (d_->descriptor_pool) { vkDestroyDescriptorPool(d_->device, d_->descriptor_pool, nullptr); d_->descriptor_pool = VK_NULL_HANDLE; }
    if (d_->pp_unpack)       { vkDestroyPipeline(d_->device, d_->pp_unpack, nullptr);             d_->pp_unpack = VK_NULL_HANDLE; }
    if (d_->pp_demosaic)     { vkDestroyPipeline(d_->device, d_->pp_demosaic, nullptr);           d_->pp_demosaic = VK_NULL_HANDLE; }
    if (d_->pl_unpack)       { vkDestroyPipelineLayout(d_->device, d_->pl_unpack, nullptr);       d_->pl_unpack = VK_NULL_HANDLE; }
    if (d_->pl_demosaic)     { vkDestroyPipelineLayout(d_->device, d_->pl_demosaic, nullptr);     d_->pl_demosaic = VK_NULL_HANDLE; }
    if (d_->dsl)             { vkDestroyDescriptorSetLayout(d_->device, d_->dsl, nullptr);        d_->dsl = VK_NULL_HANDLE; }
    if (d_->sm_unpack)       { vkDestroyShaderModule(d_->device, d_->sm_unpack, nullptr);         d_->sm_unpack = VK_NULL_HANDLE; }
    if (d_->sm_demosaic)     { vkDestroyShaderModule(d_->device, d_->sm_demosaic, nullptr);       d_->sm_demosaic = VK_NULL_HANDLE; }
    if (d_->fence)           { vkDestroyFence(d_->device, d_->fence, nullptr);                    d_->fence = VK_NULL_HANDLE; }
    if (d_->cmd_pool)        { vkDestroyCommandPool(d_->device, d_->cmd_pool, nullptr);           d_->cmd_pool = VK_NULL_HANDLE; }
    if (d_->device)          { vkDestroyDevice(d_->device, nullptr);                              d_->device = VK_NULL_HANDLE; }
    if (d_->instance)        { vkDestroyInstance(d_->instance, nullptr);                          d_->instance = VK_NULL_HANDLE; }

    d_->phys = VK_NULL_HANDLE;
    available_ = false;
    configured_ = false;
}

bool VulkanIsp::configure(int raw_width, int raw_height, int stride,
                          int downsample, BayerPattern pattern) {
    if (!available_) return false;

    raw_w_      = raw_width;
    raw_h_      = raw_height;
    stride_     = stride;
    downsample_ = downsample;
    pattern_    = pattern;
    out_w_      = raw_width  / downsample;
    out_h_      = raw_height / downsample;

    vkDeviceWaitIdle(d_->device);

    // (Re)allocate buffers.
    d_->destroy_buffer(d_->raw_buf);
    d_->destroy_buffer(d_->bayer_buf);
    d_->destroy_buffer(d_->rgba_buf);

    VkDeviceSize raw_sz   = (VkDeviceSize)stride * raw_height;
    VkDeviceSize bayer_sz = (VkDeviceSize)out_w_ * out_h_ * sizeof(uint32_t);
    VkDeviceSize rgba_sz  = (VkDeviceSize)out_w_ * out_h_ * sizeof(uint32_t);

    // Adreno is UMA: prefer DEVICE_LOCAL+HOST_VISIBLE+HOST_COHERENT(+CACHED).
    // Fall back to plain HOST_VISIBLE+HOST_COHERENT if the device is non-UMA.
    const VkMemoryPropertyFlags host_req  = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                          | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags host_pref = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                                          | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const VkMemoryPropertyFlags dev_req   = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    if (!d_->create_buffer(raw_sz,   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           host_req, host_pref, d_->raw_buf))   return false;
    if (!d_->create_buffer(bayer_sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           dev_req,  0,         d_->bayer_buf)) return false;
    if (!d_->create_buffer(rgba_sz,  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           host_req, host_pref, d_->rgba_buf))  return false;

    // Update descriptor set with new buffer handles.
    VkDescriptorBufferInfo bi[3]{};
    bi[0].buffer = d_->raw_buf.buf;   bi[0].range = raw_sz;
    bi[1].buffer = d_->bayer_buf.buf; bi[1].range = bayer_sz;
    bi[2].buffer = d_->rgba_buf.buf;  bi[2].range = rgba_sz;

    std::array<VkWriteDescriptorSet, 3> ws{};
    for (uint32_t i = 0; i < 3; i++) {
        ws[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws[i].dstSet          = d_->descriptor_set;
        ws[i].dstBinding      = i;
        ws[i].descriptorCount = 1;
        ws[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ws[i].pBufferInfo     = &bi[i];
    }
    vkUpdateDescriptorSets(d_->device, static_cast<uint32_t>(ws.size()), ws.data(), 0, nullptr);

    configured_ = true;
    printf("VulkanIsp: configured %dx%d ds=%d -> %dx%d\n",
           raw_w_, raw_h_, downsample_, out_w_, out_h_);
    return true;
}

bool VulkanIsp::process(const uint8_t* raw_data, size_t raw_size,
                        float r_gain, float b_gain,
                        uint8_t* rgb_out) {
    if (!available_ || !configured_) return false;

    int bayer_code = 0;
    switch (pattern_) {
        case BayerPattern::RGGB: bayer_code = 0; break;
        case BayerPattern::BGGR: bayer_code = 1; break;
        case BayerPattern::GRBG: bayer_code = 2; break;
        case BayerPattern::GBRG: bayer_code = 3; break;
    }

    // Stage raw bytes via the unified-memory mapped pointer (no staging buffer).
    size_t copy_n = std::min(raw_size, (size_t)d_->raw_buf.size);
    std::memcpy(d_->raw_buf.mapped, raw_data, copy_n);

    int total = out_w_ * out_h_;
    uint32_t groups = (uint32_t)((total + 63) / 64);

    PushUnpack pu{
        raw_w_, raw_h_, stride_, downsample_, out_w_, out_h_,
        r_gain, b_gain, bayer_code
    };
    PushDemosaic pd{ out_w_, out_h_, bayer_code };

    // ---- Record command buffer ----
    VK_CHECK(vkResetCommandBuffer(d_->cmd_buf, 0));

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(d_->cmd_buf, &bi));

    // Pass 1: unpack
    vkCmdBindPipeline(d_->cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, d_->pp_unpack);
    vkCmdBindDescriptorSets(d_->cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
                            d_->pl_unpack, 0, 1, &d_->descriptor_set, 0, nullptr);
    vkCmdPushConstants(d_->cmd_buf, d_->pl_unpack,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pu), &pu);
    vkCmdDispatch(d_->cmd_buf, groups, 1, 1);

    // SSBO write→read barrier between passes.
    VkMemoryBarrier mb{};
    mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(d_->cmd_buf,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);

    // Pass 2: demosaic
    vkCmdBindPipeline(d_->cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, d_->pp_demosaic);
    vkCmdBindDescriptorSets(d_->cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
                            d_->pl_demosaic, 0, 1, &d_->descriptor_set, 0, nullptr);
    vkCmdPushConstants(d_->cmd_buf, d_->pl_demosaic,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pd), &pd);
    vkCmdDispatch(d_->cmd_buf, groups, 1, 1);

    // Make GPU writes visible to host reads after the fence is signalled.
    VkMemoryBarrier mb2{};
    mb2.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb2.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb2.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(d_->cmd_buf,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &mb2, 0, nullptr, 0, nullptr);

    VK_CHECK(vkEndCommandBuffer(d_->cmd_buf));

    // ---- Submit & wait ----
    VK_CHECK(vkResetFences(d_->device, 1, &d_->fence));
    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &d_->cmd_buf;
    VK_CHECK(vkQueueSubmit(d_->queue, 1, &si, d_->fence));
    VK_CHECK(vkWaitForFences(d_->device, 1, &d_->fence, VK_TRUE, UINT64_MAX));

    // ---- Readback (HOST_COHERENT memory: no invalidate needed) ----
    rgba_to_rgb_neon((const uint8_t*)d_->rgba_buf.mapped, rgb_out,
                     (size_t)out_w_ * out_h_);
    return true;
}
