extern crate mesa_rust;
extern crate rusticl_opencl_gen;

use crate::api::icd::*;
use crate::api::types::*;
use crate::api::util::*;
use crate::core::context::*;
use crate::core::device::*;
use crate::core::queue::*;
use crate::impl_cl_type_trait;

use self::mesa_rust::pipe::context::*;
use self::mesa_rust::pipe::resource::*;
use self::mesa_rust::pipe::transfer::*;
use self::rusticl_opencl_gen::*;

use std::collections::HashMap;
use std::convert::TryInto;
use std::os::raw::c_void;
use std::ptr;
use std::sync::Arc;
use std::sync::Mutex;

#[repr(C)]
pub struct Mem {
    pub base: CLObjectBase<CL_INVALID_MEM_OBJECT>,
    pub context: Arc<Context>,
    pub parent: Option<Arc<Mem>>,
    pub mem_type: cl_mem_object_type,
    pub flags: cl_mem_flags,
    pub size: usize,
    pub offset: usize,
    pub host_ptr: *mut c_void,
    pub image_format: cl_image_format,
    pub image_desc: cl_image_desc,
    pub image_elem_size: u8,
    pub cbs: Mutex<Vec<Box<dyn Fn(cl_mem)>>>,
    res: Option<HashMap<Arc<Device>, PipeResource>>,
    maps: Mutex<HashMap<*mut c_void, (u32, PipeTransfer)>>,
}

impl_cl_type_trait!(cl_mem, Mem, CL_INVALID_MEM_OBJECT);

fn sw_copy(
    src: *const c_void,
    dst: *mut c_void,
    region: &CLVec<usize>,
    src_origin: &CLVec<usize>,
    src_row_pitch: usize,
    src_slice_pitch: usize,
    dst_origin: &CLVec<usize>,
    dst_row_pitch: usize,
    dst_slice_pitch: usize,
) {
    for z in 0..region[2] {
        for y in 0..region[1] {
            unsafe {
                ptr::copy_nonoverlapping(
                    src.add((*src_origin + [0, y, z]) * [1, src_row_pitch, src_slice_pitch]),
                    dst.add((*dst_origin + [0, y, z]) * [1, dst_row_pitch, dst_slice_pitch]),
                    region[0],
                )
            };
        }
    }
}

impl Mem {
    pub fn new_buffer(
        context: Arc<Context>,
        flags: cl_mem_flags,
        size: usize,
        host_ptr: *mut c_void,
    ) -> CLResult<Arc<Mem>> {
        if bit_check(flags, CL_MEM_COPY_HOST_PTR | CL_MEM_ALLOC_HOST_PTR) {
            println!("host ptr semantics not implemented!");
        }

        let buffer = if bit_check(flags, CL_MEM_USE_HOST_PTR) {
            context.create_buffer_from_user(size, host_ptr)
        } else {
            context.create_buffer(size)
        }?;

        let host_ptr = if bit_check(flags, CL_MEM_USE_HOST_PTR) {
            host_ptr
        } else {
            ptr::null_mut()
        };

        Ok(Arc::new(Self {
            base: CLObjectBase::new(),
            context: context,
            parent: None,
            mem_type: CL_MEM_OBJECT_BUFFER,
            flags: flags,
            size: size,
            offset: 0,
            host_ptr: host_ptr,
            image_format: cl_image_format::default(),
            image_desc: cl_image_desc::default(),
            image_elem_size: 0,
            cbs: Mutex::new(Vec::new()),
            res: Some(buffer),
            maps: Mutex::new(HashMap::new()),
        }))
    }

    pub fn new_sub_buffer(
        parent: Arc<Mem>,
        flags: cl_mem_flags,
        offset: usize,
        size: usize,
    ) -> Arc<Mem> {
        let host_ptr = if parent.host_ptr.is_null() {
            ptr::null_mut()
        } else {
            unsafe { parent.host_ptr.add(offset) }
        };

        Arc::new(Self {
            base: CLObjectBase::new(),
            context: parent.context.clone(),
            parent: Some(parent),
            mem_type: CL_MEM_OBJECT_BUFFER,
            flags: flags,
            size: size,
            offset: offset,
            host_ptr: host_ptr,
            image_format: cl_image_format::default(),
            image_desc: cl_image_desc::default(),
            image_elem_size: 0,
            cbs: Mutex::new(Vec::new()),
            res: None,
            maps: Mutex::new(HashMap::new()),
        })
    }

    pub fn new_image(
        context: Arc<Context>,
        mem_type: cl_mem_object_type,
        flags: cl_mem_flags,
        image_format: &cl_image_format,
        image_desc: cl_image_desc,
        image_elem_size: u8,
        host_ptr: *mut c_void,
    ) -> Arc<Mem> {
        if bit_check(
            flags,
            CL_MEM_USE_HOST_PTR | CL_MEM_COPY_HOST_PTR | CL_MEM_ALLOC_HOST_PTR,
        ) {
            println!("host ptr semantics not implemented!");
        }

        let host_ptr = if bit_check(flags, CL_MEM_USE_HOST_PTR) {
            host_ptr
        } else {
            ptr::null_mut()
        };

        Arc::new(Self {
            base: CLObjectBase::new(),
            context: context,
            parent: None,
            mem_type: mem_type,
            flags: flags,
            size: 0,
            offset: 0,
            host_ptr: host_ptr,
            image_format: *image_format,
            image_desc: image_desc,
            image_elem_size: image_elem_size,
            cbs: Mutex::new(Vec::new()),
            res: None,
            maps: Mutex::new(HashMap::new()),
        })
    }

    pub fn is_buffer(&self) -> bool {
        self.mem_type == CL_MEM_OBJECT_BUFFER
    }

    pub fn has_same_parent(&self, other: &Self) -> bool {
        let a = self.parent.as_ref().map_or(self, |p| p);
        let b = other.parent.as_ref().map_or(other, |p| p);
        ptr::eq(a, b)
    }

    fn get_res(&self) -> &HashMap<Arc<Device>, PipeResource> {
        self.parent
            .as_ref()
            .map_or(self, |p| p.as_ref())
            .res
            .as_ref()
            .unwrap()
    }

    pub fn write_from_user(
        &self,
        q: &Arc<Queue>,
        ctx: &PipeContext,
        offset: usize,
        ptr: *const c_void,
        size: usize,
    ) -> CLResult<()> {
        // TODO support sub buffers
        let r = self.get_res().get(&q.device).unwrap();
        ctx.buffer_subdata(
            r,
            offset.try_into().map_err(|_| CL_OUT_OF_HOST_MEMORY)?,
            ptr,
            size.try_into().map_err(|_| CL_OUT_OF_HOST_MEMORY)?,
        );
        Ok(())
    }

    pub fn write_from_user_rect(
        &self,
        src: *const c_void,
        q: &Arc<Queue>,
        ctx: &Arc<PipeContext>,
        region: &CLVec<usize>,
        src_origin: &CLVec<usize>,
        src_row_pitch: usize,
        src_slice_pitch: usize,
        dst_origin: &CLVec<usize>,
        dst_row_pitch: usize,
        dst_slice_pitch: usize,
    ) -> CLResult<()> {
        let r = self.res.as_ref().unwrap().get(&q.device).unwrap();
        let tx = ctx.buffer_map(r, 0, self.size.try_into().unwrap(), true);

        sw_copy(
            src,
            tx.ptr(),
            region,
            src_origin,
            src_row_pitch,
            src_slice_pitch,
            dst_origin,
            dst_row_pitch,
            dst_slice_pitch,
        );

        drop(tx);
        Ok(())
    }

    pub fn read_to_user_rect(
        &self,
        dst: *mut c_void,
        q: &Arc<Queue>,
        ctx: &Arc<PipeContext>,
        region: &CLVec<usize>,
        src_origin: &CLVec<usize>,
        src_row_pitch: usize,
        src_slice_pitch: usize,
        dst_origin: &CLVec<usize>,
        dst_row_pitch: usize,
        dst_slice_pitch: usize,
    ) -> CLResult<()> {
        let r = self.res.as_ref().unwrap().get(&q.device).unwrap();
        let tx = ctx.buffer_map(r, 0, self.size.try_into().unwrap(), true);

        sw_copy(
            tx.ptr(),
            dst,
            region,
            src_origin,
            src_row_pitch,
            src_slice_pitch,
            dst_origin,
            dst_row_pitch,
            dst_slice_pitch,
        );

        drop(tx);
        Ok(())
    }

    pub fn copy_to(
        &self,
        dst: &Self,
        q: &Arc<Queue>,
        ctx: &Arc<PipeContext>,
        region: &CLVec<usize>,
        src_origin: &CLVec<usize>,
        src_row_pitch: usize,
        src_slice_pitch: usize,
        dst_origin: &CLVec<usize>,
        dst_row_pitch: usize,
        dst_slice_pitch: usize,
    ) -> CLResult<()> {
        let res_src = self.res.as_ref().unwrap().get(&q.device).unwrap();
        let res_dst = dst.res.as_ref().unwrap().get(&q.device).unwrap();

        let tx_src = ctx.buffer_map(res_src, 0, self.size.try_into().unwrap(), true);
        let tx_dst = ctx.buffer_map(res_dst, 0, dst.size.try_into().unwrap(), true);

        // TODO check to use hw accelerated paths (e.g. resource_copy_region or blits)
        sw_copy(
            tx_src.ptr(),
            tx_dst.ptr(),
            region,
            src_origin,
            src_row_pitch,
            src_slice_pitch,
            dst_origin,
            dst_row_pitch,
            dst_slice_pitch,
        );

        drop(tx_src);
        drop(tx_dst);

        Ok(())
    }

    // TODO use PIPE_MAP_UNSYNCHRONIZED for non blocking
    pub fn map(&self, q: &Arc<Queue>, offset: usize, size: usize, block: bool) -> *mut c_void {
        let res = self.res.as_ref().unwrap().get(&q.device).unwrap();
        let tx = q.device.helper_ctx().buffer_map(
            res,
            offset.try_into().unwrap(),
            size.try_into().unwrap(),
            block,
        );
        let ptr = tx.ptr();
        let mut lock = self.maps.lock().unwrap();
        let e = lock.get_mut(&ptr);

        // if we already have a mapping, reuse that and increase the refcount
        if let Some(e) = e {
            e.0 += 1;
        } else {
            lock.insert(tx.ptr(), (1, tx));
        }

        ptr
    }

    pub fn is_mapped_ptr(&self, ptr: *mut c_void) -> bool {
        self.maps.lock().unwrap().contains_key(&ptr)
    }

    pub fn unmap(&self, q: &Arc<Queue>, ptr: *mut c_void) {
        let mut lock = self.maps.lock().unwrap();
        let e = lock.get_mut(&ptr).unwrap();

        e.0 -= 1;
        if e.0 == 0 {
            lock.remove(&ptr)
                .unwrap()
                .1
                .with_ctx(&q.device.helper_ctx());
        }
    }
}

impl Drop for Mem {
    fn drop(&mut self) {
        let cl = cl_mem::from_ptr(self);
        self.cbs
            .get_mut()
            .unwrap()
            .iter()
            .rev()
            .for_each(|cb| cb(cl));
    }
}

#[repr(C)]
pub struct Sampler {
    pub base: CLObjectBase<CL_INVALID_SAMPLER>,
    pub context: Arc<Context>,
    pub normalized_coords: bool,
    pub addressing_mode: cl_addressing_mode,
    pub filter_mode: cl_filter_mode,
}

impl_cl_type_trait!(cl_sampler, Sampler, CL_INVALID_SAMPLER);

impl Sampler {
    pub fn new(
        context: Arc<Context>,
        normalized_coords: bool,
        addressing_mode: cl_addressing_mode,
        filter_mode: cl_filter_mode,
    ) -> Arc<Sampler> {
        Arc::new(Self {
            base: CLObjectBase::new(),
            context: context,
            normalized_coords: normalized_coords,
            addressing_mode: addressing_mode,
            filter_mode: filter_mode,
        })
    }
}
