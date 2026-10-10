// Headless Vulkan renderer: no window or swapchain. Each frame is drawn into an
// offscreen MSAA target, resolved, copied into a host-visible buffer and read
// back as an Image. Works on MoltenVK (macOS) via the portability extensions.
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "render/renderer.hpp"
#include "render/vk_loader.hpp"

namespace sf {

namespace {

const uint32_t kVertSpv[] =
#include "render/spirv/mesh.vert.inc"
    ;
const uint32_t kFragSpv[] =
#include "render/spirv/mesh.frag.inc"
    ;

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed (VkResult " + std::to_string(r) + ")");
}

bool hasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

class VulkanRenderer : public Renderer {
public:
    VulkanRenderer() {
        try {
            init();
        } catch (...) {
            destroy();
            throw;
        }
    }
    ~VulkanRenderer() override { destroy(); }

    std::string name() const override {
        return "Vulkan (" + deviceName_ + " via " + source_ + ", " + std::to_string(samples_) + "x MSAA)";
    }

    Image render(const Frame& f) override {
        if (f.width != width_ || f.height != height_) createTargets(f.width, f.height);
        uploadVertices(f);

        check(vkResetCommandBuffer(cmd_, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cmd_, &begin), "vkBeginCommandBuffer");

        VkClearValue clears[3]{};
        clears[0].color = {{f.background.x, f.background.y, f.background.z, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};
        clears[2].color = clears[0].color;
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = renderPass_;
        rp.framebuffer = framebuffer_;
        rp.renderArea.extent = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_)};
        rp.clearValueCount = msaa() ? 3 : 2;
        rp.pClearValues = clears;
        vkCmdBeginRenderPass(cmd_, &rp, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_)}};
        vkCmdSetViewport(cmd_, 0, 1, &vp);
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
        vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(FrameUniforms), &f.uniforms);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd_, 0, 1, &vertexBuffer_, &offset);
        if (!f.triangles.empty()) {
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, trianglePipeline_);
            vkCmdDraw(cmd_, static_cast<uint32_t>(f.triangles.size()), 1, 0, 0);
        }
        if (!f.lines.empty()) {
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, linePipeline_);
            vkCmdDraw(cmd_, static_cast<uint32_t>(f.lines.size()), 1, static_cast<uint32_t>(f.triangles.size()), 0);
        }
        vkCmdEndRenderPass(cmd_);

        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};
        vkCmdCopyImageToBuffer(cmd_, msaa() ? resolve_.image : color_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback_, 1, &copy);
        VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.buffer = readback_;
        toHost.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                             &toHost, 0, nullptr);
        check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd_;
        check(vkResetFences(device_, 1, &fence_), "vkResetFences");
        check(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit");
        check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, 30ull * 1000 * 1000 * 1000), "vkWaitForFences");

        if (!readbackCoherent_) {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = readbackMemory_;
            range.size = VK_WHOLE_SIZE;
            vkInvalidateMappedMemoryRanges(device_, 1, &range);
        }
        Image out(width_, height_);
        const auto* src = static_cast<const uint8_t*>(readbackMapped_);
        for (size_t i = 0, n = static_cast<size_t>(width_) * height_; i < n; ++i) {
            out.pixels[i * 3] = src[i * 4];
            out.pixels[i * 3 + 1] = src[i * 4 + 1];
            out.pixels[i * 3 + 2] = src[i * 4 + 2];
        }
        return out;
    }

private:
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    bool msaa() const { return samples_ > 1; }

    void init() {
        std::string err;
        if (!loadVulkan(err, source_)) throw std::runtime_error(err);
        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> instExts(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, instExts.data());

        std::vector<const char*> enabled;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        if (hasExtension(instExts, "VK_KHR_portability_enumeration")) {
            enabled.push_back("VK_KHR_portability_enumeration");
            ici.flags |= 0x00000001;  // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
        }
        if (hasExtension(instExts, "VK_KHR_get_physical_device_properties2"))
            enabled.push_back("VK_KHR_get_physical_device_properties2");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "shapeforge";
        app.pEngineName = "shapeforge";
        app.apiVersion = VK_API_VERSION_1_1;
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
        ici.ppEnabledExtensionNames = enabled.data();
        check(vkCreateInstance(&ici, nullptr, &instance_), "vkCreateInstance");
        if (!loadVulkanInstanceFunctions(instance_, err)) throw std::runtime_error(err);

        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        if (count == 0) throw std::runtime_error("no Vulkan physical devices");
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        int bestScore = -1;
        for (VkPhysicalDevice d : devices) {
            uint32_t qc = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qc, nullptr);
            std::vector<VkQueueFamilyProperties> qprops(qc);
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qc, qprops.data());
            for (uint32_t q = 0; q < qc; ++q) {
                if (!(qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(d, &props);
                int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                            : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                                         : 1;
                if (score > bestScore) {
                    bestScore = score;
                    physical_ = d;
                    queueFamily_ = q;
                    deviceName_ = props.deviceName;
                }
                break;
            }
        }
        if (bestScore < 0) throw std::runtime_error("no Vulkan device with a graphics queue");

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_, &props);
        VkSampleCountFlags counts =
            props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
        samples_ = (counts & VK_SAMPLE_COUNT_4_BIT) ? 4 : 1;
        vkGetPhysicalDeviceMemoryProperties(physical_, &memProps_);

        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> devExts(count);
        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, devExts.data());
        std::vector<const char*> devEnabled;
        if (hasExtension(devExts, "VK_KHR_portability_subset")) devEnabled.push_back("VK_KHR_portability_subset");

        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = static_cast<uint32_t>(devEnabled.size());
        dci.ppEnabledExtensionNames = devEnabled.data();
        check(vkCreateDevice(physical_, &dci, nullptr, &device_), "vkCreateDevice");
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queueFamily_;
        check(vkCreateCommandPool(device_, &pci, nullptr, &pool_), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device_, &cai, &cmd_), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(device_, &fci, nullptr, &fence_), "vkCreateFence");

        createRenderPass();
        createPipelines();
    }

    uint32_t findMemory(uint32_t typeBits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
            if ((typeBits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & want) == want) return i;
        throw std::runtime_error("no suitable Vulkan memory type");
    }

    void createRenderPass() {
        VkSampleCountFlagBits s = static_cast<VkSampleCountFlagBits>(samples_);
        VkAttachmentDescription att[3]{};
        att[0].format = kColorFormat;
        att[0].samples = s;
        att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[0].storeOp = msaa() ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[0].finalLayout = msaa() ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        att[1].format = kDepthFormat;
        att[1].samples = s;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[2].format = kColorFormat;
        att[2].samples = VK_SAMPLE_COUNT_1_BIT;
        att[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[2].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkAttachmentReference resolveRef{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &colorRef;
        sub.pDepthStencilAttachment = &depthRef;
        if (msaa()) sub.pResolveAttachments = &resolveRef;

        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[0].dstStageMask =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = msaa() ? 3 : 2;
        rpci.pAttachments = att;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2;
        rpci.pDependencies = deps;
        check(vkCreateRenderPass(device_, &rpci, nullptr, &renderPass_), "vkCreateRenderPass");
    }

    VkShaderModule makeShader(const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes;
        ci.pCode = code;
        VkShaderModule m;
        check(vkCreateShaderModule(device_, &ci, nullptr, &m), "vkCreateShaderModule");
        return m;
    }

    void createPipelines() {
        VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FrameUniforms)};
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges = &pcr;
        check(vkCreatePipelineLayout(device_, &lci, nullptr, &layout_), "vkCreatePipelineLayout");

        VkShaderModule vs = makeShader(kVertSpv, sizeof(kVertSpv));
        VkShaderModule fs = makeShader(kFragSpv, sizeof(kFragSpv));
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[3] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
            {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)},
        };
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &binding;
        vin.vertexAttributeDescriptionCount = 3;
        vin.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1;
        vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;  // LLM-authored meshes may have inconsistent winding
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(samples_);
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState blendAtt{};
        blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                  VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &blendAtt;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dsci{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dsci.dynamicStateCount = 2;
        dsci.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2;
        gp.pStages = stages;
        gp.pVertexInputState = &vin;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &ds;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsci;
        gp.layout = layout_;
        gp.renderPass = renderPass_;

        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkResult r1 = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &trianglePipeline_);
        ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        VkResult r2 = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &linePipeline_);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
        check(r1, "vkCreateGraphicsPipelines (triangles)");
        check(r2, "vkCreateGraphicsPipelines (lines)");
    }

    Target createImage(VkFormat format, VkImageUsageFlags usage, int samples, VkImageAspectFlags aspect) {
        Target t;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format;
        ici.extent = {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_), 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = static_cast<VkSampleCountFlagBits>(samples);
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(device_, &ici, nullptr, &t.image), "vkCreateImage");
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, t.image, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device_, &mai, nullptr, &t.memory), "vkAllocateMemory (image)");
        vkBindImageMemory(device_, t.image, t.memory, 0);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = t.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.subresourceRange = {aspect, 0, 1, 0, 1};
        check(vkCreateImageView(device_, &vci, nullptr, &t.view), "vkCreateImageView");
        return t;
    }

    void destroyTarget(Target& t) {
        if (t.view) vkDestroyImageView(device_, t.view, nullptr);
        if (t.image) vkDestroyImage(device_, t.image, nullptr);
        if (t.memory) vkFreeMemory(device_, t.memory, nullptr);
        t = Target{};
    }

    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, VkDeviceMemory& mem, void*& mapped,
                      bool* coherent = nullptr) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = usage;
        check(vkCreateBuffer(device_, &bci, nullptr, &buf), "vkCreateBuffer");
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, buf, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        bool isCoherent = true;
        try {
            mai.memoryTypeIndex = findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        } catch (const std::exception&) {
            mai.memoryTypeIndex = findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
            isCoherent = false;
        }
        if (coherent) *coherent = isCoherent;
        check(vkAllocateMemory(device_, &mai, nullptr, &mem), "vkAllocateMemory (buffer)");
        vkBindBufferMemory(device_, buf, mem, 0);
        check(vkMapMemory(device_, mem, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
    }

    void destroyBuffer(VkBuffer& buf, VkDeviceMemory& mem) {
        if (mem) vkUnmapMemory(device_, mem);
        if (buf) vkDestroyBuffer(device_, buf, nullptr);
        if (mem) vkFreeMemory(device_, mem, nullptr);
        buf = VK_NULL_HANDLE;
        mem = VK_NULL_HANDLE;
    }

    void destroyTargets() {
        if (framebuffer_) vkDestroyFramebuffer(device_, framebuffer_, nullptr);
        framebuffer_ = VK_NULL_HANDLE;
        destroyTarget(color_);
        destroyTarget(depth_);
        destroyTarget(resolve_);
        destroyBuffer(readback_, readbackMemory_);
    }

    void createTargets(int w, int h) {
        vkDeviceWaitIdle(device_);
        destroyTargets();
        width_ = w;
        height_ = h;
        VkImageUsageFlags colorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (!msaa()) colorUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        else colorUsage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        color_ = createImage(kColorFormat, colorUsage, samples_, VK_IMAGE_ASPECT_COLOR_BIT);
        depth_ = createImage(kDepthFormat,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                 (msaa() ? VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT : 0),
                             samples_, VK_IMAGE_ASPECT_DEPTH_BIT);
        std::vector<VkImageView> views{color_.view, depth_.view};
        if (msaa()) {
            resolve_ = createImage(kColorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                   1, VK_IMAGE_ASPECT_COLOR_BIT);
            views.push_back(resolve_.view);
        }
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = renderPass_;
        fci.attachmentCount = static_cast<uint32_t>(views.size());
        fci.pAttachments = views.data();
        fci.width = static_cast<uint32_t>(w);
        fci.height = static_cast<uint32_t>(h);
        fci.layers = 1;
        check(vkCreateFramebuffer(device_, &fci, nullptr, &framebuffer_), "vkCreateFramebuffer");
        createBuffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback_,
                     readbackMemory_, readbackMapped_, &readbackCoherent_);
    }

    void uploadVertices(const Frame& f) {
        size_t count = f.triangles.size() + f.lines.size();
        VkDeviceSize bytes = std::max<VkDeviceSize>(sizeof(Vertex) * count, sizeof(Vertex) * 3);
        if (bytes > vertexCapacity_) {
            vkDeviceWaitIdle(device_);
            destroyBuffer(vertexBuffer_, vertexMemory_);
            VkDeviceSize cap = std::max<VkDeviceSize>(bytes + bytes / 2, 1 << 20);
            createBuffer(cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertexBuffer_, vertexMemory_, vertexMapped_,
                         &vertexCoherent_);
            vertexCapacity_ = cap;
        }
        auto* dst = static_cast<uint8_t*>(vertexMapped_);
        if (!f.triangles.empty()) std::memcpy(dst, f.triangles.data(), sizeof(Vertex) * f.triangles.size());
        if (!f.lines.empty())
            std::memcpy(dst + sizeof(Vertex) * f.triangles.size(), f.lines.data(), sizeof(Vertex) * f.lines.size());
        if (!vertexCoherent_) {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = vertexMemory_;
            range.size = VK_WHOLE_SIZE;
            vkFlushMappedMemoryRanges(device_, 1, &range);
        }
    }

    void destroy() {
        if (device_) {
            vkDeviceWaitIdle(device_);
            destroyTargets();
            destroyBuffer(vertexBuffer_, vertexMemory_);
            if (trianglePipeline_) vkDestroyPipeline(device_, trianglePipeline_, nullptr);
            if (linePipeline_) vkDestroyPipeline(device_, linePipeline_, nullptr);
            if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
            if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
            if (fence_) vkDestroyFence(device_, fence_, nullptr);
            if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (instance_ && vkDestroyInstance) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
    }

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    std::string deviceName_;
    std::string source_;
    int samples_ = 1;

    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline trianglePipeline_ = VK_NULL_HANDLE;
    VkPipeline linePipeline_ = VK_NULL_HANDLE;

    int width_ = 0, height_ = 0;
    Target color_, depth_, resolve_;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkBuffer readback_ = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory_ = VK_NULL_HANDLE;
    void* readbackMapped_ = nullptr;
    bool readbackCoherent_ = true;

    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory_ = VK_NULL_HANDLE;
    void* vertexMapped_ = nullptr;
    VkDeviceSize vertexCapacity_ = 0;
    bool vertexCoherent_ = true;
};

}  // namespace

std::unique_ptr<Renderer> createVulkanRenderer() { return std::make_unique<VulkanRenderer>(); }

}  // namespace sf
