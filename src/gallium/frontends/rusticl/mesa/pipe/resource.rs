extern crate mesa_rust_gen;

use self::mesa_rust_gen::*;

use std::ptr;

pub struct PipeResource {
    pipe: *mut pipe_resource,
}

impl PipeResource {
    pub fn new(res: *mut pipe_resource) -> Option<Self> {
        if res.is_null() {
            return None;
        }

        Some(Self { pipe: res })
    }

    pub(super) fn pipe(&self) -> *mut pipe_resource {
        self.pipe
    }
}

impl Drop for PipeResource {
    fn drop(&mut self) {
        unsafe { pipe_resource_reference(&mut self.pipe, ptr::null_mut()) }
    }
}
