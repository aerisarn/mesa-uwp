use crate::api::icd::CLResult;
use crate::api::icd::DISPATCH;
use crate::core::version::*;

use rusticl_opencl_gen::*;

#[repr(C)]
#[allow(non_camel_case_types)]
pub struct _cl_platform_id {
    dispatch: &'static cl_icd_dispatch,
    pub extensions: [cl_name_version; 2],
}

static PLATFORM: _cl_platform_id = _cl_platform_id {
    dispatch: &DISPATCH,
    extensions: [
        mk_cl_version_ext(1, 0, 0, "cl_khr_icd"),
        mk_cl_version_ext(1, 0, 0, "cl_khr_il_program"),
    ],
};

pub fn get_platform() -> cl_platform_id {
    &PLATFORM as *const crate::core::platform::_cl_platform_id
        as *mut ::rusticl_opencl_gen::_cl_platform_id
}

pub trait GetPlatformRef {
    fn get_ref(&self) -> CLResult<&'static _cl_platform_id>;
}

impl GetPlatformRef for cl_platform_id {
    fn get_ref(&self) -> CLResult<&'static _cl_platform_id> {
        if !self.is_null() && *self == get_platform() {
            Ok(&PLATFORM)
        } else {
            Err(CL_INVALID_PLATFORM)
        }
    }
}
