extern crate mesa_rust_gen;
extern crate mesa_rust_util;

use self::mesa_rust_gen::*;
use self::mesa_rust_util::string::*;

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
        features: clc_optional_features,
    ) -> (Option<Self>, String) {
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
            Some(SPIRVBin {
                spirv: out,
                info: None,
            })
        } else {
            None
        };
        (res, msgs.join("\n"))
    }

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

    pub fn kernels(&self) -> Vec<String> {
        unsafe {
            match self.info {
                None => Vec::new(),
                Some(info) => slice::from_raw_parts(info.kernels, info.num_kernels as usize)
                    .iter()
                    .map(|i| i.name)
                    .map(c_string_to_string)
                    .collect(),
            }
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
