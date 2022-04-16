use crate::compiler::nir::*;
use crate::pipe::screen::*;
use crate::util::disk_cache::*;

use mesa_rust_gen::*;
use mesa_rust_util::string::*;

use std::ffi::CString;
use std::os::raw::c_char;
use std::os::raw::c_void;
use std::ptr;
use std::slice;

const INPUT_STR: *const c_char = b"input.cl\0" as *const u8 as *const c_char;

pub struct SPIRVBin {
    spirv: clc_binary,
    info: Option<clc_parsed_spirv>,
}

#[derive(PartialEq, Eq, Hash, Clone)]
pub struct SPIRVKernelArg {
    pub name: String,
    pub type_name: String,
    pub access_qualifier: clc_kernel_arg_access_qualifier,
    pub address_qualifier: clc_kernel_arg_address_qualifier,
    pub type_qualifier: clc_kernel_arg_type_qualifier,
}

pub struct CLCHeader<'a> {
    pub name: CString,
    pub source: &'a CString,
}

unsafe extern "C" fn msg_callback(data: *mut std::ffi::c_void, msg: *const c_char) {
    let msgs = (data as *mut Vec<String>).as_mut().expect("");
    msgs.push(c_string_to_string(msg));
}

impl SPIRVBin {
    pub fn from_clc(
        source: &CString,
        args: &[CString],
        headers: &[CLCHeader],
        cache: &Option<DiskCache>,
        features: clc_optional_features,
    ) -> (Option<Self>, String) {
        let mut hash_key = None;
        let has_includes = args.iter().any(|a| a.as_bytes()[0..2] == *b"-I");

        if let Some(cache) = cache {
            if !has_includes {
                let mut key = Vec::new();

                key.extend_from_slice(source.as_bytes());
                args.iter()
                    .for_each(|a| key.extend_from_slice(a.as_bytes()));
                headers.iter().for_each(|h| {
                    key.extend_from_slice(h.name.as_bytes());
                    key.extend_from_slice(h.source.as_bytes());
                });

                let mut key = cache.gen_key(&key);
                if let Some(data) = cache.get(&mut key) {
                    return (Some(Self::from_bin(&data, false)), String::from(""));
                }

                hash_key = Some(key);
            }
        }

        let c_headers: Vec<_> = headers
            .iter()
            .map(|h| clc_named_value {
                name: h.name.as_ptr(),
                value: h.source.as_ptr(),
            })
            .collect();

        let c_args: Vec<_> = args.iter().map(|a| a.as_ptr()).collect();

        let args = clc_compile_args {
            headers: c_headers.as_ptr(),
            num_headers: c_headers.len() as u32,
            source: clc_named_value {
                name: INPUT_STR,
                value: source.as_ptr(),
            },
            args: c_args.as_ptr(),
            num_args: c_args.len() as u32,
            spirv_version: clc_spirv_version::CLC_SPIRV_VERSION_MAX,
            features: features,
            allowed_spirv_extensions: ptr::null(),
        };
        let mut msgs: Vec<String> = Vec::new();
        let logger = clc_logger {
            priv_: &mut msgs as *mut Vec<String> as *mut c_void,
            error: Some(msg_callback),
            warning: Some(msg_callback),
        };
        let mut out = clc_binary::default();

        let res = unsafe { clc_compile_c_to_spirv(&args, &logger, &mut out) };

        let res = if res {
            let spirv = SPIRVBin {
                spirv: out,
                info: None,
            };

            // add cache entry
            if !has_includes {
                if let Some(mut key) = hash_key {
                    cache.as_ref().unwrap().put(spirv.to_bin(), &mut key);
                }
            }

            Some(spirv)
        } else {
            None
        };

        (res, msgs.join("\n"))
    }

    // TODO cache linking, parsing is around 25% of link time
    pub fn link(spirvs: &[&SPIRVBin], library: bool) -> (Option<Self>, String) {
        let bins: Vec<_> = spirvs.iter().map(|s| &s.spirv as *const _).collect();

        let linker_args = clc_linker_args {
            in_objs: bins.as_ptr(),
            num_in_objs: bins.len() as u32,
            create_library: library as u32,
        };

        let mut msgs: Vec<String> = Vec::new();
        let logger = clc_logger {
            priv_: &mut msgs as *mut Vec<String> as *mut c_void,
            error: Some(msg_callback),
            warning: Some(msg_callback),
        };

        let mut out = clc_binary::default();
        let res = unsafe { clc_link_spirv(&linker_args, &logger, &mut out) };

        let info;
        if !library {
            let mut pspirv = clc_parsed_spirv::default();
            let res = unsafe { clc_parse_spirv(&out, &logger, &mut pspirv) };

            if res {
                info = Some(pspirv);
            } else {
                info = None;
            }
        } else {
            info = None;
        }

        let res = if res {
            Some(SPIRVBin {
                spirv: out,
                info: info,
            })
        } else {
            None
        };
        (res, msgs.join("\n"))
    }

    fn kernel_infos(&self) -> &[clc_kernel_info] {
        match self.info {
            None => &[],
            Some(info) => unsafe { slice::from_raw_parts(info.kernels, info.num_kernels as usize) },
        }
    }

    fn kernel_info(&self, name: &str) -> Option<&clc_kernel_info> {
        self.kernel_infos()
            .iter()
            .find(|i| c_string_to_string(i.name) == name)
    }

    pub fn kernels(&self) -> Vec<String> {
        self.kernel_infos()
            .iter()
            .map(|i| i.name)
            .map(c_string_to_string)
            .collect()
    }

    pub fn args(&self, name: &str) -> Vec<SPIRVKernelArg> {
        match self.kernel_info(name) {
            None => Vec::new(),
            Some(info) => unsafe { slice::from_raw_parts(info.args, info.num_args) }
                .iter()
                .map(|a| SPIRVKernelArg {
                    name: c_string_to_string(a.name),
                    type_name: c_string_to_string(a.type_name),
                    access_qualifier: clc_kernel_arg_access_qualifier(a.access_qualifier),
                    address_qualifier: a.address_qualifier,
                    type_qualifier: clc_kernel_arg_type_qualifier(a.type_qualifier),
                })
                .collect(),
        }
    }

    fn get_spirv_options(library: bool, clc_shader: *const nir_shader) -> spirv_to_nir_options {
        spirv_to_nir_options {
            create_library: library,
            environment: nir_spirv_execution_environment::NIR_SPIRV_OPENCL,
            clc_shader: clc_shader,
            float_controls_execution_mode: float_controls::FLOAT_CONTROLS_DENORM_FLUSH_TO_ZERO_FP32
                as u16,

            caps: spirv_supported_capabilities {
                address: true,
                float64: true,
                generic_pointers: true,
                int8: true,
                int16: true,
                int64: true,
                kernel: true,
                kernel_image: true,
                linkage: true,
                literal_sampler: true,
                printf: true,
                ..Default::default()
            },

            constant_addr_format: nir_address_format::nir_address_format_64bit_global,
            global_addr_format: nir_address_format::nir_address_format_64bit_global, // TODO 32 bit devices
            shared_addr_format: nir_address_format::nir_address_format_32bit_offset_as_64bit,
            temp_addr_format: nir_address_format::nir_address_format_32bit_offset_as_64bit,

            // default
            debug: spirv_to_nir_options__bindgen_ty_1::default(),
            ..Default::default()
        }
    }

    pub fn to_nir(
        &self,
        entry_point: &str,
        nir_options: *const nir_shader_compiler_options,
        libclc: &NirShader,
    ) -> Option<NirShader> {
        let c_entry = CString::new(entry_point.as_bytes()).unwrap();
        let spirv_options = Self::get_spirv_options(false, libclc.get_nir());
        let nir = unsafe {
            spirv_to_nir(
                self.spirv.data.cast(),
                self.spirv.size / 4,
                ptr::null_mut(), // spec
                0,               // spec count
                gl_shader_stage::MESA_SHADER_KERNEL,
                c_entry.as_ptr(),
                &spirv_options,
                nir_options,
            )
        };

        NirShader::new(nir)
    }

    pub fn get_lib_clc(screen: &PipeScreen) -> Option<NirShader> {
        let nir_options = screen.nir_shader_compiler_options(pipe_shader_type::PIPE_SHADER_COMPUTE);
        let spirv_options = Self::get_spirv_options(true, ptr::null());
        let shader_cache = DiskCacheBorrowed::as_ptr(&screen.shader_cache());
        NirShader::new(unsafe {
            nir_load_libclc_shader(64, shader_cache, &spirv_options, nir_options)
        })
    }

    pub fn to_bin(&self) -> &[u8] {
        unsafe { slice::from_raw_parts(self.spirv.data.cast(), self.spirv.size) }
    }

    pub fn from_bin(bin: &[u8], executable: bool) -> Self {
        unsafe {
            let ptr = malloc(bin.len());
            ptr::copy_nonoverlapping(bin.as_ptr(), ptr.cast(), bin.len());
            let spirv = clc_binary {
                data: ptr,
                size: bin.len(),
            };
            let info = if executable {
                let mut pspirv = clc_parsed_spirv::default();
                clc_parse_spirv(&spirv, ptr::null(), &mut pspirv);
                Some(pspirv)
            } else {
                None
            };

            SPIRVBin {
                spirv: spirv,
                info: info,
            }
        }
    }

    pub fn print(&self) {
        unsafe {
            clc_dump_spirv(&self.spirv, stderr);
        }
    }
}

impl Drop for SPIRVBin {
    fn drop(&mut self) {
        unsafe {
            clc_free_spirv(&mut self.spirv);
            if let Some(info) = &mut self.info {
                clc_free_parsed_spirv(info);
            }
        }
    }
}
