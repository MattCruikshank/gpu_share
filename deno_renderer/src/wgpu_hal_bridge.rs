// wgpu_hal_bridge.rs — Wraps our raw ash Vulkan objects as wgpu-core objects
// so that deno_webgpu can use them as a standard WebGPU device.

use std::ffi::CStr;
use std::sync::Arc;

use ash::vk;
use wgpu_core::id::{AdapterId, DeviceId, QueueId, TextureId};
use wgpu_types as wgt;

/// The result of bridging our ash Vulkan objects into wgpu-core.
/// Contains everything needed to inject into deno_webgpu's OpState.
pub struct BridgeState {
    pub global: Arc<wgpu_core::global::Global>,
    pub adapter_id: AdapterId,
    pub device_id: DeviceId,
    pub queue_id: QueueId,
}

/// Create a wgpu-core Global + Adapter + Device from our existing ash objects.
///
/// # Safety
/// - `entry`, `instance`, `phys_device`, `device` must be valid Vulkan handles
/// - `device` must have been created from `phys_device` with the listed extensions
/// - `queue_family_index` / `queue_index` must match the queue the device was created with
pub unsafe fn create_bridge(
    entry: ash::Entry,
    instance: ash::Instance,
    phys_device: vk::PhysicalDevice,
    device: ash::Device,
    queue_family_index: u32,
    queue_index: u32,
    instance_extensions: &[&'static CStr],
    device_extensions: &[&'static CStr],
) -> Result<BridgeState, String> {
    // -----------------------------------------------------------------------
    // 1. Wrap ash Instance → wgpu_hal::vulkan::Instance
    //    We pass a drop_callback so wgpu-hal does NOT destroy our instance.
    // -----------------------------------------------------------------------
    let instance_clone = instance.clone();
    let hal_instance = unsafe {
        wgpu_hal::vulkan::Instance::from_raw(
            entry.clone(),
            instance.clone(),
            vk::API_VERSION_1_2,
            0, // android_sdk_version
            None, // debug_utils_create_info — we manage debug utils ourselves
            instance_extensions.to_vec(),
            wgt::InstanceFlags::empty(),
            wgt::MemoryBudgetThresholds::default(),
            false, // has_nv_optimus
            Some(Box::new(move || {
                // Drop guard: prevent wgpu-hal from destroying our instance.
                // The ash::Instance is Clone (it's an Arc internally), so this
                // closure just holds a reference that keeps it alive. We destroy
                // the instance ourselves in main.rs cleanup.
                let _ = &instance_clone;
            })),
        )
    }
    .map_err(|e| format!("Failed to wrap ash Instance for wgpu-hal: {e}"))?;

    // -----------------------------------------------------------------------
    // 2. Expose adapter from our physical device
    // -----------------------------------------------------------------------
    let hal_exposed_adapter = hal_instance
        .expose_adapter(phys_device)
        .ok_or_else(|| "wgpu-hal failed to expose adapter from physical device".to_string())?;

    // Use minimal features — only what our ash Device actually has enabled.
    // Passing all adapter features would make wgpu-hal try to use extensions
    // (buffer_device_address, subgroup_size_control, etc.) we didn't enable.
    let device_features = wgt::Features::empty();

    // -----------------------------------------------------------------------
    // 3. Wrap ash Device → wgpu_hal OpenDevice
    //    Again, drop_callback prevents wgpu-hal from destroying our device.
    // -----------------------------------------------------------------------
    let device_clone = device.clone();
    let hal_open_device = unsafe {
        hal_exposed_adapter.adapter.device_from_raw(
            device.clone(),
            Some(Box::new(move || {
                let _ = &device_clone;
            })),
            device_extensions,
            device_features,
            &wgt::MemoryHints::Performance,
            queue_family_index,
            queue_index,
        )
    }
    .map_err(|e| format!("Failed to wrap ash Device for wgpu-hal: {e}"))?;

    // -----------------------------------------------------------------------
    // 4. Lift to wgpu-core: Global → Adapter → Device
    // -----------------------------------------------------------------------
    let global = unsafe {
        wgpu_core::global::Global::from_hal_instance::<wgpu_hal::api::Vulkan>(
            "gpu-share",
            hal_instance,
        )
    };

    let adapter_id = unsafe {
        global.create_adapter_from_hal(hal_exposed_adapter.into(), None)
    };

    let device_desc = wgpu_core::device::DeviceDescriptor {
        label: None,
        required_features: device_features,
        required_limits: wgt::Limits::default(),
        experimental_features: wgt::ExperimentalFeatures::default(),
        memory_hints: wgt::MemoryHints::Performance,
        trace: wgt::Trace::Off,
    };

    let (device_id, queue_id) = unsafe {
        global.create_device_from_hal(
            adapter_id,
            hal_open_device.into(),
            &device_desc,
            None,
            None,
        )
    }
    .map_err(|e| format!("Failed to create wgpu-core device from HAL: {e}"))?;

    eprintln!("[wgpu_hal_bridge] Bridge created: adapter={adapter_id:?}, device={device_id:?}, queue={queue_id:?}");

    Ok(BridgeState {
        global: Arc::new(global),
        adapter_id,
        device_id,
        queue_id,
    })
}

/// Wrap an existing VkImage as a wgpu-core Texture.
///
/// Call this after creating (or recreating) the shared image. The returned
/// TextureId can be used to create GPUTextureView objects in JS.
///
/// # Safety
/// - `vk_image` must be a valid VkImage created from the same device
/// - The image must remain valid for as long as the TextureId is in use
pub unsafe fn import_texture(
    global: &wgpu_core::global::Global,
    device_id: DeviceId,
    vk_image: vk::Image,
    width: u32,
    height: u32,
    format: wgt::TextureFormat,
) -> Result<TextureId, String> {
    // Access the HAL device to call texture_from_raw
    let hal_texture = unsafe {
        // Get the raw hal device through the global
        let hal_device = global
            .device_as_hal::<wgpu_hal::api::Vulkan>(device_id)
            .ok_or_else(|| "Failed to get HAL device".to_string())?;

        hal_device.texture_from_raw(
            vk_image,
            &wgpu_hal::TextureDescriptor {
                label: Some("shared_surface"),
                size: wgt::Extent3d {
                    width,
                    height,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgt::TextureDimension::D2,
                format,
                usage: wgt::TextureUses::COLOR_TARGET
                    | wgt::TextureUses::COPY_SRC
                    | wgt::TextureUses::COPY_DST
                    | wgt::TextureUses::RESOURCE,
                memory_flags: wgpu_hal::MemoryFlags::empty(),
                view_formats: vec![],
            },
            None, // no drop callback — we manage the VkImage lifetime ourselves
            wgpu_hal::vulkan::TextureMemory::External,
        )
    };

    let wgpu_desc = wgpu_core::resource::TextureDescriptor {
        label: Some(std::borrow::Cow::Borrowed("shared_surface")),
        size: wgt::Extent3d {
            width,
            height,
            depth_or_array_layers: 1,
        },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgt::TextureDimension::D2,
        format,
        usage: wgt::TextureUsages::RENDER_ATTACHMENT
            | wgt::TextureUsages::COPY_SRC
            | wgt::TextureUsages::COPY_DST
            | wgt::TextureUsages::TEXTURE_BINDING,
        view_formats: vec![],
    };

    let (texture_id, maybe_err) = unsafe {
        global.create_texture_from_hal(Box::new(hal_texture), device_id, &wgpu_desc, None)
    };

    if let Some(err) = maybe_err {
        return Err(format!("Failed to create wgpu-core texture from HAL: {err}"));
    }

    eprintln!("[wgpu_hal_bridge] Imported shared texture: {texture_id:?} ({width}x{height})");
    Ok(texture_id)
}
