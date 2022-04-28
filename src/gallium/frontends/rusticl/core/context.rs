extern crate mesa_rust;
extern crate rusticl_opencl_gen;

use crate::api::icd::*;
use crate::core::device::*;
use crate::impl_cl_type_trait;

use self::mesa_rust::pipe::resource::*;
use self::rusticl_opencl_gen::*;

use std::collections::HashMap;
use std::convert::TryInto;
use std::os::raw::c_void;
use std::sync::Arc;

pub struct Context {
    pub base: CLObjectBase<CL_INVALID_CONTEXT>,
    pub devs: Vec<Arc<Device>>,
    pub properties: Vec<cl_context_properties>,
}

impl_cl_type_trait!(cl_context, Context, CL_INVALID_CONTEXT);

impl Context {
    pub fn new(devs: Vec<Arc<Device>>, properties: Vec<cl_context_properties>) -> Arc<Context> {
        Arc::new(Self {
            base: CLObjectBase::new(),
            devs: devs,
            properties: properties,
        })
    }

    pub fn create_buffer(&self, size: usize) -> CLResult<HashMap<Arc<Device>, Arc<PipeResource>>> {
        let adj_size: u32 = size.try_into().map_err(|_| CL_OUT_OF_HOST_MEMORY)?;
        let mut res = HashMap::new();
        for dev in &self.devs {
            let resource = dev
                .screen()
                .resource_create_buffer(adj_size)
                .ok_or(CL_OUT_OF_RESOURCES);
            res.insert(Arc::clone(dev), Arc::new(resource?));
        }
        Ok(res)
    }

    pub fn create_buffer_from_user(
        &self,
        size: usize,
        user_ptr: *mut c_void,
    ) -> CLResult<HashMap<Arc<Device>, Arc<PipeResource>>> {
        let adj_size: u32 = size.try_into().map_err(|_| CL_OUT_OF_HOST_MEMORY)?;
        let mut res = HashMap::new();
        for dev in &self.devs {
            let resource = dev
                .screen()
                .resource_create_buffer_from_user(adj_size, user_ptr)
                .ok_or(CL_OUT_OF_RESOURCES);
            res.insert(Arc::clone(dev), Arc::new(resource?));
        }
        Ok(res)
    }
}
