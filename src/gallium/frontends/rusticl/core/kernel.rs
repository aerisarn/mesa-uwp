extern crate mesa_rust;
extern crate rusticl_opencl_gen;

use crate::api::icd::*;
use crate::impl_cl_type_trait;

use self::rusticl_opencl_gen::*;

use std::sync::Arc;

#[repr(C)]
pub struct Kernel {
    pub base: CLObjectBase<CL_INVALID_KERNEL>,
}

impl_cl_type_trait!(cl_kernel, Kernel, CL_INVALID_KERNEL);

impl Kernel {
    pub fn new() -> Arc<Kernel> {
        Arc::new(Self {
            base: CLObjectBase::new(),
        })
    }
}
