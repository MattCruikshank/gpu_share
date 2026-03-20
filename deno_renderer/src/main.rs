// deno_renderer: Headless Vulkan renderer with embedded Deno TypeScript runtime.
// Creates an exportable VkImage, runs a TypeScript scene script to control
// the clear color, and shares the image with the presenter via TCP.

use ash::vk;
use deno_core::{op2, JsRuntime, OpState, RuntimeOptions};
use std::ffi::CStr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

#[cfg(unix)]
type NativeHandle = i32;    // fd
#[cfg(windows)]
type NativeHandle = isize;  // HANDLE as isize for easy storage
#[cfg(unix)]
const INVALID_HANDLE: NativeHandle = -1;
#[cfg(windows)]
const INVALID_HANDLE: NativeHandle = 0;  // NULL HANDLE

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

// InputEventType values:
const _INPUT_EVENT_NONE: u8 = 0;
const _INPUT_EVENT_MOUSE_MOTION: u8 = 1;
const _INPUT_EVENT_MOUSE_BUTTON: u8 = 2;
const _INPUT_EVENT_MOUSE_WHEEL: u8 = 3;
const _INPUT_EVENT_KEY_DOWN: u8 = 4;
const _INPUT_EVENT_KEY_UP: u8 = 5;
const INPUT_EVENT_RESIZE: u8 = 6;

// Resize payload sits at the start of the data union
#[repr(C)]
#[derive(Clone, Copy)]
struct ResizeData {
    width: u32,
    height: u32,
}

// ---------------------------------------------------------------------------
// SceneState — shared between deno ops and render loop
// ---------------------------------------------------------------------------
#[derive(Clone)]
struct SceneState {
    clear_color: [f32; 4],
    rotation_speed: f32,
}

impl Default for SceneState {
    fn default() -> Self {
        SceneState {
            clear_color: [0.1, 0.1, 0.1, 1.0],
            rotation_speed: 1.0,
        }
    }
}

// ---------------------------------------------------------------------------
// deno_core ops
// ---------------------------------------------------------------------------
#[op2(fast)]
fn op_set_clear_color(state: &mut OpState, r: f64, g: f64, b: f64) {
    let scene = state.borrow_mut::<Arc<Mutex<SceneState>>>();
    let mut s = scene.lock().unwrap();
    s.clear_color = [r as f32, g as f32, b as f32, 1.0];
}

#[op2(fast)]
fn op_set_rotation_speed(state: &mut OpState, speed: f64) {
    let scene = state.borrow_mut::<Arc<Mutex<SceneState>>>();
    let mut s = scene.lock().unwrap();
    s.rotation_speed = speed as f32;
}

#[op2(fast)]
fn op_log(#[string] msg: &str) {
    eprintln!("[scene.ts] {}", msg);
}

// ---------------------------------------------------------------------------
// TCP transport (cross-platform)
// ---------------------------------------------------------------------------
mod transport {
    use std::io::{Read, Write};
    use std::net::{TcpListener, TcpStream};

    pub struct TcpTransport {
        listener: Option<TcpListener>,
        stream: Option<TcpStream>,
        #[allow(dead_code)]
        remote_pid: u32,
        #[cfg(windows)]
        remote_process: isize,  // HANDLE from OpenProcess
    }

    fn write_all(stream: &mut TcpStream, data: &[u8]) -> Result<(), String> {
        stream.write_all(data).map_err(|e| format!("write failed: {}", e))
    }

    fn read_exact(stream: &mut TcpStream, buf: &mut [u8]) -> Result<(), String> {
        stream.read_exact(buf).map_err(|e| format!("read failed: {}", e))
    }

    impl TcpTransport {
        pub fn new() -> Self {
            TcpTransport {
                listener: None,
                stream: None,
                remote_pid: 0,
                #[cfg(windows)]
                remote_process: 0,
            }
        }

        pub fn listen(&mut self, port: &str) -> Result<(), String> {
            let addr = format!("127.0.0.1:{}", port);
            let listener = TcpListener::bind(&addr)
                .map_err(|e| format!("bind({}) failed: {}", addr, e))?;
            self.listener = Some(listener);
            Ok(())
        }

        pub fn accept(&mut self) -> Result<(), String> {
            let listener = self.listener.as_ref()
                .ok_or("not listening")?;
            let (stream, _addr) = listener.accept()
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
            eprintln!("[transport] PID exchange: local={}, remote={}", my_pid, self.remote_pid);

            #[cfg(windows)]
            {
                use windows_sys::Win32::System::Threading::OpenProcess;
                use windows_sys::Win32::System::Threading::PROCESS_DUP_HANDLE;
                self.remote_process = unsafe { OpenProcess(PROCESS_DUP_HANDLE, 0, self.remote_pid) };
                if self.remote_process == 0 {
                    return Err(format!("OpenProcess({}) failed", self.remote_pid));
                }
            }

            Ok(())
        }

        /// Send a handle (fd on Linux, HANDLE on Windows) + data payload over TCP.
        /// On Linux the receiver uses pidfd_getfd to clone the fd.
        /// On Windows the renderer DuplicateHandle's into the remote process first.
        pub fn send_handle(
            &mut self,
            handle: super::NativeHandle,
            data: &[u8],
        ) -> Result<(), String> {
            let stream = self.stream.as_mut()
                .ok_or("not connected")?;

            #[cfg(unix)]
            let handle_val = handle as u64;

            #[cfg(windows)]
            let handle_val = {
                use windows_sys::Win32::Foundation::{DuplicateHandle, DUPLICATE_SAME_ACCESS};
                use windows_sys::Win32::System::Threading::GetCurrentProcess;
                let mut remote_handle: isize = 0;
                let ok = unsafe {
                    DuplicateHandle(
                        GetCurrentProcess(),
                        handle,
                        self.remote_process,
                        &mut remote_handle,
                        0,
                        0, // FALSE
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

        /// Non-blocking receive of plain data.
        pub fn recv_data_non_blocking(
            &self,
            buf: &mut [u8],
        ) -> Option<usize> {
            let stream = self.stream.as_ref()?;
            stream.set_nonblocking(true).ok();
            let result = (&*stream).read(buf);
            stream.set_nonblocking(false).ok();
            match result {
                Ok(0) => None,
                Ok(n) => Some(n),
                Err(_) => None,
            }
        }

        pub fn close(&mut self) {
            self.stream = None;
            self.listener = None;
            #[cfg(windows)]
            if self.remote_process != 0 {
                unsafe { windows_sys::Win32::Foundation::CloseHandle(self.remote_process); }
                self.remote_process = 0;
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
// Vulkan helpers
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Shared image state
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

    // Create image with external memory support
    let mut ext_mem_image_ci = vk::ExternalMemoryImageCreateInfo::default()
        .handle_types(handle_type);

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
            vk::ImageUsageFlags::TRANSFER_DST
                | vk::ImageUsageFlags::TRANSFER_SRC
                | vk::ImageUsageFlags::SAMPLED,
        )
        .sharing_mode(vk::SharingMode::EXCLUSIVE)
        .initial_layout(vk::ImageLayout::UNDEFINED);

    let image = device.create_image(&image_ci, None).expect("Failed to create image");

    // Get memory requirements
    let mem_reqs = device.get_image_memory_requirements(image);

    // Allocate exportable memory
    let mut export_alloc_info = vk::ExportMemoryAllocateInfo::default()
        .handle_types(handle_type);

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
        ext.get_memory_fd(&get_fd_info).expect("Failed to export memory fd") as NativeHandle
    };

    #[cfg(windows)]
    let handle = {
        let ext = ash::khr::external_memory_win32::Device::new(instance, device);
        let get_handle_info = vk::MemoryGetWin32HandleInfoKHR::default()
            .memory(memory)
            .handle_type(vk::ExternalMemoryHandleTypeFlags::OPAQUE_WIN32);
        ext.get_memory_win32_handle(&get_handle_info).expect("Failed to export memory handle") as NativeHandle
    };

    eprintln!(
        "[deno_renderer] Exported memory handle={} for {}x{} image (size={})",
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
        unsafe { windows_sys::Win32::Foundation::CloseHandle(img.handle); }
        img.handle = INVALID_HANDLE;
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

    // Default script path: scene.ts next to the executable
    if script_path.is_empty() {
        if let Ok(exe_path) = std::env::current_exe() {
            if let Some(dir) = exe_path.parent() {
                script_path = dir.join("scene.ts").to_string_lossy().to_string();
            }
        }
        // Fallback to current directory
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
    ];

    let instance_ci = vk::InstanceCreateInfo::default()
        .application_info(&app_info)
        .enabled_extension_names(&instance_extensions);

    let instance = unsafe {
        entry
            .create_instance(&instance_ci, None)
            .expect("Failed to create VkInstance")
    };
    eprintln!("[deno_renderer] VkInstance created");

    // -----------------------------------------------------------------------
    // 2. Pick physical device with a graphics queue family
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
    // 3. Create logical device
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
    // 5. Load memory export function & create shared image
    // -----------------------------------------------------------------------
    let image_format = vk::Format::R8G8B8A8_UNORM;
    let initial_width: u32 = 640;
    let initial_height: u32 = 480;

    let mut shared_img = unsafe {
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
    // 6. Socket: listen and accept presenter connection
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

    // Send the fd + SharedSurfaceInfo
    let surface_info = SharedSurfaceInfo {
        width: shared_img.width,
        height: shared_img.height,
        format: image_format.as_raw() as u32,
        memory_size: shared_img.memory_size,
        memory_type_bits: shared_img.memory_type_bits,
    };

    let info_bytes = unsafe {
        std::slice::from_raw_parts(
            &surface_info as *const SharedSurfaceInfo as *const u8,
            std::mem::size_of::<SharedSurfaceInfo>(),
        )
    };

    transport
        .send_handle(shared_img.handle, info_bytes)
        .unwrap_or_else(|e| panic!("Failed to send handle: {}", e));
    eprintln!("[deno_renderer] Surface shared, handle sent.");

    // -----------------------------------------------------------------------
    // 7. deno_core integration — run TypeScript scene script
    // -----------------------------------------------------------------------
    let scene_state = Arc::new(Mutex::new(SceneState::default()));

    {
        let scene_state_clone = scene_state.clone();

        deno_core::extension!(
            scene_ext,
            ops = [op_set_clear_color, op_set_rotation_speed, op_log]
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

            // Put the scene state into the OpState so ops can access it
            runtime.op_state().borrow_mut().put(scene_state_clone);

            // Try to load and run the script
            match std::fs::read_to_string(&script_path) {
                Ok(code) => {
                    eprintln!("[deno_renderer] Running script: {}", script_path);
                    if let Err(e) = runtime.execute_script("<scene>", code) {
                        eprintln!("[deno_renderer] Script error: {}", e);
                    }
                    // Run the event loop to completion in case script has async work
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
        });
    }

    // -----------------------------------------------------------------------
    // 8. Allocate command buffer and fence for render loop
    // -----------------------------------------------------------------------
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

    // -----------------------------------------------------------------------
    // 9. Render loop
    // -----------------------------------------------------------------------
    eprintln!("[deno_renderer] Entering render loop (Ctrl+C to exit)...");

    while running.load(Ordering::Relaxed) {
        // Poll for input events from presenter (non-blocking)
        let mut event_buf = [0u8; std::mem::size_of::<InputEvent>()];
        while let Some(n) = transport.recv_data_non_blocking(&mut event_buf) {
            if n == std::mem::size_of::<InputEvent>() {
                let event: InputEvent =
                    unsafe { std::ptr::read(event_buf.as_ptr() as *const InputEvent) };

                if event.event_type == INPUT_EVENT_RESIZE {
                    let resize: ResizeData =
                        unsafe { std::ptr::read(event.data.as_ptr() as *const ResizeData) };
                    let new_w = resize.width;
                    let new_h = resize.height;
                    if new_w > 0
                        && new_h > 0
                        && (new_w != shared_img.width || new_h != shared_img.height)
                    {
                        unsafe {
                            device.device_wait_idle().expect("device_wait_idle failed");
                            destroy_shared_image(&device, &mut shared_img);
                            shared_img = create_shared_image(
                                &instance,
                                &device,
                                phys_device,
                                new_w,
                                new_h,
                                image_format,
                            );
                        }

                        // Send new surface info to presenter
                        let si = SharedSurfaceInfo {
                            width: shared_img.width,
                            height: shared_img.height,
                            format: image_format.as_raw() as u32,
                            memory_size: shared_img.memory_size,
                            memory_type_bits: shared_img.memory_type_bits,
                        };
                        let si_bytes = unsafe {
                            std::slice::from_raw_parts(
                                &si as *const SharedSurfaceInfo as *const u8,
                                std::mem::size_of::<SharedSurfaceInfo>(),
                            )
                        };
                        let _ = transport.send_handle(shared_img.handle, si_bytes);
                        eprintln!("[deno_renderer] Resized to {}x{}", new_w, new_h);
                    }
                }
                // Other event types: ignore for now (no geometry to rotate)
            }
        }

        // Get current scene state
        let state = scene_state.lock().unwrap().clone();

        unsafe {
            // Reset command buffer
            device
                .reset_command_buffer(cmd_buf, vk::CommandBufferResetFlags::empty())
                .expect("Failed to reset command buffer");

            // Begin command buffer
            let begin_info = vk::CommandBufferBeginInfo::default()
                .flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT);

            device
                .begin_command_buffer(cmd_buf, &begin_info)
                .expect("Failed to begin command buffer");

            // Transition image: UNDEFINED -> TRANSFER_DST_OPTIMAL
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

            // Clear with scene color
            let clear_color = vk::ClearColorValue {
                float32: state.clear_color,
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
                &clear_color,
                &[range],
            );

            // Transition image: TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL
            // (presenter expects this layout)
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

            device
                .end_command_buffer(cmd_buf)
                .expect("Failed to end command buffer");

            // Submit with fence
            device
                .reset_fences(&[fence])
                .expect("Failed to reset fences");

            let submit_info =
                vk::SubmitInfo::default().command_buffers(std::slice::from_ref(&cmd_buf));

            device
                .queue_submit(graphics_queue, &[submit_info], fence)
                .expect("Failed to submit queue");

            device
                .wait_for_fences(&[fence], true, u64::MAX)
                .expect("Failed to wait for fence");
        }

        // Target ~60fps
        std::thread::sleep(Duration::from_millis(16));
    }

    eprintln!("\n[deno_renderer] Shutting down...");

    // -----------------------------------------------------------------------
    // 10. Cleanup
    // -----------------------------------------------------------------------
    unsafe {
        device.device_wait_idle().expect("device_wait_idle failed");
    }

    transport.close();

    unsafe {
        device.destroy_fence(fence, None);
        device.free_command_buffers(command_pool, &[cmd_buf]);
        destroy_shared_image(&device, &mut shared_img);
        device.destroy_command_pool(command_pool, None);
        device.destroy_device(None);
        instance.destroy_instance(None);
    }

    eprintln!("[deno_renderer] Cleanup complete");
}

// ---------------------------------------------------------------------------
// Simple ctrlc handler using signal
// ---------------------------------------------------------------------------
#[cfg(unix)]
fn ctrlc_simple(running: Arc<AtomicBool>) {
    unsafe {
        // Store in a static so the signal handler can access it
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
            1 // TRUE, handled
        }

        SetConsoleCtrlHandler(Some(handler), 1); // TRUE
    }
}
