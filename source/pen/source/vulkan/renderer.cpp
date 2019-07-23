#include "renderer.h"
#include "console.h"
#include "data_struct.h"

#include "vulkan/vulkan.h"

#ifdef _WIN32
#include "vulkan/vulkan_win32.h"
#endif

#define CHECK_CALL(C) { VkResult r = (C); PEN_ASSERT(r == VK_SUCCESS); }

extern pen::window_creation_params pen_window;
a_u8                               g_window_resize(0);

using namespace pen;

namespace
{
    struct vulkan_context
    {
        VkInstance                  instance;
        const char**                layer_names = nullptr;
        const char**                ext_names = nullptr;
        VkDebugUtilsMessengerEXT    debug_messanger;
        bool                        enable_validation = false;
        VkPhysicalDevice            physical_device = VK_NULL_HANDLE;
        u32                         num_queue_families;
        u32                         graphics_family_index;
        VkQueue                     graphics_queue;
        u32                         present_family_index;
        VkQueue                     present_queue;
        VkDevice                    device;
        VkSurfaceKHR                surface;
        VkSwapchainKHR              swap_chain;
        VkImage*                    swap_chain_images = nullptr;
        VkCommandPool               cmd_pool;
        VkCommandBuffer*            cmd_bufs = nullptr;
        u32                         img_index = 0;
    };
    vulkan_context _ctx;

    struct pen_state
    {
        u32  shader[3]; // vs, fs, cs
        u32* colour_attachments = nullptr;
        u32  depth_attachment = 0;
        u32  colour_slice;
        u32  depth_slice;
        u32  clear_state;
    };
    pen_state _state;

    struct vulkan_texture
    {
        VkImageView image_view;
        VkImage     image;
        VkFormat    format;
    };

    enum class e_shd
    {
        vertex,
        fragment,
        compute
    };

    struct vulkan_shader
    {
        VkShaderModule module;
        e_shd          type;
    };

    enum class e_res
    {
        none,
        clear,
        texture,
        render_target,
        shader,
    };

    struct resource_allocation
    {
        e_res type = e_res::none;

        union {
            vulkan_texture texture;
            vulkan_shader  shader;
            clear_state    clear;
        };
    };
    res_pool<resource_allocation> _res_pool;

    void enumerate_layers()
    {
        u32 num_layer_props;
        vkEnumerateInstanceLayerProperties(&num_layer_props, nullptr);

        VkLayerProperties* layer_props = new VkLayerProperties[num_layer_props];
        vkEnumerateInstanceLayerProperties(&num_layer_props, layer_props);

        for (u32 i = 0; i < num_layer_props; ++i)
            sb_push(_ctx.layer_names, layer_props[i].layerName);
    }

    void enumerate_extensions()
    {
        u32 num_ext_props;
        vkEnumerateInstanceExtensionProperties(nullptr, &num_ext_props, nullptr);

        VkExtensionProperties* ext_props = new VkExtensionProperties[num_ext_props];
        vkEnumerateInstanceExtensionProperties(nullptr, &num_ext_props, ext_props);

        for (u32 i = 0; i < num_ext_props; ++i)
            sb_push(_ctx.ext_names, ext_props[i].extensionName);
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT messageType,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData) {

        PEN_LOG("[vulkan validation] %s\n", pCallbackData->pMessage);
        return VK_FALSE;
    }

    VkResult CreateDebugUtilsMessengerEXT(
        VkInstance instance, 
        const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, 
        const VkAllocationCallbacks* pAllocator,
        VkDebugUtilsMessengerEXT* pDebugMessenger) 
    {
        auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        if (func != nullptr) 
            return func(instance, pCreateInfo, pAllocator, pDebugMessenger);

        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    void DestroyDebugUtilsMessengerEXT(
        VkInstance instance, 
        VkDebugUtilsMessengerEXT debugMessenger, 
        const VkAllocationCallbacks* pAllocator)
    {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (func != nullptr) 
            func(instance, debugMessenger, pAllocator);
    }

    void create_debug_messenger()
    {
        VkDebugUtilsMessengerCreateInfoEXT info = {};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = debug_callback;
        info.pUserData = nullptr;

        VkResult res = CreateDebugUtilsMessengerEXT(_ctx.instance, &info, nullptr, &_ctx.debug_messanger);
        PEN_ASSERT(res == VK_SUCCESS);
    }

    void destroy_debug_messenger()
    {
        DestroyDebugUtilsMessengerEXT(_ctx.instance, _ctx.debug_messanger, nullptr);
    }

    void create_surface(void* params)
    {
        // in win32 params is HWND
        HWND hwnd = *((HWND*)params);

        VkWin32SurfaceCreateInfoKHR info = {};
        info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        info.hwnd = hwnd;
        info.hinstance = GetModuleHandle(nullptr);

        CHECK_CALL(vkCreateWin32SurfaceKHR(_ctx.instance, &info, nullptr, &_ctx.surface));
    }

    void create_backbuffer_targets()
    {
        // get swapchain images
        u32 num_images = 0;
        vkGetSwapchainImagesKHR(_ctx.device, _ctx.swap_chain, &num_images, nullptr);
        VkImage* images = new VkImage[num_images];
        vkGetSwapchainImagesKHR(_ctx.device, _ctx.swap_chain, &num_images, images);
        for (u32 i = 0; i < num_images; ++i)
            sb_push(_ctx.swap_chain_images, images[i]);

        for (u32 i = 0; i < sb_count(_ctx.swap_chain_images); ++i)
        {
            _res_pool.insert({}, i);
            vulkan_texture& vt = _res_pool.get(i).texture;

            VkImageViewCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            info.image = _ctx.swap_chain_images[i];
            info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            info.format = VK_FORMAT_B8G8R8A8_UNORM;
            info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            info.subresourceRange.baseMipLevel = 0;
            info.subresourceRange.levelCount = 1;
            info.subresourceRange.baseArrayLayer = 0;
            info.subresourceRange.layerCount = 1;

            VkImageView iv;
            CHECK_CALL(vkCreateImageView(_ctx.device, &info, nullptr, &iv));

            vt.image_view = iv;
            vt.image = _ctx.swap_chain_images[i];
            vt.format = VK_FORMAT_B8G8R8A8_UNORM;
        }

        delete images;
    }

    void create_command_buffers()
    {
        VkCommandPoolCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.queueFamilyIndex = 0;

        CHECK_CALL(vkCreateCommandPool(_ctx.device, &info, NULL, &_ctx.cmd_pool));

        VkCommandBufferAllocateInfo buf_info = {};
        buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        buf_info.commandPool = _ctx.cmd_pool;
        buf_info.commandBufferCount = sb_count(_ctx.swap_chain_images);
        buf_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;

        // make enough size
        for (u32 i = 0; i < buf_info.commandBufferCount; ++i)
            sb_push(_ctx.cmd_bufs, VkCommandBuffer());

        CHECK_CALL(vkAllocateCommandBuffers(_ctx.device, &buf_info, _ctx.cmd_bufs));
    }

    void create_device_surface_swapchain(void* params)
    {
        u32 dev_count;
        CHECK_CALL(vkEnumeratePhysicalDevices(_ctx.instance, &dev_count, nullptr));
        PEN_ASSERT(dev_count); // no supported devices

        VkPhysicalDevice* devices = new VkPhysicalDevice[dev_count];
        CHECK_CALL(vkEnumeratePhysicalDevices(_ctx.instance, &dev_count, devices));

        _ctx.physical_device = devices[0];

        // find gfx queue
        _ctx.num_queue_families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(_ctx.physical_device, &_ctx.num_queue_families, nullptr);

        VkQueueFamilyProperties* queue_families = new VkQueueFamilyProperties[_ctx.num_queue_families];
        vkGetPhysicalDeviceQueueFamilyProperties(_ctx.physical_device, &_ctx.num_queue_families, queue_families);

        // surface
        create_surface(params);
        
        _ctx.graphics_family_index = -1;
        _ctx.present_family_index = -1;
        for (u32 i = 0; i < _ctx.num_queue_families; ++i)
        {
            if (queue_families[i].queueCount > 0 && queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                _ctx.graphics_family_index = i;
            }

            VkBool32 present = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(_ctx.physical_device, i, _ctx.surface, &present);
            if (queue_families[i].queueCount > 0 && present)
            {
                _ctx.present_family_index = i;
            }
        }
        PEN_ASSERT(_ctx.graphics_family_index != -1);
        PEN_ASSERT(_ctx.present_family_index != -1);

        // gfx queue
        f32 pri = 1.0f;
        VkDeviceQueueCreateInfo gfx_queue_info = {};
        gfx_queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        gfx_queue_info.queueFamilyIndex = _ctx.graphics_family_index;
        gfx_queue_info.queueCount = 1;
        gfx_queue_info.pQueuePriorities = &pri;

        VkDeviceQueueCreateInfo present_queue_info = {};
        present_queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        present_queue_info.queueFamilyIndex = _ctx.present_family_index;
        present_queue_info.queueCount = 1;
        present_queue_info.pQueuePriorities = &pri;

        VkDeviceQueueCreateInfo* queues = nullptr;
        sb_push(queues, gfx_queue_info);
        sb_push(queues, present_queue_info);

        // device
        VkPhysicalDeviceFeatures features = {};
        VkDeviceCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.pQueueCreateInfos = queues;
        info.queueCreateInfoCount = sb_count(queues);
        info.pEnabledFeatures = &features;

        // validation
        if (_ctx.enable_validation)
        {
            static const char** debug_layer;
            sb_push(debug_layer, "VK_LAYER_KHRONOS_validation");
            info.enabledLayerCount = 1;
            info.ppEnabledLayerNames = debug_layer;
        }

        // enable extensions
        {
            static const char** exts;
            sb_push(exts, "VK_KHR_swapchain");

            info.enabledExtensionCount = sb_count(exts);
            info.ppEnabledExtensionNames = exts;
        }

        CHECK_CALL(vkCreateDevice(_ctx.physical_device, &info, nullptr, &_ctx.device));

        vkGetDeviceQueue(_ctx.device, _ctx.graphics_family_index, 0, &_ctx.graphics_queue);
        vkGetDeviceQueue(_ctx.device, _ctx.present_family_index, 0, &_ctx.present_queue);

        // swap chain

        // store these?
        {
            u32 num_formats;
            vkGetPhysicalDeviceSurfaceFormatsKHR(_ctx.physical_device, _ctx.surface, &num_formats, nullptr);
            VkSurfaceFormatKHR* formats = new VkSurfaceFormatKHR[num_formats];
            vkGetPhysicalDeviceSurfaceFormatsKHR(_ctx.physical_device, _ctx.surface, &num_formats, formats);
            delete formats;

            u32 num_present_modes;
            vkGetPhysicalDeviceSurfacePresentModesKHR(_ctx.physical_device, _ctx.surface, &num_present_modes, nullptr);
            VkPresentModeKHR* present_modes = new VkPresentModeKHR[num_present_modes];
            vkGetPhysicalDeviceSurfacePresentModesKHR(_ctx.physical_device, _ctx.surface, &num_formats, present_modes);
            delete present_modes;
        }


        VkSurfaceCapabilitiesKHR caps;
        CHECK_CALL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_ctx.physical_device, _ctx.surface, &caps));

        VkSwapchainCreateInfoKHR swap_chain_info = {};
        swap_chain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        swap_chain_info.surface = _ctx.surface;
        swap_chain_info.minImageCount = 3;
        swap_chain_info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        swap_chain_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        swap_chain_info.imageExtent = { pen_window.width, pen_window.height };
        swap_chain_info.imageArrayLayers = 1;
        swap_chain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        swap_chain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swap_chain_info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        swap_chain_info.oldSwapchain = VK_NULL_HANDLE;
        swap_chain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        swap_chain_info.clipped = VK_TRUE;
        swap_chain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;

        CHECK_CALL(vkCreateSwapchainKHR(_ctx.device, &swap_chain_info, nullptr, &_ctx.swap_chain));

        create_backbuffer_targets();

        create_command_buffers();

        delete queue_families;
        delete devices;
    }

    void bind()
    {
        const clear_state& clear = _res_pool.get(_state.clear_state).clear;

        // attachments
        VkAttachmentDescription* colour_attachments = nullptr;
        VkAttachmentReference*   colour_refs = nullptr;
        VkImageView*             colour_img_view = nullptr;

        for (u32 i = 0; i < sb_count(_state.colour_attachments); ++i)
        {
            u32 ica = _state.colour_attachments[i];
            const vulkan_texture& vt = _res_pool.get(ica).texture;

            VkAttachmentDescription col = {};
            col.format = vt.format;
            col.samples = VK_SAMPLE_COUNT_1_BIT;

            if (clear.flags & PEN_CLEAR_COLOUR_BUFFER)
                col.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            else
                col.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;

            col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            col.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            col.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            VkAttachmentReference ref = {};
            ref.attachment = i;
            ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            sb_push(colour_attachments, col);
            sb_push(colour_refs, ref);
            sb_push(colour_img_view, vt.image_view);
        }

        // sub pass
        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = sb_count(colour_refs);
        subpass.pColorAttachments = colour_refs;

        // pass
        VkRenderPassCreateInfo pass_info = {};
        pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        pass_info.attachmentCount = sb_count(colour_attachments);
        pass_info.pAttachments = colour_attachments;
        pass_info.subpassCount = 1;
        pass_info.pSubpasses = &subpass;

        VkRenderPass pass;
        CHECK_CALL(vkCreateRenderPass(_ctx.device, &pass_info, nullptr, &pass));

        // framebuffer
        VkFramebufferCreateInfo fb_info = {};
        fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass = pass;
        fb_info.attachmentCount = sb_count(colour_img_view);
        fb_info.pAttachments = colour_img_view;
        fb_info.width = pen_window.width;
        fb_info.height = pen_window.height;
        fb_info.layers = 1;

        VkFramebuffer fb;
        CHECK_CALL(vkCreateFramebuffer(_ctx.device, &fb_info, nullptr, &fb));

        sb_free(colour_attachments);
        sb_free(colour_refs);
        sb_free(colour_img_view);

        // command buffers
        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

        VkClearColorValue clear_colour = { clear.r, clear.g, clear.b };
        VkClearValue clear_value = {};
        clear_value.color = clear_colour;

        VkImageSubresourceRange img_range = {};
        img_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        img_range.levelCount = 1;
        img_range.layerCount = 1;

        CHECK_CALL(vkBeginCommandBuffer(_ctx.cmd_bufs[_ctx.img_index], &begin_info));

        vkCmdClearColorImage(_ctx.cmd_bufs[_ctx.img_index], _ctx.swap_chain_images[_ctx.img_index], 
            VK_IMAGE_LAYOUT_GENERAL, &clear_colour, 1, &img_range);
    }
}

namespace pen
{
    a_u64 g_gpu_total;

    static renderer_info s_renderer_info;
    const renderer_info& renderer_get_info()
    {
        return s_renderer_info;
    }

    const c8* renderer_get_shader_platform()
    {
        return "spirv";
    }

    bool renderer_viewport_vup()
    {
        return false;
    }

    namespace direct
    {
        u32 renderer_initialise(void* params, u32 bb_res, u32 bb_depth_res)
        {
            _res_pool.init(4096);

#if _DEBUG
            _ctx.enable_validation = true;
#endif
            enumerate_layers();
            enumerate_extensions();

            VkApplicationInfo app_info = {};
            app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            app_info.pApplicationName = pen_window.window_title;
            app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
            app_info.pEngineName = "pmtech";
            app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
            app_info.apiVersion = VK_API_VERSION_1_0;

            VkInstanceCreateInfo create_info = {};
            create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            create_info.pApplicationInfo = &app_info;
            create_info.enabledExtensionCount = sb_count(_ctx.ext_names);
            create_info.ppEnabledExtensionNames = _ctx.ext_names;

            if (_ctx.enable_validation)
            {
                static const char** debug_layer;
                sb_push(debug_layer, "VK_LAYER_KHRONOS_validation");
                create_info.enabledLayerCount = 1;
                create_info.ppEnabledLayerNames = debug_layer;
            }

            CHECK_CALL(vkCreateInstance(&create_info, nullptr, &_ctx.instance));

            if(_ctx.enable_validation)
                create_debug_messenger();

            create_device_surface_swapchain(params);

            return 0;
        }

        void renderer_shutdown()
        {
            if (_ctx.enable_validation)
                destroy_debug_messenger();

            for (u32 i = 0; i < sb_count(_ctx.swap_chain_images); ++i)
                vkDestroyImage(_ctx.device, _ctx.swap_chain_images[i], nullptr);

            vkDestroySurfaceKHR(_ctx.instance, _ctx.surface, nullptr);
            vkDestroySwapchainKHR(_ctx.device, _ctx.swap_chain, nullptr);
            vkDestroyInstance(_ctx.instance, nullptr);
        }

        void renderer_make_context_current()
        {

        }

        void renderer_sync()
        {

        }

        void renderer_create_clear_state(const clear_state& cs, u32 resource_slot)
        {

        }

        void renderer_clear(u32 clear_state_index, u32 colour_face, u32 depth_face)
        {
            _state.clear_state = clear_state_index;
        }

        void renderer_load_shader(const pen::shader_load_params& params, u32 resource_slot)
        {
            _res_pool.insert({}, resource_slot);

            VkShaderModuleCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            info.codeSize = params.byte_code_size;
            info.pCode = (const u32*)params.byte_code;

            vulkan_shader& vs = _res_pool.get(resource_slot).shader;
            vs.type = (e_shd)params.type;
            CHECK_CALL(vkCreateShaderModule(_ctx.device, &info, nullptr, &vs.module));
        }

        void renderer_set_shader(u32 shader_index, u32 shader_type)
        {
            _state.shader[shader_type] = shader_index;
        }

        void renderer_create_input_layout(const input_layout_creation_params& params, u32 resource_slot)
        {

        }

        void renderer_set_input_layout(u32 layout_index)
        {

        }

        void renderer_link_shader_program(const shader_link_params& params, u32 resource_slot)
        {

        }

        void renderer_create_buffer(const buffer_creation_params& params, u32 resource_slot)
        {

        }

        void renderer_set_vertex_buffers(u32* buffer_indices, u32 num_buffers, u32 start_slot, const u32* strides, const u32* offsets)
        {

        }

        void renderer_set_index_buffer(u32 buffer_index, u32 format, u32 offset)
        {

        }

        void renderer_set_constant_buffer(u32 buffer_index, u32 resource_slot, u32 flags)
        {

        }

        void renderer_update_buffer(u32 buffer_index, const void* data, u32 data_size, u32 offset)
        {

        }

        void renderer_create_texture(const texture_creation_params& tcp, u32 resource_slot)
        {

        }

        void renderer_create_sampler(const sampler_creation_params& scp, u32 resource_slot)
        {

        }

        void renderer_set_texture(u32 texture_index, u32 sampler_index, u32 resource_slot, u32 bind_flags)
        {

        }

        void renderer_create_rasterizer_state(const rasteriser_state_creation_params& rscp, u32 resource_slot)
        {

        }

        void renderer_set_rasterizer_state(u32 rasterizer_state_index)
        {

        }

        void renderer_set_viewport(const viewport& vp)
        {

        }

        void renderer_set_scissor_rect(const rect& r)
        {

        }

        void renderer_create_blend_state(const blend_creation_params& bcp, u32 resource_slot)
        {

        }

        void renderer_set_blend_state(u32 blend_state_index)
        {

        }

        void renderer_create_depth_stencil_state(const depth_stencil_creation_params& dscp, u32 resource_slot)
        {

        }

        void renderer_set_depth_stencil_state(u32 depth_stencil_state)
        {

        }

        void renderer_set_stencil_ref(u8 ref)
        {

        }

        void renderer_draw(u32 vertex_count, u32 start_vertex, u32 primitive_topology)
        {
            bind();

            // now we make draws
        }

        void renderer_draw_indexed(u32 index_count, u32 start_index, u32 base_vertex, u32 primitive_topology)
        {

        }

        void renderer_draw_indexed_instanced(u32 instance_count, u32 start_instance, u32 index_count, u32 start_index, u32 base_vertex, u32 primitive_topology)
        {

        }

        void renderer_draw_auto()
        {

        }

        void renderer_dispatch_compute(uint3 grid, uint3 num_threads)
        {

        }

        void renderer_create_render_target(const texture_creation_params& tcp, u32 resource_slot, bool track)
        {

        }

        void renderer_set_targets(const u32* const colour_targets, u32 num_colour_targets, u32 depth_target, u32 colour_face, u32 depth_face)
        {
            sb_clear(_state.colour_attachments);
            _state.colour_attachments = nullptr;

            for (u32 i = 0; i < num_colour_targets; ++i)
                sb_push(_state.colour_attachments, colour_targets[i]);

            _state.depth_attachment = depth_target;
            _state.depth_slice = depth_face;
            _state.colour_slice = colour_face;
        }

        void renderer_set_resolve_targets(u32 colour_target, u32 depth_target)
        {

        }

        void renderer_set_stream_out_target(u32 buffer_index)
        {

        }

        void renderer_resolve_target(u32 target, e_msaa_resolve_type type, resolve_resources res)
        {

        }

        void renderer_read_back_resource(const resource_read_back_params& rrbp)
        {

        }

        void renderer_present()
        {
            vkEndCommandBuffer(_ctx.cmd_bufs[_ctx.img_index]);

            CHECK_CALL(vkAcquireNextImageKHR(_ctx.device, _ctx.swap_chain, UINT64_MAX, nullptr, nullptr, &_ctx.img_index));

            VkSubmitInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            info.commandBufferCount = 1;
            info.pCommandBuffers = &_ctx.cmd_bufs[_ctx.img_index];

            CHECK_CALL(vkQueueSubmit(_ctx.graphics_queue, 1, &info, nullptr));

            VkPresentInfoKHR present = {};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.swapchainCount = 1;
            present.pSwapchains = &_ctx.swap_chain;
            present.pImageIndices = &_ctx.img_index;

            CHECK_CALL(vkQueuePresentKHR(_ctx.graphics_queue, &present));
        }

        void renderer_push_perf_marker(const c8* name)
        {

        }

        void renderer_pop_perf_marker()
        {

        }

        void renderer_replace_resource(u32 dest, u32 src, e_renderer_resource type)
        {

        }

        void renderer_release_shader(u32 shader_index, u32 shader_type)
        {
            vulkan_shader& vs = _res_pool.get(shader_index).shader;
            vkDestroyShaderModule(_ctx.device, vs.module, nullptr);
        }

        void renderer_release_clear_state(u32 clear_state)
        {

        }

        void renderer_release_buffer(u32 buffer_index)
        {

        }

        void renderer_release_texture(u32 texture_index)
        {

        }

        void renderer_release_sampler(u32 sampler)
        {

        }

        void renderer_release_raster_state(u32 raster_state_index)
        {

        }

        void renderer_release_blend_state(u32 blend_state)
        {

        }

        void renderer_release_render_target(u32 render_target)
        {

        }

        void renderer_release_input_layout(u32 input_layout)
        {

        }

        void renderer_release_depth_stencil_state(u32 depth_stencil_state)
        {

        }
    }
}