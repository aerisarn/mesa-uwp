extern crate mesa_rust_gen;

use crate::pipe::resource::*;
use crate::pipe::transfer::*;

use self::mesa_rust_gen::*;

use std::os::raw::*;
use std::ptr;
use std::ptr::*;
use std::sync::Arc;

pub struct PipeContext {
    pipe: NonNull<pipe_context>,
}

unsafe impl Send for PipeContext {}
unsafe impl Sync for PipeContext {}

impl PipeContext {
    pub(super) fn new(context: *mut pipe_context) -> Option<Arc<Self>> {
        let s = Self {
            pipe: NonNull::new(context)?,
        };

        if !has_required_cbs(unsafe { s.pipe.as_ref() }) {
            assert!(false, "Context missing features. This should never happen!");
            return None;
        }

        Some(Arc::new(s))
    }

    pub fn buffer_subdata(
        &self,
        res: &PipeResource,
        offset: c_uint,
        data: *const c_void,
        size: c_uint,
    ) {
        unsafe {
            self.pipe.as_ref().buffer_subdata.unwrap()(
                self.pipe.as_ptr(),
                res.pipe(),
                pipe_map_flags::PIPE_MAP_WRITE.0, // TODO PIPE_MAP_x
                offset,
                size,
                data,
            )
        }
    }

    pub fn buffer_map(
        &self,
        res: &PipeResource,
        offset: i32,
        size: i32,
        block: bool,
    ) -> PipeTransfer {
        let mut b = pipe_box::default();
        let mut out: *mut pipe_transfer = ptr::null_mut();

        b.x = offset;
        b.width = size;
        b.height = 1;
        b.depth = 1;

        let flags = match block {
            false => pipe_map_flags::PIPE_MAP_UNSYNCHRONIZED,
            true => pipe_map_flags(0),
        } | pipe_map_flags::PIPE_MAP_READ_WRITE;

        let ptr = unsafe {
            self.pipe.as_ref().buffer_map.unwrap()(
                self.pipe.as_ptr(),
                res.pipe(),
                0,
                flags.0,
                &b,
                &mut out,
            )
        };

        PipeTransfer::new(out, ptr)
    }

    pub(super) fn buffer_unmap(&self, tx: *mut pipe_transfer) {
        unsafe { self.pipe.as_ref().buffer_unmap.unwrap()(self.pipe.as_ptr(), tx) };
    }

    pub fn blit(&self, src: &PipeResource, dst: &PipeResource) {
        let mut blit_info = pipe_blit_info::default();
        blit_info.src.resource = src.pipe();
        blit_info.dst.resource = dst.pipe();

        println!("blit not implemented!");

        unsafe { self.pipe.as_ref().blit.unwrap()(self.pipe.as_ptr(), &blit_info) }
    }
}

impl Drop for PipeContext {
    fn drop(&mut self) {
        unsafe {
            self.pipe.as_ref().destroy.unwrap()(self.pipe.as_ptr());
        }
    }
}

fn has_required_cbs(c: &pipe_context) -> bool {
    c.destroy.is_some()
        && c.blit.is_some()
        && c.buffer_map.is_some()
        && c.buffer_subdata.is_some()
        && c.buffer_unmap.is_some()
}
