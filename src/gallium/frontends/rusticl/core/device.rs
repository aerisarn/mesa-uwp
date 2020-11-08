extern crate mesa_rust;
extern crate mesa_rust_gen;
extern crate mesa_rust_util;
extern crate rusticl_opencl_gen;

use crate::api::icd::*;
use crate::api::util::*;
use crate::core::format::*;
use crate::core::util::*;
use crate::core::version::*;
use crate::impl_cl_type_trait;

use self::mesa_rust::pipe::context::*;
use self::mesa_rust::pipe::device::load_screens;
use self::mesa_rust::pipe::screen::*;
use self::mesa_rust_gen::*;
use self::rusticl_opencl_gen::*;

use std::cmp::max;
use std::cmp::min;
use std::collections::HashMap;
use std::convert::TryInto;
use std::env;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::MutexGuard;

pub struct Device {
    pub base: CLObjectBase<CL_INVALID_DEVICE>,
    screen: Arc<PipeScreen>,
    pub cl_version: CLVersion,
    pub clc_version: CLVersion,
    pub clc_versions: Vec<cl_name_version>,
    pub custom: bool,
    pub embedded: bool,
    pub extension_string: String,
    pub extensions: Vec<cl_name_version>,
    pub formats: HashMap<cl_image_format, HashMap<cl_mem_object_type, cl_mem_flags>>,
    helper_ctx: Mutex<Arc<PipeContext>>,
}

impl_cl_type_trait!(cl_device_id, Device, CL_INVALID_DEVICE);

impl Device {
    fn new(screen: Arc<PipeScreen>) -> Option<Arc<Device>> {
        let mut d = Self {
            base: CLObjectBase::new(),
            helper_ctx: Mutex::new(screen.create_context().unwrap()),
            screen: screen,
            cl_version: CLVersion::Cl3_0,
            clc_version: CLVersion::Cl3_0,
            clc_versions: Vec::new(),
            custom: false,
            embedded: false,
            extension_string: String::from(""),
            extensions: Vec::new(),
            formats: HashMap::new(),
        };

        if !d.check_valid() {
            return None;
        }

        d.fill_format_tables();

        // check if we are embedded or full profile first
        d.embedded = d.check_embedded_profile();

        // check if we have to report it as a custom device
        d.custom = d.check_custom();

        // query supported extensions
        d.fill_extensions();

        // now figure out what version we are
        d.check_version();

        Some(Arc::new(d))
    }

    fn fill_format_tables(&mut self) {
        for f in FORMATS {
            let mut fs = HashMap::new();
            for t in CL_IMAGE_TYPES {
                let mut flags: cl_uint = 0;
                if self.screen.is_format_supported(
                    f.pipe,
                    cl_mem_type_to_texture_target(t),
                    PIPE_BIND_SAMPLER_VIEW,
                ) {
                    flags |= CL_MEM_READ_ONLY;
                }
                if self.screen.is_format_supported(
                    f.pipe,
                    cl_mem_type_to_texture_target(t),
                    PIPE_BIND_SHADER_IMAGE,
                ) {
                    flags |= CL_MEM_WRITE_ONLY;
                    // TODO: enable once we support it
                    // flags |= CL_MEM_KERNEL_READ_AND_WRITE;
                }
                if self.screen.is_format_supported(
                    f.pipe,
                    cl_mem_type_to_texture_target(t),
                    PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_SHADER_IMAGE,
                ) {
                    flags |= CL_MEM_READ_WRITE;
                }
                fs.insert(t, flags as cl_mem_flags);
            }
            self.formats.insert(f.cl_image_format, fs);
        }
    }

    fn check_valid(&self) -> bool {
        if self.screen.param(pipe_cap::PIPE_CAP_COMPUTE) == 0 ||
         // even though we use PIPE_SHADER_IR_NIR, PIPE_SHADER_IR_NIR_SERIALIZED marks CL support by the driver
         self.shader_param(pipe_shader_cap::PIPE_SHADER_CAP_SUPPORTED_IRS) & (1 << (pipe_shader_ir::PIPE_SHADER_IR_NIR_SERIALIZED as i32)) == 0
        {
            return false;
        }

        // CL_DEVICE_MAX_PARAMETER_SIZE
        // For this minimum value, only a maximum of 128 arguments can be passed to a kernel
        if self.param_max_size() < 128 {
            return false;
        }
        true
    }

    fn check_custom(&self) -> bool {
        // Max size of memory object allocation in bytes. The minimum value is
        // max(min(1024 × 1024 × 1024, 1/4th of CL_DEVICE_GLOBAL_MEM_SIZE), 32 × 1024 × 1024)
        // for devices that are not of type CL_DEVICE_TYPE_CUSTOM.
        let mut limit = min(1024 * 1024 * 1024, self.global_mem_size());
        limit = max(limit, 32 * 1024 * 1024);
        if self.max_mem_alloc() < limit {
            return true;
        }

        // CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS
        // The minimum value is 3 for devices that are not of type CL_DEVICE_TYPE_CUSTOM.
        if self.max_grid_dimensions() < 3 {
            return true;
        }

        if self.embedded {
            // CL_DEVICE_MAX_PARAMETER_SIZE
            // The minimum value is 256 bytes for devices that are not of type CL_DEVICE_TYPE_CUSTOM.
            if self.param_max_size() < 256 {
                return true;
            }

            // CL_DEVICE_MAX_CONSTANT_BUFFER_SIZE
            // The minimum value is 1 KB for devices that are not of type CL_DEVICE_TYPE_CUSTOM.
            if self.const_max_size() < 1024 {
                return true;
            }

            // TODO
            // CL_DEVICE_MAX_CONSTANT_ARGS
            // The minimum value is 4 for devices that are not of type CL_DEVICE_TYPE_CUSTOM.

            // CL_DEVICE_LOCAL_MEM_SIZE
            // The minimum value is 1 KB for devices that are not of type CL_DEVICE_TYPE_CUSTOM.
            if self.local_mem_size() < 1024 {
                return true;
            }
        } else {
            // CL 1.0 spec:
            // CL_DEVICE_MAX_PARAMETER_SIZE
            // The minimum value is 256 for devices that are not of type CL_DEVICE_TYPE_CUSTOM.
            if self.param_max_size() < 256 {
                return true;
            }

            // CL_DEVICE_MAX_CONSTANT_BUFFER_SIZE
            // The minimum value is 64 KB for devices that are not of type CL_DEVICE_TYPE_CUSTOM.
            if self.const_max_size() < 64 * 1024 {
                return true;
            }

            // TODO
            // CL_DEVICE_MAX_CONSTANT_ARGS
            // The minimum value is 8 for devices that are not of type CL_DEVICE_TYPE_CUSTOM.

            // CL 1.0 spec:
            // CL_DEVICE_LOCAL_MEM_SIZE
            // The minimum value is 16 KB for devices that are not of type CL_DEVICE_TYPE_CUSTOM.
            if self.local_mem_size() < 16 * 1024 {
                return true;
            }
        }

        false
    }

    fn check_embedded_profile(&self) -> bool {
        if self.image_supported() {
            // The minimum value is 16 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            if self.max_samplers() < 16 ||
            // The minimum value is 128 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            self.image_read_count() < 128 ||
            // The minimum value is 64 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            self.image_write_count() < 64 ||
            // The minimum value is 16384 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            self.image_2d_size() < 16384 ||
            // The minimum value is 2048 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            self.image_array_size() < 2048 ||
            // The minimum value is 65536 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            self.image_buffer_size() < 65536
            {
                return true;
            }

            // TODO check req formats
        }
        false
    }

    fn parse_env_version() -> Option<CLVersion> {
        let val = env::var("RUSTICL_CL_VERSION").ok()?;
        let (major, minor) = val.split_once('.')?;
        let major = major.parse().ok()?;
        let minor = minor.parse().ok()?;
        mk_cl_version(major, minor, 0).try_into().ok()
    }

    // TODO add CLC checks
    fn check_version(&mut self) {
        let exts: Vec<&str> = self.extension_string.split(' ').collect();
        let mut res = CLVersion::Cl3_0;

        if self.embedded {
            if self.image_supported() {
                let supports_array_writes = !FORMATS
                    .iter()
                    .filter(|f| f.req_for_embeded_read_or_write)
                    .map(|f| self.formats.get(&f.cl_image_format).unwrap())
                    .map(|f| f.get(&CL_MEM_OBJECT_IMAGE2D_ARRAY).unwrap())
                    .any(|f| *f & cl_mem_flags::from(CL_MEM_WRITE_ONLY) == 0);
                if self.image_3d_size() < 2048 || !supports_array_writes {
                    res = CLVersion::Cl1_2;
                }
            }
        }

        // TODO: check image 1D, 1Dbuffer, 1Darray and 2Darray support explicitly
        if self.image_supported() {
            // The minimum value is 256 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            if self.image_array_size() < 256 ||
            // The minimum value is 2048 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            self.image_buffer_size() < 2048
            {
                res = CLVersion::Cl1_1;
            }
        }

        if !exts.contains(&"cl_khr_byte_addressable_store")
         || !exts.contains(&"cl_khr_global_int32_base_atomics")
         || !exts.contains(&"cl_khr_global_int32_extended_atomics")
         || !exts.contains(&"cl_khr_local_int32_base_atomics")
         || !exts.contains(&"cl_khr_local_int32_extended_atomics")
         // The following modifications are made to the OpenCL 1.1 platform layer and runtime (sections 4 and 5):
         // The minimum FULL_PROFILE value for CL_DEVICE_MAX_PARAMETER_SIZE increased from 256 to 1024 bytes
         || self.param_max_size() < 1024
         // The minimum FULL_PROFILE value for CL_DEVICE_LOCAL_MEM_SIZE increased from 16 KB to 32 KB.
         || self.local_mem_size() < 32 * 1024
        {
            res = CLVersion::Cl1_0;
        }

        if let Some(val) = Self::parse_env_version() {
            res = val;
        }

        if res >= CLVersion::Cl3_0 {
            self.clc_versions
                .push(mk_cl_version_ext(3, 0, 0, "OpenCL C"));
        }

        if res >= CLVersion::Cl1_2 {
            self.clc_versions
                .push(mk_cl_version_ext(1, 2, 0, "OpenCL C"));
        }

        if res >= CLVersion::Cl1_1 {
            self.clc_versions
                .push(mk_cl_version_ext(1, 1, 0, "OpenCL C"));
        }

        if res >= CLVersion::Cl1_0 {
            self.clc_versions
                .push(mk_cl_version_ext(1, 0, 0, "OpenCL C"));
        }

        self.cl_version = res;
        self.clc_version = min(CLVersion::Cl1_2, res);
    }

    fn fill_extensions(&mut self) {
        let mut exts_str: Vec<String> = Vec::new();
        let mut exts = Vec::new();
        let mut add_ext = |major, minor, patch, ext| {
            exts.push(mk_cl_version_ext(major, minor, patch, ext));
            exts_str.push(ext.to_owned());
        };

        add_ext(1, 0, 0, "cl_khr_byte_addressable_store");

        self.extensions = exts;
        self.extension_string = exts_str.join(" ");
    }

    fn shader_param(&self, cap: pipe_shader_cap) -> i32 {
        self.screen
            .shader_param(pipe_shader_type::PIPE_SHADER_COMPUTE, cap)
    }

    pub fn all() -> Vec<Arc<Device>> {
        load_screens().into_iter().filter_map(Device::new).collect()
    }

    pub fn address_bits(&self) -> cl_uint {
        self.screen
            .compute_param(pipe_compute_cap::PIPE_COMPUTE_CAP_ADDRESS_BITS)
    }

    pub fn const_max_size(&self) -> cl_ulong {
        self.screen
            .param(pipe_cap::PIPE_CAP_MAX_SHADER_BUFFER_SIZE_UINT) as u64
    }

    pub fn device_type(&self) -> cl_device_type {
        if self.custom {
            return CL_DEVICE_TYPE_CUSTOM as cl_device_type;
        }
        (match self.screen.device_type() {
            pipe_loader_device_type::PIPE_LOADER_DEVICE_SOFTWARE => CL_DEVICE_TYPE_CPU,
            pipe_loader_device_type::PIPE_LOADER_DEVICE_PCI => {
                CL_DEVICE_TYPE_GPU | CL_DEVICE_TYPE_DEFAULT
            }
            pipe_loader_device_type::PIPE_LOADER_DEVICE_PLATFORM => {
                CL_DEVICE_TYPE_GPU | CL_DEVICE_TYPE_DEFAULT
            }
            pipe_loader_device_type::NUM_PIPE_LOADER_DEVICE_TYPES => CL_DEVICE_TYPE_CUSTOM,
        }) as cl_device_type
    }

    pub fn global_mem_size(&self) -> cl_ulong {
        self.screen
            .compute_param(pipe_compute_cap::PIPE_COMPUTE_CAP_MAX_GLOBAL_SIZE)
    }

    pub fn image_2d_size(&self) -> usize {
        self.screen.param(pipe_cap::PIPE_CAP_MAX_TEXTURE_2D_SIZE) as usize
    }

    pub fn image_3d_size(&self) -> usize {
        1 << (self.screen.param(pipe_cap::PIPE_CAP_MAX_TEXTURE_3D_LEVELS) - 1)
    }

    pub fn image_3d_supported(&self) -> bool {
        self.screen.param(pipe_cap::PIPE_CAP_MAX_TEXTURE_3D_LEVELS) != 0
    }

    pub fn image_array_size(&self) -> usize {
        self.screen
            .param(pipe_cap::PIPE_CAP_MAX_TEXTURE_ARRAY_LAYERS) as usize
    }

    pub fn image_base_address_alignment(&self) -> cl_uint {
        0
    }

    pub fn image_buffer_size(&self) -> usize {
        self.screen
            .param(pipe_cap::PIPE_CAP_MAX_TEXEL_BUFFER_ELEMENTS_UINT) as usize
    }

    pub fn image_read_count(&self) -> cl_uint {
        self.shader_param(pipe_shader_cap::PIPE_SHADER_CAP_MAX_SAMPLER_VIEWS) as cl_uint
    }

    pub fn image_supported(&self) -> bool {
        // TODO check CL_DEVICE_IMAGE_SUPPORT reqs
        self.shader_param(pipe_shader_cap::PIPE_SHADER_CAP_MAX_SHADER_IMAGES) != 0 &&
      // The minimum value is 8 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
      self.image_read_count() >= 8 &&
      // The minimum value is 8 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
      self.image_write_count() >= 8 &&
      // The minimum value is 2048 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
      self.image_2d_size() >= 2048
    }

    pub fn image_write_count(&self) -> cl_uint {
        self.shader_param(pipe_shader_cap::PIPE_SHADER_CAP_MAX_SHADER_IMAGES) as cl_uint
    }

    pub fn little_endian(&self) -> bool {
        let endianness = self.screen.param(pipe_cap::PIPE_CAP_ENDIANNESS);
        endianness == (pipe_endian::PIPE_ENDIAN_LITTLE as i32)
    }

    pub fn local_mem_size(&self) -> cl_ulong {
        self.screen
            .compute_param(pipe_compute_cap::PIPE_COMPUTE_CAP_MAX_LOCAL_SIZE)
    }

    pub fn max_block_sizes(&self) -> Vec<usize> {
        let v: Vec<u64> = self
            .screen
            .compute_param(pipe_compute_cap::PIPE_COMPUTE_CAP_MAX_BLOCK_SIZE);
        v.into_iter().map(|v| v as usize).collect()
    }

    pub fn max_clock_freq(&self) -> cl_uint {
        self.screen
            .compute_param(pipe_compute_cap::PIPE_COMPUTE_CAP_MAX_CLOCK_FREQUENCY)
    }

    pub fn max_compute_units(&self) -> cl_uint {
        self.screen
            .compute_param(pipe_compute_cap::PIPE_COMPUTE_CAP_MAX_COMPUTE_UNITS)
    }

    pub fn max_grid_dimensions(&self) -> cl_uint {
        ComputeParam::<u64>::compute_param(
            self.screen.as_ref(),
            pipe_compute_cap::PIPE_COMPUTE_CAP_GRID_DIMENSION,
        ) as cl_uint
    }

    pub fn max_mem_alloc(&self) -> cl_ulong {
        self.screen
            .compute_param(pipe_compute_cap::PIPE_COMPUTE_CAP_MAX_MEM_ALLOC_SIZE)
    }

    pub fn max_samplers(&self) -> cl_uint {
        self.shader_param(pipe_shader_cap::PIPE_SHADER_CAP_MAX_TEXTURE_SAMPLERS) as cl_uint
    }

    pub fn max_threads_per_block(&self) -> usize {
        ComputeParam::<u64>::compute_param(
            self.screen.as_ref(),
            pipe_compute_cap::PIPE_COMPUTE_CAP_MAX_THREADS_PER_BLOCK,
        ) as usize
    }

    pub fn param_max_size(&self) -> usize {
        ComputeParam::<u64>::compute_param(
            self.screen.as_ref(),
            pipe_compute_cap::PIPE_COMPUTE_CAP_MAX_INPUT_SIZE,
        ) as usize
    }

    pub fn screen(&self) -> &Arc<PipeScreen> {
        &self.screen
    }

    pub fn unified_memory(&self) -> bool {
        self.screen.param(pipe_cap::PIPE_CAP_UMA) == 1
    }

    pub fn vendor_id(&self) -> cl_uint {
        let id = self.screen.param(pipe_cap::PIPE_CAP_VENDOR_ID);
        if id == -1 {
            return 0;
        }
        id as u32
    }

    pub fn helper_ctx(&self) -> MutexGuard<Arc<PipeContext>> {
        self.helper_ctx.lock().unwrap()
    }

    pub fn cl_features(&self) -> clc_optional_features {
        clc_optional_features {
            fp16: false,
            fp64: false,
            int64: false,
            images: self.image_supported(),
            images_read_write: false,
            images_write_3d: false,
            intel_subgroups: false,
            subgroups: false,
        }
    }
}
