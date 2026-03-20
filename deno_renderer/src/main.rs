// deno_renderer: Headless Vulkan renderer with WebGPU-like ops for TypeScript.
// Uses ash for Vulkan, naga for WGSL→SPIR-V compilation.
// Creates an exportable VkImage and renders directly to it — zero copy.

use ash::vk;
use deno_core::{op2, JsRuntime, OpState, RuntimeOptions};
use std::collections::HashMap;
use std::ffi::CStr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

#[cfg(unix)]
type NativeHandle = i32; // fd
#[cfg(windows)]
type NativeHandle = *mut std::ffi::c_void; // HANDLE
#[cfg(unix)]
const INVALID_HANDLE: NativeHandle = -1;
#[cfg(windows)]
const INVALID_HANDLE: NativeHandle = std::ptr::null_mut();

// ---------------------------------------------------------------------------
// SharedSurfaceInfo — must match the C++ layout exactly (C ABI)
// ---------------------------------------------------------------------------
#[repr(C)]
#[derive(Clone, Copy, Debug)]
struct SharedSurfaceInfo {
    width: u32,
    height: u32,
    format: u32,          // VkFormat value
    memory_size: u64,     // VkDeviceSize
    memory_type_bits: u32,
}

// ---------------------------------------------------------------------------
// InputEvent — must match the C++ layout exactly (C ABI)
// ---------------------------------------------------------------------------
#[repr(C)]
#[derive(Clone, Copy)]
struct InputEvent {
    event_type: u8, // InputEventType enum
    padding: [u8; 3],
    data: [u8; 20], // union payload (parsed based on event_type)
}

const INPUT_EVENT_NONE: u8 = 0;
const INPUT_EVENT_MOUSE_MOTION: u8 = 1;
const INPUT_EVENT_MOUSE_BUTTON: u8 = 2;
const INPUT_EVENT_MOUSE_WHEEL: u8 = 3;
const INPUT_EVENT_KEY_DOWN: u8 = 4;
const INPUT_EVENT_KEY_UP: u8 = 5;
const INPUT_EVENT_RESIZE: u8 = 6;

#[repr(C)]
#[derive(Clone, Copy)]
struct ResizeData {
    width: u32,
    height: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct MouseMotionData {
    x: f32,
    y: f32,
    dx: f32,
    dy: f32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct MouseButtonData {
    button: u32,
    pressed: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct MouseWheelData {
    dy: f32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct KeyData {
    scancode: u32,
}

// ---------------------------------------------------------------------------
// GPU resource types tracked by ID
// ---------------------------------------------------------------------------

/// A compiled shader module (SPIR-V + naga module for reflection)
struct ShaderModuleRes {
    module: vk::ShaderModule,
    /// The naga module, kept for entry point enumeration
    naga_module: naga::Module,
}

/// A render pipeline: VkPipeline + associated render pass + layout
struct RenderPipelineRes {
    pipeline: vk::Pipeline,
    pipeline_layout: vk::PipelineLayout,
    render_pass: vk::RenderPass,
}

/// A GPU buffer (vertex, index, uniform)
#[allow(dead_code)]
struct BufferRes {
    buffer: vk::Buffer,
    memory: vk::DeviceMemory,
    size: u64,
}

// ---------------------------------------------------------------------------
// GpuState — all Vulkan resources shared between ops and render loop
// ---------------------------------------------------------------------------
#[allow(dead_code)]
struct GpuState {
    // Core Vulkan
    instance: ash::Instance,
    device: ash::Device,
    phys_device: vk::PhysicalDevice,
    graphics_queue: vk::Queue,
    graphics_queue_family: u32,
    command_pool: vk::CommandPool,

    // Shared exportable image
    shared_img: SharedImage,
    image_format: vk::Format,

    // Framebuffer (recreated on resize or pipeline change)
    framebuffer: vk::Framebuffer,
    image_view: vk::ImageView,

    // Resource tables (ID → resource)
    next_id: u32,
    shader_modules: HashMap<u32, ShaderModuleRes>,
    render_pipelines: HashMap<u32, RenderPipelineRes>,
    buffers: HashMap<u32, BufferRes>,

    // Per-frame draw state set by TypeScript
    draw_state: DrawState,

    // JSON-encoded events for TypeScript to consume
    input_events: Vec<String>,
}

#[derive(Clone)]
struct DrawState {
    clear_color: [f32; 4],
    /// List of draw commands to execute each frame
    draw_commands: Vec<DrawCommand>,
    /// Accumulated drag rotation from TypeScript
    drag_angle: f32,
    /// Rotation speed multiplier from TypeScript
    rotation_speed: f32,
    /// Scale factor from TypeScript
    scale: f32,
}

impl Default for DrawState {
    fn default() -> Self {
        Self {
            clear_color: [0.0; 4],
            draw_commands: vec![],
            drag_angle: 0.0,
            rotation_speed: 1.0,
            scale: 1.0,
        }
    }
}

#[derive(Clone)]
struct DrawCommand {
    pipeline_id: u32,
    vertex_count: u32,
    instance_count: u32,
    vertex_buffers: Vec<u32>, // buffer IDs
}

impl GpuState {
    fn alloc_id(&mut self) -> u32 {
        let id = self.next_id;
        self.next_id += 1;
        id
    }
}

// ---------------------------------------------------------------------------
// SharedImage
// ---------------------------------------------------------------------------
struct SharedImage {
    image: vk::Image,
    memory: vk::DeviceMemory,
    handle: NativeHandle,
    width: u32,
    height: u32,
    memory_size: u64,
    memory_type_bits: u32,
}

unsafe fn find_memory_type(
    instance: &ash::Instance,
    phys_device: vk::PhysicalDevice,
    type_filter: u32,
    properties: vk::MemoryPropertyFlags,
) -> u32 {
    let mem_props = instance.get_physical_device_memory_properties(phys_device);
    for i in 0..mem_props.memory_type_count {
        if (type_filter & (1 << i)) != 0
            && mem_props.memory_types[i as usize]
                .property_flags
                .contains(properties)
        {
            return i;
        }
    }
    panic!("Failed to find suitable memory type");
}

unsafe fn create_shared_image(
    instance: &ash::Instance,
    device: &ash::Device,
    phys_device: vk::PhysicalDevice,
    width: u32,
    height: u32,
    format: vk::Format,
) -> SharedImage {
    #[cfg(unix)]
    let handle_type = vk::ExternalMemoryHandleTypeFlags::OPAQUE_FD;
    #[cfg(windows)]
    let handle_type = vk::ExternalMemoryHandleTypeFlags::OPAQUE_WIN32;

    let mut ext_mem_image_ci =
        vk::ExternalMemoryImageCreateInfo::default().handle_types(handle_type);

    let image_ci = vk::ImageCreateInfo::default()
        .push_next(&mut ext_mem_image_ci)
        .image_type(vk::ImageType::TYPE_2D)
        .format(format)
        .extent(vk::Extent3D {
            width,
            height,
            depth: 1,
        })
        .mip_levels(1)
        .array_layers(1)
        .samples(vk::SampleCountFlags::TYPE_1)
        .tiling(vk::ImageTiling::OPTIMAL)
        .usage(
            vk::ImageUsageFlags::COLOR_ATTACHMENT
                | vk::ImageUsageFlags::TRANSFER_DST
                | vk::ImageUsageFlags::TRANSFER_SRC
                | vk::ImageUsageFlags::SAMPLED,
        )
        .sharing_mode(vk::SharingMode::EXCLUSIVE)
        .initial_layout(vk::ImageLayout::UNDEFINED);

    let image = device
        .create_image(&image_ci, None)
        .expect("Failed to create image");

    let mem_reqs = device.get_image_memory_requirements(image);

    let mut export_alloc_info =
        vk::ExportMemoryAllocateInfo::default().handle_types(handle_type);

    let mem_type_index = find_memory_type(
        instance,
        phys_device,
        mem_reqs.memory_type_bits,
        vk::MemoryPropertyFlags::DEVICE_LOCAL,
    );

    let alloc_info = vk::MemoryAllocateInfo::default()
        .push_next(&mut export_alloc_info)
        .allocation_size(mem_reqs.size)
        .memory_type_index(mem_type_index);

    let memory = device
        .allocate_memory(&alloc_info, None)
        .expect("Failed to allocate memory");

    device
        .bind_image_memory(image, memory, 0)
        .expect("Failed to bind image memory");

    // Export handle
    #[cfg(unix)]
    let handle = {
        let ext = ash::khr::external_memory_fd::Device::new(instance, device);
        let get_fd_info = vk::MemoryGetFdInfoKHR::default()
            .memory(memory)
            .handle_type(vk::ExternalMemoryHandleTypeFlags::OPAQUE_FD);
        ext.get_memory_fd(&get_fd_info)
            .expect("Failed to export memory fd") as NativeHandle
    };

    #[cfg(windows)]
    let handle = {
        let ext = ash::khr::external_memory_win32::Device::new(instance, device);
        let get_handle_info = vk::MemoryGetWin32HandleInfoKHR::default()
            .memory(memory)
            .handle_type(vk::ExternalMemoryHandleTypeFlags::OPAQUE_WIN32);
        ext.get_memory_win32_handle(&get_handle_info)
            .expect("Failed to export memory handle") as NativeHandle
    };

    eprintln!(
        "[deno_renderer] Exported memory handle={:?} for {}x{} image (size={})",
        handle, width, height, mem_reqs.size
    );

    SharedImage {
        image,
        memory,
        handle,
        width,
        height,
        memory_size: mem_reqs.size,
        memory_type_bits: mem_reqs.memory_type_bits,
    }
}

unsafe fn destroy_shared_image(device: &ash::Device, img: &mut SharedImage) {
    if img.image != vk::Image::null() {
        device.destroy_image(img.image, None);
        img.image = vk::Image::null();
    }
    if img.memory != vk::DeviceMemory::null() {
        device.free_memory(img.memory, None);
        img.memory = vk::DeviceMemory::null();
    }
    #[cfg(unix)]
    if img.handle != INVALID_HANDLE {
        libc::close(img.handle as i32);
        img.handle = INVALID_HANDLE;
    }
    #[cfg(windows)]
    if img.handle != INVALID_HANDLE {
        unsafe {
            windows_sys::Win32::Foundation::CloseHandle(img.handle);
        }
        img.handle = INVALID_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Image view + framebuffer helpers
// ---------------------------------------------------------------------------
unsafe fn create_image_view(
    device: &ash::Device,
    image: vk::Image,
    format: vk::Format,
) -> vk::ImageView {
    let view_ci = vk::ImageViewCreateInfo::default()
        .image(image)
        .view_type(vk::ImageViewType::TYPE_2D)
        .format(format)
        .components(vk::ComponentMapping {
            r: vk::ComponentSwizzle::IDENTITY,
            g: vk::ComponentSwizzle::IDENTITY,
            b: vk::ComponentSwizzle::IDENTITY,
            a: vk::ComponentSwizzle::IDENTITY,
        })
        .subresource_range(vk::ImageSubresourceRange {
            aspect_mask: vk::ImageAspectFlags::COLOR,
            base_mip_level: 0,
            level_count: 1,
            base_array_layer: 0,
            layer_count: 1,
        });
    device
        .create_image_view(&view_ci, None)
        .expect("Failed to create image view")
}

unsafe fn create_framebuffer(
    device: &ash::Device,
    render_pass: vk::RenderPass,
    image_view: vk::ImageView,
    width: u32,
    height: u32,
) -> vk::Framebuffer {
    let attachments = [image_view];
    let fb_ci = vk::FramebufferCreateInfo::default()
        .render_pass(render_pass)
        .attachments(&attachments)
        .width(width)
        .height(height)
        .layers(1);
    device
        .create_framebuffer(&fb_ci, None)
        .expect("Failed to create framebuffer")
}



// ---------------------------------------------------------------------------
// Deno ops — WebGPU-like API for TypeScript
// ---------------------------------------------------------------------------

/// op_gpu_create_shader_module(wgsl_code: String) -> u32
/// Parses WGSL, validates, keeps the naga Module for reflection.
/// Does NOT yet compile to SPIR-V (that happens per entry point in pipeline creation).
#[op2(fast)]
fn op_gpu_create_shader_module(state: &mut OpState, #[string] wgsl_code: &str) -> u32 {
    let gpu = state.borrow_mut::<Arc<Mutex<GpuState>>>();
    let mut gpu = gpu.lock().unwrap();

    // Parse WGSL
    let naga_module = match naga::front::wgsl::parse_str(wgsl_code) {
        Ok(m) => m,
        Err(e) => {
            eprintln!("[gpu] WGSL parse error: {}", e);
            return u32::MAX; // error sentinel
        }
    };

    // Validate
    let mut validator = naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::all(),
    );
    if let Err(e) = validator.validate(&naga_module) {
        eprintln!("[gpu] WGSL validation error: {}", e);
        return u32::MAX;
    }

    // Create a dummy VkShaderModule — we will create real ones per entry point
    // during pipeline creation. Store the naga module for later.
    let id = gpu.alloc_id();
    gpu.shader_modules.insert(
        id,
        ShaderModuleRes {
            module: vk::ShaderModule::null(), // placeholder
            naga_module,
        },
    );

    eprintln!("[gpu] Created shader module id={}", id);
    id
}

/// op_gpu_create_render_pipeline(shader_id: u32, vert_entry: String, frag_entry: String) -> u32
/// Compiles WGSL to SPIR-V for both stages, creates VkRenderPass + VkPipeline.
#[op2(fast)]
fn op_gpu_create_render_pipeline(
    state: &mut OpState,
    shader_id: u32,
    #[string] vert_entry: &str,
    #[string] frag_entry: &str,
) -> u32 {
    let gpu = state.borrow_mut::<Arc<Mutex<GpuState>>>();
    let mut gpu = gpu.lock().unwrap();

    let wgsl_source = {
        let shader_res = match gpu.shader_modules.get(&shader_id) {
            Some(s) => s,
            None => {
                eprintln!("[gpu] Invalid shader module id={}", shader_id);
                return u32::MAX;
            }
        };
        // Re-serialize isn't possible; we need the original source.
        // Instead, we stored the naga module. We need to compile SPIR-V from it.
        // But naga::back::spv::write_vec needs a Module + ModuleInfo.
        // Let's re-validate to get ModuleInfo.
        let mut validator = naga::valid::Validator::new(
            naga::valid::ValidationFlags::all(),
            naga::valid::Capabilities::all(),
        );
        let info = match validator.validate(&shader_res.naga_module) {
            Ok(i) => i,
            Err(e) => {
                eprintln!("[gpu] Re-validation error: {}", e);
                return u32::MAX;
            }
        };
        (shader_res.naga_module.clone(), info)
    };

    let (naga_module, module_info) = wgsl_source;

    // Compile vertex SPIR-V
    let options = naga::back::spv::Options {
        lang_version: (1, 0),
        flags: naga::back::spv::WriterFlags::empty(),
        ..Default::default()
    };
    let vert_spirv = {
        let pipeline_options = naga::back::spv::PipelineOptions {
            shader_stage: naga::ShaderStage::Vertex,
            entry_point: vert_entry.to_string(),
        };
        match naga::back::spv::write_vec(&naga_module, &module_info, &options, Some(&pipeline_options))
        {
            Ok(s) => s,
            Err(e) => {
                eprintln!("[gpu] Vertex SPIR-V error: {}", e);
                return u32::MAX;
            }
        }
    };

    // Compile fragment SPIR-V
    let frag_spirv = {
        let pipeline_options = naga::back::spv::PipelineOptions {
            shader_stage: naga::ShaderStage::Fragment,
            entry_point: frag_entry.to_string(),
        };
        match naga::back::spv::write_vec(&naga_module, &module_info, &options, Some(&pipeline_options))
        {
            Ok(s) => s,
            Err(e) => {
                eprintln!("[gpu] Fragment SPIR-V error: {}", e);
                return u32::MAX;
            }
        }
    };

    unsafe {
        // Create VkShaderModules
        let vert_module_ci = vk::ShaderModuleCreateInfo::default().code(&vert_spirv);
        let vert_module = gpu
            .device
            .create_shader_module(&vert_module_ci, None)
            .expect("Failed to create vertex shader module");

        let frag_module_ci = vk::ShaderModuleCreateInfo::default().code(&frag_spirv);
        let frag_module = gpu
            .device
            .create_shader_module(&frag_module_ci, None)
            .expect("Failed to create fragment shader module");

        // naga 24 preserves entry point names from WGSL in the SPIR-V
        let vert_entry_c = std::ffi::CString::new(vert_entry).unwrap();
        let frag_entry_c = std::ffi::CString::new(frag_entry).unwrap();

        let shader_stages = [
            vk::PipelineShaderStageCreateInfo::default()
                .stage(vk::ShaderStageFlags::VERTEX)
                .module(vert_module)
                .name(&vert_entry_c),
            vk::PipelineShaderStageCreateInfo::default()
                .stage(vk::ShaderStageFlags::FRAGMENT)
                .module(frag_module)
                .name(&frag_entry_c),
        ];

        // Vertex input: no buffers by default (shader generates vertices)
        let vertex_input = vk::PipelineVertexInputStateCreateInfo::default();

        let input_assembly = vk::PipelineInputAssemblyStateCreateInfo::default()
            .topology(vk::PrimitiveTopology::TRIANGLE_LIST)
            .primitive_restart_enable(false);

        // Dynamic viewport and scissor
        let dynamic_states = [vk::DynamicState::VIEWPORT, vk::DynamicState::SCISSOR];
        let dynamic_state =
            vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&dynamic_states);

        let viewport_state = vk::PipelineViewportStateCreateInfo::default()
            .viewport_count(1)
            .scissor_count(1);

        let rasterizer = vk::PipelineRasterizationStateCreateInfo::default()
            .depth_clamp_enable(false)
            .rasterizer_discard_enable(false)
            .polygon_mode(vk::PolygonMode::FILL)
            .line_width(1.0)
            .cull_mode(vk::CullModeFlags::NONE)
            .front_face(vk::FrontFace::COUNTER_CLOCKWISE)
            .depth_bias_enable(false);

        let multisampling = vk::PipelineMultisampleStateCreateInfo::default()
            .sample_shading_enable(false)
            .rasterization_samples(vk::SampleCountFlags::TYPE_1);

        let color_blend_attachment = vk::PipelineColorBlendAttachmentState::default()
            .color_write_mask(vk::ColorComponentFlags::RGBA)
            .blend_enable(false);

        let color_blending = vk::PipelineColorBlendStateCreateInfo::default()
            .logic_op_enable(false)
            .attachments(std::slice::from_ref(&color_blend_attachment));

        // Create render pass
        let color_attachment = vk::AttachmentDescription::default()
            .format(gpu.image_format)
            .samples(vk::SampleCountFlags::TYPE_1)
            .load_op(vk::AttachmentLoadOp::CLEAR)
            .store_op(vk::AttachmentStoreOp::STORE)
            .stencil_load_op(vk::AttachmentLoadOp::DONT_CARE)
            .stencil_store_op(vk::AttachmentStoreOp::DONT_CARE)
            .initial_layout(vk::ImageLayout::UNDEFINED)
            .final_layout(vk::ImageLayout::TRANSFER_SRC_OPTIMAL);

        let color_ref = vk::AttachmentReference::default()
            .attachment(0)
            .layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL);

        let subpass = vk::SubpassDescription::default()
            .pipeline_bind_point(vk::PipelineBindPoint::GRAPHICS)
            .color_attachments(std::slice::from_ref(&color_ref));

        let dependency = vk::SubpassDependency::default()
            .src_subpass(vk::SUBPASS_EXTERNAL)
            .dst_subpass(0)
            .src_stage_mask(vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT)
            .src_access_mask(vk::AccessFlags::empty())
            .dst_stage_mask(vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT)
            .dst_access_mask(vk::AccessFlags::COLOR_ATTACHMENT_WRITE);

        let render_pass_ci = vk::RenderPassCreateInfo::default()
            .attachments(std::slice::from_ref(&color_attachment))
            .subpasses(std::slice::from_ref(&subpass))
            .dependencies(std::slice::from_ref(&dependency));

        let render_pass = gpu
            .device
            .create_render_pass(&render_pass_ci, None)
            .expect("Failed to create render pass");

        // Pipeline layout with push constants: float angle, float aspect_ratio, float scale
        let push_constant_range = vk::PushConstantRange::default()
            .stage_flags(vk::ShaderStageFlags::VERTEX)
            .offset(0)
            .size(12); // 3 floats

        let pipeline_layout_ci = vk::PipelineLayoutCreateInfo::default()
            .push_constant_ranges(std::slice::from_ref(&push_constant_range));
        let pipeline_layout = gpu
            .device
            .create_pipeline_layout(&pipeline_layout_ci, None)
            .expect("Failed to create pipeline layout");

        // Create the graphics pipeline
        let pipeline_ci = vk::GraphicsPipelineCreateInfo::default()
            .stages(&shader_stages)
            .vertex_input_state(&vertex_input)
            .input_assembly_state(&input_assembly)
            .viewport_state(&viewport_state)
            .rasterization_state(&rasterizer)
            .multisample_state(&multisampling)
            .color_blend_state(&color_blending)
            .dynamic_state(&dynamic_state)
            .layout(pipeline_layout)
            .render_pass(render_pass)
            .subpass(0);

        // Dump SPIR-V to files for debugging
        if let Ok(dir) = std::env::current_exe().map(|p| p.parent().unwrap().to_path_buf()) {
            let vert_path = dir.join("debug_vert.spv");
            let frag_path = dir.join("debug_frag.spv");
            let vert_bytes: Vec<u8> = vert_spirv.iter().flat_map(|w| w.to_le_bytes()).collect();
            let frag_bytes: Vec<u8> = frag_spirv.iter().flat_map(|w| w.to_le_bytes()).collect();
            let _ = std::fs::write(&vert_path, &vert_bytes);
            let _ = std::fs::write(&frag_path, &frag_bytes);
            eprintln!("[gpu] SPIR-V dumped to {:?} and {:?}", vert_path, frag_path);
        }

        let pipeline = match gpu
            .device
            .create_graphics_pipelines(vk::PipelineCache::null(), &[pipeline_ci], None)
        {
            Ok(pipelines) => pipelines[0],
            Err((_, err)) => {
                eprintln!("[gpu] Failed to create graphics pipeline: {:?}", err);
                eprintln!("[gpu] Vert SPIR-V: {} words, Frag SPIR-V: {} words", vert_spirv.len(), frag_spirv.len());
                gpu.device.destroy_pipeline_layout(pipeline_layout, None);
                gpu.device.destroy_render_pass(render_pass, None);
                return u32::MAX;
            }
        };

        // Cleanup shader modules (SPIR-V is baked into the pipeline now)
        gpu.device.destroy_shader_module(vert_module, None);
        gpu.device.destroy_shader_module(frag_module, None);

        // If this is the first pipeline, create the framebuffer for it
        if gpu.image_view == vk::ImageView::null() {
            gpu.image_view =
                create_image_view(&gpu.device, gpu.shared_img.image, gpu.image_format);
        }
        if gpu.framebuffer == vk::Framebuffer::null() {
            gpu.framebuffer = create_framebuffer(
                &gpu.device,
                render_pass,
                gpu.image_view,
                gpu.shared_img.width,
                gpu.shared_img.height,
            );
        }

        let id = gpu.alloc_id();
        gpu.render_pipelines.insert(
            id,
            RenderPipelineRes {
                pipeline,
                pipeline_layout,
                render_pass,
            },
        );

        eprintln!("[gpu] Created render pipeline id={}", id);
        id
    }
}

/// op_gpu_create_buffer(data: &[u8], usage: String) -> u32
/// Creates a VkBuffer with the given data. Usage: "vertex", "index", "uniform".
#[op2(fast)]
fn op_gpu_create_buffer(
    state: &mut OpState,
    #[buffer] data: &[u8],
    #[string] usage: &str,
) -> u32 {
    let gpu = state.borrow_mut::<Arc<Mutex<GpuState>>>();
    let mut gpu = gpu.lock().unwrap();

    let vk_usage = match usage {
        "vertex" => vk::BufferUsageFlags::VERTEX_BUFFER,
        "index" => vk::BufferUsageFlags::INDEX_BUFFER,
        "uniform" => vk::BufferUsageFlags::UNIFORM_BUFFER,
        _ => {
            eprintln!("[gpu] Unknown buffer usage: {}", usage);
            return u32::MAX;
        }
    };

    let size = data.len() as u64;

    unsafe {
        let buffer_ci = vk::BufferCreateInfo::default()
            .size(size)
            .usage(vk_usage)
            .sharing_mode(vk::SharingMode::EXCLUSIVE);

        let buffer = gpu
            .device
            .create_buffer(&buffer_ci, None)
            .expect("Failed to create buffer");

        let mem_reqs = gpu.device.get_buffer_memory_requirements(buffer);

        let mem_type_index = find_memory_type(
            &gpu.instance,
            gpu.phys_device,
            mem_reqs.memory_type_bits,
            vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT,
        );

        let alloc_info = vk::MemoryAllocateInfo::default()
            .allocation_size(mem_reqs.size)
            .memory_type_index(mem_type_index);

        let memory = gpu
            .device
            .allocate_memory(&alloc_info, None)
            .expect("Failed to allocate buffer memory");

        gpu.device
            .bind_buffer_memory(buffer, memory, 0)
            .expect("Failed to bind buffer memory");

        // Map and copy data
        let ptr = gpu
            .device
            .map_memory(memory, 0, size, vk::MemoryMapFlags::empty())
            .expect("Failed to map buffer memory");
        std::ptr::copy_nonoverlapping(data.as_ptr(), ptr as *mut u8, data.len());
        gpu.device.unmap_memory(memory);

        let id = gpu.alloc_id();
        gpu.buffers.insert(id, BufferRes { buffer, memory, size });

        eprintln!("[gpu] Created buffer id={} size={} usage={}", id, size, usage);
        id
    }
}

/// op_gpu_set_clear_color(r, g, b, a)
#[op2(fast)]
fn op_gpu_set_clear_color(state: &mut OpState, r: f64, g: f64, b: f64, a: f64) {
    let gpu = state.borrow_mut::<Arc<Mutex<GpuState>>>();
    let mut gpu = gpu.lock().unwrap();
    gpu.draw_state.clear_color = [r as f32, g as f32, b as f32, a as f32];
}

/// op_gpu_draw(pipeline_id: u32, vertex_count: u32, instance_count: u32)
/// Adds a draw command to the per-frame draw list.
#[op2(fast)]
fn op_gpu_draw(state: &mut OpState, pipeline_id: u32, vertex_count: u32, instance_count: u32) {
    let gpu = state.borrow_mut::<Arc<Mutex<GpuState>>>();
    let mut gpu = gpu.lock().unwrap();
    gpu.draw_state.draw_commands.push(DrawCommand {
        pipeline_id,
        vertex_count,
        instance_count,
        vertex_buffers: vec![],
    });
}

/// op_gpu_draw_with_vb(pipeline_id: u32, vertex_count: u32, instance_count: u32, buffer_id: u32)
/// Adds a draw command that binds a vertex buffer.
#[op2(fast)]
fn op_gpu_draw_with_vb(
    state: &mut OpState,
    pipeline_id: u32,
    vertex_count: u32,
    instance_count: u32,
    buffer_id: u32,
) {
    let gpu = state.borrow_mut::<Arc<Mutex<GpuState>>>();
    let mut gpu = gpu.lock().unwrap();
    gpu.draw_state.draw_commands.push(DrawCommand {
        pipeline_id,
        vertex_count,
        instance_count,
        vertex_buffers: vec![buffer_id],
    });
}

/// op_gpu_clear_draws() — reset draw list (call before re-setting draws)
#[op2(fast)]
fn op_gpu_clear_draws(state: &mut OpState) {
    let gpu = state.borrow_mut::<Arc<Mutex<GpuState>>>();
    let mut gpu = gpu.lock().unwrap();
    gpu.draw_state.draw_commands.clear();
}

#[op2]
#[string]
fn op_gpu_poll_events(state: &mut OpState) -> String {
    let gpu = state.borrow_mut::<Arc<Mutex<GpuState>>>();
    let mut gpu = gpu.lock().unwrap();
    let events = std::mem::take(&mut gpu.input_events);
    format!("[{}]", events.join(","))
}

#[op2(fast)]
fn op_gpu_set_rotation(state: &mut OpState, drag_angle: f64, speed: f64, scale: f64) {
    let gpu = state.borrow_mut::<Arc<Mutex<GpuState>>>();
    let mut gpu = gpu.lock().unwrap();
    gpu.draw_state.drag_angle = drag_angle as f32;
    gpu.draw_state.rotation_speed = speed as f32;
    gpu.draw_state.scale = scale as f32;
}

#[op2(fast)]
fn op_log(#[string] msg: &str) {
    eprintln!("[scene.ts] {}", msg);
}

// ---------------------------------------------------------------------------
// Render frame — records commands and submits
// ---------------------------------------------------------------------------
unsafe fn render_frame(
    gpu: &GpuState,
    cmd_buf: vk::CommandBuffer,
    fence: vk::Fence,
    elapsed_secs: f32,
) {
    let device = &gpu.device;
    let draw_state = &gpu.draw_state;

    device
        .reset_command_buffer(cmd_buf, vk::CommandBufferResetFlags::empty())
        .expect("Failed to reset command buffer");

    let begin_info = vk::CommandBufferBeginInfo::default()
        .flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT);
    device
        .begin_command_buffer(cmd_buf, &begin_info)
        .expect("Failed to begin command buffer");

    let has_pipeline = !draw_state.draw_commands.is_empty();

    if has_pipeline && gpu.framebuffer != vk::Framebuffer::null() {
        // Use render pass for pipeline-based rendering
        let first_pipeline_id = draw_state.draw_commands[0].pipeline_id;
        let render_pass = match gpu.render_pipelines.get(&first_pipeline_id) {
            Some(p) => p.render_pass,
            None => {
                eprintln!("[gpu] Pipeline {} not found, falling back to clear", first_pipeline_id);
                record_clear_only(device, cmd_buf, &gpu.shared_img, &draw_state.clear_color);
                device.end_command_buffer(cmd_buf).expect("end cmd buf");
                submit_and_wait(device, gpu.graphics_queue, cmd_buf, fence);
                return;
            }
        };

        let clear_values = [vk::ClearValue {
            color: vk::ClearColorValue {
                float32: draw_state.clear_color,
            },
        }];

        let render_pass_bi = vk::RenderPassBeginInfo::default()
            .render_pass(render_pass)
            .framebuffer(gpu.framebuffer)
            .render_area(vk::Rect2D {
                offset: vk::Offset2D { x: 0, y: 0 },
                extent: vk::Extent2D {
                    width: gpu.shared_img.width,
                    height: gpu.shared_img.height,
                },
            })
            .clear_values(&clear_values);

        device.cmd_begin_render_pass(cmd_buf, &render_pass_bi, vk::SubpassContents::INLINE);

        // Set dynamic viewport and scissor
        let viewport = vk::Viewport {
            x: 0.0,
            y: 0.0,
            width: gpu.shared_img.width as f32,
            height: gpu.shared_img.height as f32,
            min_depth: 0.0,
            max_depth: 1.0,
        };
        device.cmd_set_viewport(cmd_buf, 0, &[viewport]);

        let scissor = vk::Rect2D {
            offset: vk::Offset2D { x: 0, y: 0 },
            extent: vk::Extent2D {
                width: gpu.shared_img.width,
                height: gpu.shared_img.height,
            },
        };
        device.cmd_set_scissor(cmd_buf, 0, &[scissor]);

        // Execute draw commands
        for draw_cmd in &draw_state.draw_commands {
            if let Some(pipeline_res) = gpu.render_pipelines.get(&draw_cmd.pipeline_id) {
                device.cmd_bind_pipeline(
                    cmd_buf,
                    vk::PipelineBindPoint::GRAPHICS,
                    pipeline_res.pipeline,
                );

                // Push rotation constants: angle (elapsed * speed + drag), aspect, scale
                let aspect = gpu.shared_img.width as f32 / gpu.shared_img.height as f32;
                let angle = elapsed_secs * draw_state.rotation_speed + draw_state.drag_angle;
                let push_data = [angle, aspect, draw_state.scale];
                device.cmd_push_constants(
                    cmd_buf,
                    pipeline_res.pipeline_layout,
                    vk::ShaderStageFlags::VERTEX,
                    0,
                    std::slice::from_raw_parts(
                        push_data.as_ptr() as *const u8,
                        std::mem::size_of_val(&push_data),
                    ),
                );

                // Bind vertex buffers if any
                for (i, &buf_id) in draw_cmd.vertex_buffers.iter().enumerate() {
                    if let Some(buf_res) = gpu.buffers.get(&buf_id) {
                        device.cmd_bind_vertex_buffers(
                            cmd_buf,
                            i as u32,
                            &[buf_res.buffer],
                            &[0],
                        );
                    }
                }

                device.cmd_draw(
                    cmd_buf,
                    draw_cmd.vertex_count,
                    draw_cmd.instance_count,
                    0,
                    0,
                );
            }
        }

        device.cmd_end_render_pass(cmd_buf);
    } else {
        // Fallback: just clear the image (no pipeline)
        record_clear_only(device, cmd_buf, &gpu.shared_img, &draw_state.clear_color);
    }

    device
        .end_command_buffer(cmd_buf)
        .expect("Failed to end command buffer");

    submit_and_wait(device, gpu.graphics_queue, cmd_buf, fence);
}

unsafe fn record_clear_only(
    device: &ash::Device,
    cmd_buf: vk::CommandBuffer,
    shared_img: &SharedImage,
    clear_color: &[f32; 4],
) {
    // Transition UNDEFINED -> TRANSFER_DST
    let barrier = vk::ImageMemoryBarrier::default()
        .src_access_mask(vk::AccessFlags::empty())
        .dst_access_mask(vk::AccessFlags::TRANSFER_WRITE)
        .old_layout(vk::ImageLayout::UNDEFINED)
        .new_layout(vk::ImageLayout::TRANSFER_DST_OPTIMAL)
        .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .image(shared_img.image)
        .subresource_range(vk::ImageSubresourceRange {
            aspect_mask: vk::ImageAspectFlags::COLOR,
            base_mip_level: 0,
            level_count: 1,
            base_array_layer: 0,
            layer_count: 1,
        });

    device.cmd_pipeline_barrier(
        cmd_buf,
        vk::PipelineStageFlags::TOP_OF_PIPE,
        vk::PipelineStageFlags::TRANSFER,
        vk::DependencyFlags::empty(),
        &[],
        &[],
        &[barrier],
    );

    let clear = vk::ClearColorValue {
        float32: *clear_color,
    };
    let range = vk::ImageSubresourceRange {
        aspect_mask: vk::ImageAspectFlags::COLOR,
        base_mip_level: 0,
        level_count: 1,
        base_array_layer: 0,
        layer_count: 1,
    };
    device.cmd_clear_color_image(
        cmd_buf,
        shared_img.image,
        vk::ImageLayout::TRANSFER_DST_OPTIMAL,
        &clear,
        &[range],
    );

    // Transition TRANSFER_DST -> TRANSFER_SRC (presenter expects this)
    let barrier2 = vk::ImageMemoryBarrier::default()
        .src_access_mask(vk::AccessFlags::TRANSFER_WRITE)
        .dst_access_mask(vk::AccessFlags::empty())
        .old_layout(vk::ImageLayout::TRANSFER_DST_OPTIMAL)
        .new_layout(vk::ImageLayout::TRANSFER_SRC_OPTIMAL)
        .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .image(shared_img.image)
        .subresource_range(vk::ImageSubresourceRange {
            aspect_mask: vk::ImageAspectFlags::COLOR,
            base_mip_level: 0,
            level_count: 1,
            base_array_layer: 0,
            layer_count: 1,
        });

    device.cmd_pipeline_barrier(
        cmd_buf,
        vk::PipelineStageFlags::TRANSFER,
        vk::PipelineStageFlags::BOTTOM_OF_PIPE,
        vk::DependencyFlags::empty(),
        &[],
        &[],
        &[barrier2],
    );
}

unsafe fn submit_and_wait(
    device: &ash::Device,
    queue: vk::Queue,
    cmd_buf: vk::CommandBuffer,
    fence: vk::Fence,
) {
    device.reset_fences(&[fence]).expect("Failed to reset fences");

    let submit_info = vk::SubmitInfo::default().command_buffers(std::slice::from_ref(&cmd_buf));

    device
        .queue_submit(queue, &[submit_info], fence)
        .expect("Failed to submit queue");

    device
        .wait_for_fences(&[fence], true, u64::MAX)
        .expect("Failed to wait for fence");
}

// ---------------------------------------------------------------------------
// TCP transport (cross-platform) — unchanged from original
// ---------------------------------------------------------------------------
mod transport {
    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};
    use std::time::Duration;

    pub struct TcpTransport {
        listener: Option<TcpListener>,
        stream: Option<TcpStream>,
        #[allow(dead_code)]
        remote_pid: u32,
        #[cfg(windows)]
        remote_process: *mut std::ffi::c_void,
    }

    fn write_all(stream: &mut TcpStream, data: &[u8]) -> Result<(), String> {
        stream
            .write_all(data)
            .map_err(|e| format!("write failed: {}", e))
    }

    fn read_exact(stream: &mut TcpStream, buf: &mut [u8]) -> Result<(), String> {
        stream
            .read_exact(buf)
            .map_err(|e| format!("read failed: {}", e))
    }

    impl TcpTransport {
        pub fn new() -> Self {
            TcpTransport {
                listener: None,
                stream: None,
                remote_pid: 0,
                #[cfg(windows)]
                remote_process: std::ptr::null_mut(),
            }
        }

        pub fn listen(&mut self, port: &str) -> Result<(), String> {
            let addr = format!("127.0.0.1:{}", port);
            let listener =
                TcpListener::bind(&addr).map_err(|e| format!("bind({}) failed: {}", addr, e))?;
            self.listener = Some(listener);
            Ok(())
        }

        pub fn accept(&mut self) -> Result<(), String> {
            let listener = self.listener.as_ref().ok_or("not listening")?;
            let (stream, _addr) = listener
                .accept()
                .map_err(|e| format!("accept() failed: {}", e))?;
            stream.set_nodelay(true).ok();
            self.stream = Some(stream);
            self.exchange_pids(true)
        }

        fn exchange_pids(&mut self, is_server: bool) -> Result<(), String> {
            let my_pid = std::process::id();
            let stream = self.stream.as_mut().unwrap();

            if is_server {
                write_all(stream, &my_pid.to_le_bytes())?;
                let mut buf = [0u8; 4];
                read_exact(stream, &mut buf)?;
                self.remote_pid = u32::from_le_bytes(buf);
            } else {
                let mut buf = [0u8; 4];
                read_exact(stream, &mut buf)?;
                self.remote_pid = u32::from_le_bytes(buf);
                write_all(stream, &my_pid.to_le_bytes())?;
            }
            eprintln!(
                "[transport] PID exchange: local={}, remote={}",
                my_pid, self.remote_pid
            );

            #[cfg(windows)]
            {
                use windows_sys::Win32::System::Threading::OpenProcess;
                use windows_sys::Win32::System::Threading::PROCESS_DUP_HANDLE;
                self.remote_process =
                    unsafe { OpenProcess(PROCESS_DUP_HANDLE, 0, self.remote_pid) };
                if self.remote_process.is_null() {
                    return Err(format!("OpenProcess({}) failed", self.remote_pid));
                }
            }

            Ok(())
        }

        pub fn send_handle(
            &mut self,
            handle: super::NativeHandle,
            data: &[u8],
        ) -> Result<(), String> {
            let stream = self.stream.as_mut().ok_or("not connected")?;

            #[cfg(unix)]
            let handle_val = handle as u64;

            #[cfg(windows)]
            let handle_val = {
                use windows_sys::Win32::Foundation::{DuplicateHandle, DUPLICATE_SAME_ACCESS};
                use windows_sys::Win32::System::Threading::GetCurrentProcess;
                let mut remote_handle: *mut std::ffi::c_void = std::ptr::null_mut();
                let ok = unsafe {
                    DuplicateHandle(
                        GetCurrentProcess(),
                        handle,
                        self.remote_process,
                        &mut remote_handle,
                        0,
                        0,
                        DUPLICATE_SAME_ACCESS,
                    )
                };
                if ok == 0 {
                    return Err("DuplicateHandle failed".to_string());
                }
                remote_handle as u64
            };

            write_all(stream, &handle_val.to_le_bytes())?;
            if !data.is_empty() {
                write_all(stream, data)?;
            }
            Ok(())
        }

        pub fn recv_data_non_blocking(&self, buf: &mut [u8]) -> Option<usize> {
            use std::io::ErrorKind;
            let stream = self.stream.as_ref()?;
            // Set a very short read timeout instead of toggling nonblocking mode
            stream.set_read_timeout(Some(Duration::from_millis(1))).ok();
            let result = (&*stream).read(buf);
            stream.set_read_timeout(None).ok(); // back to blocking
            match result {
                Ok(0) => None,
                Ok(n) => Some(n),
                Err(ref e) if e.kind() == ErrorKind::WouldBlock || e.kind() == ErrorKind::TimedOut => None,
                Err(_) => None,
            }
        }

        pub fn close(&mut self) {
            self.stream = None;
            self.listener = None;
            #[cfg(windows)]
            if !self.remote_process.is_null() {
                unsafe {
                    windows_sys::Win32::Foundation::CloseHandle(self.remote_process);
                }
                self.remote_process = std::ptr::null_mut();
            }
        }
    }

    impl Drop for TcpTransport {
        fn drop(&mut self) {
            self.close();
        }
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
fn main() {
    // Parse command line args
    let args: Vec<String> = std::env::args().collect();
    let mut port = "9710".to_string();
    let mut script_path = String::new();

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--port" | "-p" => {
                if i + 1 < args.len() {
                    i += 1;
                    port = args[i].clone();
                }
            }
            "--script" => {
                if i + 1 < args.len() {
                    i += 1;
                    script_path = args[i].clone();
                }
            }
            _ => {}
        }
        i += 1;
    }

    if script_path.is_empty() {
        if let Ok(exe_path) = std::env::current_exe() {
            if let Some(dir) = exe_path.parent() {
                script_path = dir.join("scene.ts").to_string_lossy().to_string();
            }
        }
        if script_path.is_empty() {
            script_path = "scene.ts".to_string();
        }
    }

    // Set up graceful shutdown
    let running = Arc::new(AtomicBool::new(true));
    {
        let running = running.clone();
        ctrlc_simple(running);
    }

    // -----------------------------------------------------------------------
    // 1. Vulkan instance (headless, no surface extensions)
    // -----------------------------------------------------------------------
    eprintln!("[deno_renderer] Creating Vulkan instance (headless)...");

    let entry = unsafe { ash::Entry::load().expect("Failed to load Vulkan entry") };

    let app_name = c"deno_renderer";
    let engine_name = c"none";

    let app_info = vk::ApplicationInfo::default()
        .application_name(app_name)
        .application_version(vk::make_api_version(0, 1, 0, 0))
        .engine_name(engine_name)
        .engine_version(vk::make_api_version(0, 1, 0, 0))
        .api_version(vk::API_VERSION_1_1);

    let instance_extensions = [
        ash::khr::external_memory_capabilities::NAME.as_ptr(),
        ash::khr::external_semaphore_capabilities::NAME.as_ptr(),
        ash::khr::get_physical_device_properties2::NAME.as_ptr(),
        ash::ext::debug_utils::NAME.as_ptr(),
    ];

    let validation_layer = c"VK_LAYER_KHRONOS_validation";
    let layers = [validation_layer.as_ptr()];

    let instance_ci = vk::InstanceCreateInfo::default()
        .application_info(&app_info)
        .enabled_extension_names(&instance_extensions)
        .enabled_layer_names(&layers);

    let instance = unsafe {
        entry
            .create_instance(&instance_ci, None)
            .expect("Failed to create VkInstance")
    };
    eprintln!("[deno_renderer] VkInstance created");

    // -----------------------------------------------------------------------
    // 2. Pick physical device
    // -----------------------------------------------------------------------
    let (phys_device, graphics_queue_family) = unsafe {
        let phys_devices = instance
            .enumerate_physical_devices()
            .expect("Failed to enumerate physical devices");
        if phys_devices.is_empty() {
            panic!("No Vulkan physical devices found");
        }

        let mut selected = None;
        for pd in &phys_devices {
            let queue_families = instance.get_physical_device_queue_family_properties(*pd);
            for (idx, qf) in queue_families.iter().enumerate() {
                if qf.queue_flags.contains(vk::QueueFlags::GRAPHICS) {
                    selected = Some((*pd, idx as u32));
                    break;
                }
            }
            if selected.is_some() {
                break;
            }
        }
        selected.expect("No physical device with graphics queue found")
    };

    unsafe {
        let props = instance.get_physical_device_properties(phys_device);
        let name = CStr::from_ptr(props.device_name.as_ptr());
        eprintln!("[deno_renderer] Using device: {}", name.to_string_lossy());
    }

    // -----------------------------------------------------------------------
    // 3. Create logical device (with external memory extensions)
    // -----------------------------------------------------------------------
    let queue_priority = [1.0f32];
    let queue_ci = vk::DeviceQueueCreateInfo::default()
        .queue_family_index(graphics_queue_family)
        .queue_priorities(&queue_priority);

    #[cfg(unix)]
    let device_extensions = [
        ash::khr::external_memory::NAME.as_ptr(),
        ash::khr::external_memory_fd::NAME.as_ptr(),
        ash::khr::dedicated_allocation::NAME.as_ptr(),
        ash::khr::get_memory_requirements2::NAME.as_ptr(),
    ];

    #[cfg(windows)]
    let device_extensions = [
        ash::khr::external_memory::NAME.as_ptr(),
        ash::khr::external_memory_win32::NAME.as_ptr(),
        ash::khr::dedicated_allocation::NAME.as_ptr(),
        ash::khr::get_memory_requirements2::NAME.as_ptr(),
    ];

    let device_ci = vk::DeviceCreateInfo::default()
        .queue_create_infos(std::slice::from_ref(&queue_ci))
        .enabled_extension_names(&device_extensions);

    let device = unsafe {
        instance
            .create_device(phys_device, &device_ci, None)
            .expect("Failed to create VkDevice")
    };
    eprintln!("[deno_renderer] VkDevice created");

    let graphics_queue = unsafe { device.get_device_queue(graphics_queue_family, 0) };

    // -----------------------------------------------------------------------
    // 4. Command pool
    // -----------------------------------------------------------------------
    let pool_ci = vk::CommandPoolCreateInfo::default()
        .flags(vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER)
        .queue_family_index(graphics_queue_family);

    let command_pool = unsafe {
        device
            .create_command_pool(&pool_ci, None)
            .expect("Failed to create command pool")
    };

    // -----------------------------------------------------------------------
    // 5. Shared exportable image
    // -----------------------------------------------------------------------
    let image_format = vk::Format::R8G8B8A8_UNORM;
    let initial_width: u32 = 640;
    let initial_height: u32 = 480;

    let shared_img = unsafe {
        create_shared_image(
            &instance,
            &device,
            phys_device,
            initial_width,
            initial_height,
            image_format,
        )
    };
    eprintln!("[deno_renderer] Shared image created");

    // -----------------------------------------------------------------------
    // 6. Build GpuState
    // -----------------------------------------------------------------------
    let gpu_state = Arc::new(Mutex::new(GpuState {
        instance: instance.clone(),
        device: device.clone(),
        phys_device,
        graphics_queue,
        graphics_queue_family,
        command_pool,
        shared_img,
        image_format,
        framebuffer: vk::Framebuffer::null(),
        image_view: vk::ImageView::null(),
        next_id: 0,
        shader_modules: HashMap::new(),
        render_pipelines: HashMap::new(),
        buffers: HashMap::new(),
        draw_state: DrawState {
            clear_color: [0.1, 0.1, 0.1, 1.0],
            draw_commands: vec![],
            drag_angle: 0.0,
            rotation_speed: 1.0,
            scale: 1.0,
        },
        input_events: Vec::new(),
    }));

    // -----------------------------------------------------------------------
    // 7. Socket: listen and accept presenter connection
    // -----------------------------------------------------------------------
    let mut transport = transport::TcpTransport::new();
    transport
        .listen(&port)
        .unwrap_or_else(|e| panic!("Failed to listen on {}: {}", port, e));

    eprintln!(
        "[deno_renderer] Waiting for presenter to connect on {}...",
        port
    );

    transport
        .accept()
        .unwrap_or_else(|e| panic!("Failed to accept connection: {}", e));
    eprintln!("[deno_renderer] Presenter connected");

    // Send the handle + SharedSurfaceInfo
    {
        let gpu = gpu_state.lock().unwrap();
        let surface_info = SharedSurfaceInfo {
            width: gpu.shared_img.width,
            height: gpu.shared_img.height,
            format: image_format.as_raw() as u32,
            memory_size: gpu.shared_img.memory_size,
            memory_type_bits: gpu.shared_img.memory_type_bits,
        };

        let info_bytes = unsafe {
            std::slice::from_raw_parts(
                &surface_info as *const SharedSurfaceInfo as *const u8,
                std::mem::size_of::<SharedSurfaceInfo>(),
            )
        };

        transport
            .send_handle(gpu.shared_img.handle, info_bytes)
            .unwrap_or_else(|e| panic!("Failed to send handle: {}", e));
    }
    eprintln!("[deno_renderer] Surface shared, handle sent.");

    // -----------------------------------------------------------------------
    // 8. Deno runtime + render loop (JsRuntime stays alive through loop)
    // -----------------------------------------------------------------------
    {
        let gpu_clone = gpu_state.clone();

        deno_core::extension!(
            scene_ext,
            ops = [
                op_gpu_create_shader_module,
                op_gpu_create_render_pipeline,
                op_gpu_create_buffer,
                op_gpu_set_clear_color,
                op_gpu_draw,
                op_gpu_draw_with_vb,
                op_gpu_clear_draws,
                op_gpu_poll_events,
                op_gpu_set_rotation,
                op_log
            ]
        );

        let tokio_rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .expect("Failed to create tokio runtime");

        tokio_rt.block_on(async {
            let mut runtime = JsRuntime::new(RuntimeOptions {
                extensions: vec![scene_ext::init()],
                ..Default::default()
            });

            runtime.op_state().borrow_mut().put(gpu_clone);

            // Run initial script
            match std::fs::read_to_string(&script_path) {
                Ok(code) => {
                    eprintln!("[deno_renderer] Running script: {}", script_path);
                    if let Err(e) = runtime.execute_script("<scene>", code) {
                        eprintln!("[deno_renderer] Script error: {}", e);
                    }
                    if let Err(e) = runtime.run_event_loop(Default::default()).await {
                        eprintln!("[deno_renderer] Event loop error: {}", e);
                    }
                }
                Err(e) => {
                    eprintln!(
                        "[deno_renderer] Warning: could not load script '{}': {}",
                        script_path, e
                    );
                    eprintln!("[deno_renderer] Using default clear color (dark gray)");
                }
            }

            // ---------------------------------------------------------------
            // 9. Allocate command buffer and fence for render loop
            // ---------------------------------------------------------------
            let cmd_alloc_info = vk::CommandBufferAllocateInfo::default()
                .command_pool(command_pool)
                .level(vk::CommandBufferLevel::PRIMARY)
                .command_buffer_count(1);

            let cmd_buf = unsafe {
                device
                    .allocate_command_buffers(&cmd_alloc_info)
                    .expect("Failed to allocate command buffer")[0]
            };

            let fence_ci = vk::FenceCreateInfo::default();
            let fence = unsafe {
                device
                    .create_fence(&fence_ci, None)
                    .expect("Failed to create fence")
            };

            // ---------------------------------------------------------------
            // 10. Render loop — inside tokio block, JsRuntime alive
            // ---------------------------------------------------------------
            eprintln!("[deno_renderer] Entering render loop (Ctrl+C to exit)...");

            let start_time = std::time::Instant::now();

            while running.load(Ordering::Relaxed) {
                // Poll for input events (non-blocking)
                let mut event_buf = [0u8; std::mem::size_of::<InputEvent>()];
                while let Some(n) = transport.recv_data_non_blocking(&mut event_buf) {
                    if n == std::mem::size_of::<InputEvent>() {
                        let event: InputEvent =
                            unsafe { std::ptr::read(event_buf.as_ptr() as *const InputEvent) };

                        // Queue JSON event for TypeScript consumption
                        {
                            let mut gpu = gpu_state.lock().unwrap();
                            match event.event_type {
                                INPUT_EVENT_MOUSE_MOTION => {
                                    let d: MouseMotionData = unsafe {
                                        std::ptr::read(event.data.as_ptr() as *const MouseMotionData)
                                    };
                                    gpu.input_events.push(format!(
                                        r#"{{"type":"mouse_motion","x":{},"y":{},"dx":{},"dy":{}}}"#,
                                        d.x, d.y, d.dx, d.dy
                                    ));
                                }
                                INPUT_EVENT_MOUSE_BUTTON => {
                                    let d: MouseButtonData = unsafe {
                                        std::ptr::read(event.data.as_ptr() as *const MouseButtonData)
                                    };
                                    gpu.input_events.push(format!(
                                        r#"{{"type":"mouse_button","button":{},"pressed":{}}}"#,
                                        d.button, d.pressed
                                    ));
                                }
                                INPUT_EVENT_MOUSE_WHEEL => {
                                    let d: MouseWheelData = unsafe {
                                        std::ptr::read(event.data.as_ptr() as *const MouseWheelData)
                                    };
                                    gpu.input_events.push(format!(
                                        r#"{{"type":"mouse_wheel","dy":{}}}"#,
                                        d.dy
                                    ));
                                }
                                INPUT_EVENT_KEY_DOWN => {
                                    let d: KeyData = unsafe {
                                        std::ptr::read(event.data.as_ptr() as *const KeyData)
                                    };
                                    gpu.input_events.push(format!(
                                        r#"{{"type":"key_down","scancode":{}}}"#,
                                        d.scancode
                                    ));
                                }
                                INPUT_EVENT_KEY_UP => {
                                    let d: KeyData = unsafe {
                                        std::ptr::read(event.data.as_ptr() as *const KeyData)
                                    };
                                    gpu.input_events.push(format!(
                                        r#"{{"type":"key_up","scancode":{}}}"#,
                                        d.scancode
                                    ));
                                }
                                INPUT_EVENT_RESIZE => {
                                    let resize: ResizeData = unsafe {
                                        std::ptr::read(event.data.as_ptr() as *const ResizeData)
                                    };
                                    gpu.input_events.push(format!(
                                        r#"{{"type":"resize","width":{},"height":{}}}"#,
                                        resize.width, resize.height
                                    ));
                                }
                                INPUT_EVENT_NONE | _ => {}
                            }
                        }

                        // Handle resize (recreate shared image)
                        if event.event_type == INPUT_EVENT_RESIZE {
                            let resize: ResizeData =
                                unsafe { std::ptr::read(event.data.as_ptr() as *const ResizeData) };
                            let new_w = resize.width;
                            let new_h = resize.height;

                            let mut gpu = gpu_state.lock().unwrap();
                            if new_w > 0
                                && new_h > 0
                                && (new_w != gpu.shared_img.width || new_h != gpu.shared_img.height)
                            {
                                unsafe {
                                    device.device_wait_idle().expect("device_wait_idle failed");

                                    // Destroy old framebuffer + image view
                                    if gpu.framebuffer != vk::Framebuffer::null() {
                                        device.destroy_framebuffer(gpu.framebuffer, None);
                                        gpu.framebuffer = vk::Framebuffer::null();
                                    }
                                    if gpu.image_view != vk::ImageView::null() {
                                        device.destroy_image_view(gpu.image_view, None);
                                        gpu.image_view = vk::ImageView::null();
                                    }

                                    destroy_shared_image(&device, &mut gpu.shared_img);
                                    gpu.shared_img = create_shared_image(
                                        &instance,
                                        &device,
                                        phys_device,
                                        new_w,
                                        new_h,
                                        image_format,
                                    );

                                    // Recreate image view + framebuffer if we have a pipeline
                                    let first_render_pass = gpu
                                        .render_pipelines
                                        .values()
                                        .next()
                                        .map(|p| p.render_pass);
                                    if let Some(render_pass) = first_render_pass {
                                        gpu.image_view = create_image_view(
                                            &device,
                                            gpu.shared_img.image,
                                            image_format,
                                        );
                                        gpu.framebuffer = create_framebuffer(
                                            &device,
                                            render_pass,
                                            gpu.image_view,
                                            new_w,
                                            new_h,
                                        );
                                    }
                                }

                                let si = SharedSurfaceInfo {
                                    width: gpu.shared_img.width,
                                    height: gpu.shared_img.height,
                                    format: image_format.as_raw() as u32,
                                    memory_size: gpu.shared_img.memory_size,
                                    memory_type_bits: gpu.shared_img.memory_type_bits,
                                };
                                let si_bytes = unsafe {
                                    std::slice::from_raw_parts(
                                        &si as *const SharedSurfaceInfo as *const u8,
                                        std::mem::size_of::<SharedSurfaceInfo>(),
                                    )
                                };
                                let _ = transport.send_handle(gpu.shared_img.handle, si_bytes);
                                eprintln!("[deno_renderer] Resized to {}x{}", new_w, new_h);
                            }
                        }
                    }
                }

                // Call JS frame callback
                let elapsed = start_time.elapsed().as_secs_f32();
                let frame_code = format!("if(globalThis.__frame)globalThis.__frame({})", elapsed);
                let _ = runtime.execute_script("<frame>", frame_code);

                // Render frame
                {
                    let gpu = gpu_state.lock().unwrap();
                    unsafe {
                        render_frame(&gpu, cmd_buf, fence, elapsed);
                    }
                }

                // Target ~60fps
                std::thread::sleep(Duration::from_millis(16));
            }

            // Cleanup cmd_buf and fence inside the block where they were created
            unsafe {
                device.device_wait_idle().expect("device_wait_idle failed");
                device.destroy_fence(fence, None);
                device.free_command_buffers(command_pool, &[cmd_buf]);
            }
        });
    }

    eprintln!("\n[deno_renderer] Shutting down...");

    // -----------------------------------------------------------------------
    // 11. Cleanup
    // -----------------------------------------------------------------------
    unsafe {
        device.device_wait_idle().expect("device_wait_idle failed");
    }

    transport.close();

    {
        let mut gpu = gpu_state.lock().unwrap();
        unsafe {
            // Destroy pipelines
            for (_id, p) in gpu.render_pipelines.drain() {
                device.destroy_pipeline(p.pipeline, None);
                device.destroy_pipeline_layout(p.pipeline_layout, None);
                device.destroy_render_pass(p.render_pass, None);
            }

            // Destroy shader modules
            for (_id, s) in gpu.shader_modules.drain() {
                if s.module != vk::ShaderModule::null() {
                    device.destroy_shader_module(s.module, None);
                }
            }

            // Destroy buffers
            for (_id, b) in gpu.buffers.drain() {
                device.destroy_buffer(b.buffer, None);
                device.free_memory(b.memory, None);
            }

            // Destroy framebuffer + image view
            if gpu.framebuffer != vk::Framebuffer::null() {
                device.destroy_framebuffer(gpu.framebuffer, None);
            }
            if gpu.image_view != vk::ImageView::null() {
                device.destroy_image_view(gpu.image_view, None);
            }

            // fence and cmd_buf already cleaned up in tokio block
            destroy_shared_image(&device, &mut gpu.shared_img);
            device.destroy_command_pool(command_pool, None);
            device.destroy_device(None);
            instance.destroy_instance(None);
        }
    }

    eprintln!("[deno_renderer] Cleanup complete");
}

// ---------------------------------------------------------------------------
// Simple ctrlc handler using signal
// ---------------------------------------------------------------------------
#[cfg(unix)]
fn ctrlc_simple(running: Arc<AtomicBool>) {
    unsafe {
        static mut RUNNING: Option<*const AtomicBool> = None;
        RUNNING = Some(Arc::into_raw(running));

        extern "C" fn handler(_: libc::c_int) {
            unsafe {
                if let Some(ptr) = RUNNING {
                    (*ptr).store(false, Ordering::Relaxed);
                }
            }
        }

        let mut sa: libc::sigaction = std::mem::zeroed();
        sa.sa_sigaction = handler as *const () as usize;
        libc::sigemptyset(&mut sa.sa_mask);
        sa.sa_flags = 0;
        libc::sigaction(libc::SIGINT, &sa, std::ptr::null_mut());
        libc::sigaction(libc::SIGTERM, &sa, std::ptr::null_mut());
    }
}

#[cfg(windows)]
fn ctrlc_simple(running: Arc<AtomicBool>) {
    unsafe {
        use windows_sys::Win32::System::Console::SetConsoleCtrlHandler;
        static mut RUNNING: Option<*const AtomicBool> = None;
        RUNNING = Some(Arc::into_raw(running));

        unsafe extern "system" fn handler(_: u32) -> i32 {
            if let Some(ptr) = RUNNING {
                (*ptr).store(false, Ordering::Relaxed);
            }
            1
        }

        SetConsoleCtrlHandler(Some(handler), 1);
    }
}
