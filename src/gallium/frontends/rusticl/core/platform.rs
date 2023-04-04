use crate::api::icd::CLResult;
use crate::api::icd::DISPATCH;
use crate::core::version::*;

use rusticl_opencl_gen::*;

#[repr(C)]
pub struct Platform {
    dispatch: &'static cl_icd_dispatch,
    pub extensions: [cl_name_version; 2],
}

static PLATFORM: Platform = Platform {
    dispatch: &DISPATCH,
    extensions: [
        mk_cl_version_ext(1, 0, 0, "cl_khr_icd"),
        mk_cl_version_ext(1, 0, 0, "cl_khr_il_program"),
    ],
};

impl Platform {
    pub fn as_ptr(&self) -> cl_platform_id {
        (self as *const Self) as cl_platform_id
    }

    pub fn get() -> &'static Self {
        &PLATFORM
    }
}

pub trait GetPlatformRef {
    fn get_ref(&self) -> CLResult<&'static Platform>;
}

impl GetPlatformRef for cl_platform_id {
    fn get_ref(&self) -> CLResult<&'static Platform> {
        if !self.is_null() && *self == Platform::get().as_ptr() {
            Ok(Platform::get())
        } else {
            Err(CL_INVALID_PLATFORM)
        }
    }
}
