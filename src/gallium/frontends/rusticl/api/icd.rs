#![allow(non_snake_case)]

extern crate mesa_rust_util;
extern crate rusticl_opencl_gen;

use crate::api::context::*;
use crate::api::device::*;
use crate::api::event::*;
use crate::api::kernel::*;
use crate::api::memory::*;
use crate::api::platform::*;
use crate::api::program::*;
use crate::api::queue::*;
use crate::api::types::*;
use crate::api::util::*;

use self::mesa_rust_util::ptr::*;
use self::rusticl_opencl_gen::*;

use std::ffi::CStr;
use std::ptr;
use std::sync::Arc;

pub static DISPATCH: cl_icd_dispatch = cl_icd_dispatch {
    clGetPlatformIDs: Some(cl_get_platform_ids),
    clGetPlatformInfo: Some(cl_get_platform_info),
    clGetDeviceIDs: Some(cl_get_device_ids),
    clGetDeviceInfo: Some(cl_get_device_info),
    clCreateContext: Some(cl_create_context),
    clCreateContextFromType: Some(cl_create_context_from_type),
    clRetainContext: Some(cl_retain_context),
    clReleaseContext: Some(cl_release_context),
    clGetContextInfo: Some(cl_get_context_info),
    clCreateCommandQueue: Some(cl_create_command_queue),
    clRetainCommandQueue: Some(cl_retain_command_queue),
    clReleaseCommandQueue: Some(cl_release_command_queue),
    clGetCommandQueueInfo: Some(cl_get_command_queue_info),
    clSetCommandQueueProperty: None,
    clCreateBuffer: Some(cl_create_buffer),
    clCreateImage2D: Some(cl_create_image_2d),
    clCreateImage3D: Some(cl_create_image_3d),
    clRetainMemObject: Some(cl_retain_mem_object),
    clReleaseMemObject: Some(cl_release_mem_object),
    clGetSupportedImageFormats: Some(cl_get_supported_image_formats),
    clGetMemObjectInfo: Some(cl_get_mem_object_info),
    clGetImageInfo: Some(cl_get_image_info),
    clCreateSampler: Some(cl_create_sampler),
    clRetainSampler: Some(cl_retain_sampler),
    clReleaseSampler: Some(cl_release_sampler),
    clGetSamplerInfo: Some(cl_get_sampler_info),
    clCreateProgramWithSource: Some(cl_create_program_with_source),
    clCreateProgramWithBinary: None,
    clRetainProgram: Some(cl_retain_program),
    clReleaseProgram: Some(cl_release_program),
    clBuildProgram: Some(cl_build_program),
    clUnloadCompiler: None,
    clGetProgramInfo: Some(cl_get_program_info),
    clGetProgramBuildInfo: Some(cl_get_program_build_info),
    clCreateKernel: Some(cl_create_kernel),
    clCreateKernelsInProgram: Some(cl_create_kernels_in_program),
    clRetainKernel: Some(cl_retain_kernel),
    clReleaseKernel: Some(cl_release_kernel),
    clSetKernelArg: Some(cl_set_kernel_arg),
    clGetKernelInfo: Some(cl_get_kernel_info),
    clGetKernelWorkGroupInfo: Some(cl_get_kernel_work_group_info),
    clWaitForEvents: Some(cl_wait_for_events),
    clGetEventInfo: Some(cl_get_event_info),
    clRetainEvent: Some(cl_retain_event),
    clReleaseEvent: Some(cl_release_event),
    clGetEventProfilingInfo: Some(cl_get_event_profiling_info),
    clFlush: Some(cl_flush),
    clFinish: Some(cl_finish),
    clEnqueueReadBuffer: Some(cl_enqueue_read_buffer),
    clEnqueueWriteBuffer: Some(cl_enqueue_write_buffer),
    clEnqueueCopyBuffer: Some(cl_enqueue_copy_buffer),
    clEnqueueReadImage: Some(cl_enqueue_read_image),
    clEnqueueWriteImage: Some(cl_enqueue_write_image),
    clEnqueueCopyImage: Some(cl_enqueue_copy_image),
    clEnqueueCopyImageToBuffer: Some(cl_enqueue_copy_image_to_buffer),
    clEnqueueCopyBufferToImage: Some(cl_enqueue_copy_buffer_to_image),
    clEnqueueMapBuffer: Some(cl_enqueue_map_buffer),
    clEnqueueMapImage: Some(cl_enqueue_map_image),
    clEnqueueUnmapMemObject: Some(cl_enqueue_unmap_mem_object),
    clEnqueueNDRangeKernel: Some(cl_enqueue_ndrange_kernel),
    clEnqueueTask: None,
    clEnqueueNativeKernel: None,
    clEnqueueMarker: None,
    clEnqueueWaitForEvents: None,
    clEnqueueBarrier: None,
    clGetExtensionFunctionAddress: Some(cl_get_extension_function_address),
    clCreateFromGLBuffer: None,
    clCreateFromGLTexture2D: None,
    clCreateFromGLTexture3D: None,
    clCreateFromGLRenderbuffer: None,
    clGetGLObjectInfo: None,
    clGetGLTextureInfo: None,
    clEnqueueAcquireGLObjects: None,
    clEnqueueReleaseGLObjects: None,
    clGetGLContextInfoKHR: None,
    clGetDeviceIDsFromD3D10KHR: ptr::null_mut(),
    clCreateFromD3D10BufferKHR: ptr::null_mut(),
    clCreateFromD3D10Texture2DKHR: ptr::null_mut(),
    clCreateFromD3D10Texture3DKHR: ptr::null_mut(),
    clEnqueueAcquireD3D10ObjectsKHR: ptr::null_mut(),
    clEnqueueReleaseD3D10ObjectsKHR: ptr::null_mut(),
    clSetEventCallback: Some(cl_set_event_callback),
    clCreateSubBuffer: Some(cl_create_sub_buffer),
    clSetMemObjectDestructorCallback: Some(cl_set_mem_object_destructor_callback),
    clCreateUserEvent: Some(cl_create_user_event),
    clSetUserEventStatus: Some(cl_set_user_event_status),
    clEnqueueReadBufferRect: Some(cl_enqueue_read_buffer_rect),
    clEnqueueWriteBufferRect: Some(cl_enqueue_write_buffer_rect),
    clEnqueueCopyBufferRect: Some(cl_enqueue_copy_buffer_rect),
    clCreateSubDevicesEXT: None,
    clRetainDeviceEXT: None,
    clReleaseDeviceEXT: None,
    clCreateEventFromGLsyncKHR: None,
    clCreateSubDevices: None,
    clRetainDevice: None,
    clReleaseDevice: None,
    clCreateImage: Some(cl_create_image),
    clCreateProgramWithBuiltInKernels: None,
    clCompileProgram: Some(cl_compile_program),
    clLinkProgram: Some(cl_link_program),
    clUnloadPlatformCompiler: Some(cl_unload_platform_compiler),
    clGetKernelArgInfo: Some(cl_get_kernel_arg_info),
    clEnqueueFillBuffer: None,
    clEnqueueFillImage: Some(cl_enqueue_fill_image),
    clEnqueueMigrateMemObjects: None,
    clEnqueueMarkerWithWaitList: None,
    clEnqueueBarrierWithWaitList: None,
    clGetExtensionFunctionAddressForPlatform: None,
    clCreateFromGLTexture: None,
    clGetDeviceIDsFromD3D11KHR: ptr::null_mut(),
    clCreateFromD3D11BufferKHR: ptr::null_mut(),
    clCreateFromD3D11Texture2DKHR: ptr::null_mut(),
    clCreateFromD3D11Texture3DKHR: ptr::null_mut(),
    clCreateFromDX9MediaSurfaceKHR: ptr::null_mut(),
    clEnqueueAcquireD3D11ObjectsKHR: ptr::null_mut(),
    clEnqueueReleaseD3D11ObjectsKHR: ptr::null_mut(),
    clGetDeviceIDsFromDX9MediaAdapterKHR: ptr::null_mut(),
    clEnqueueAcquireDX9MediaSurfacesKHR: ptr::null_mut(),
    clEnqueueReleaseDX9MediaSurfacesKHR: ptr::null_mut(),
    clCreateFromEGLImageKHR: None,
    clEnqueueAcquireEGLObjectsKHR: None,
    clEnqueueReleaseEGLObjectsKHR: None,
    clCreateEventFromEGLSyncKHR: None,
    clCreateCommandQueueWithProperties: Some(cl_create_command_queue_with_properties),
    clCreatePipe: None,
    clGetPipeInfo: None,
    clSVMAlloc: None,
    clSVMFree: None,
    clEnqueueSVMFree: None,
    clEnqueueSVMMemcpy: None,
    clEnqueueSVMMemFill: None,
    clEnqueueSVMMap: None,
    clEnqueueSVMUnmap: None,
    clCreateSamplerWithProperties: None,
    clSetKernelArgSVMPointer: None,
    clSetKernelExecInfo: None,
    clGetKernelSubGroupInfoKHR: None,
    clCloneKernel: None,
    clCreateProgramWithIL: None,
    clEnqueueSVMMigrateMem: None,
    clGetDeviceAndHostTimer: None,
    clGetHostTimer: None,
    clGetKernelSubGroupInfo: None,
    clSetDefaultDeviceCommandQueue: None,
    clSetProgramReleaseCallback: None,
    clSetProgramSpecializationConstant: None,
    clCreateBufferWithProperties: None,
    clCreateImageWithProperties: None,
    clSetContextDestructorCallback: None,
};

pub type CLError = cl_int;
pub type CLResult<T> = Result<T, CLError>;

#[repr(C)]
pub struct CLObjectBase<const ERR: i32> {
    dispatch: &'static cl_icd_dispatch,
    type_err: i32,
}

impl<const ERR: i32> Default for CLObjectBase<ERR> {
    fn default() -> Self {
        Self::new()
    }
}

impl<const ERR: i32> CLObjectBase<ERR> {
    pub fn new() -> Self {
        Self {
            dispatch: &DISPATCH,
            type_err: ERR,
        }
    }

    pub fn check_ptr(ptr: *const Self) -> CLResult<()> {
        if ptr.is_null() {
            return Err(ERR);
        }

        unsafe {
            if !::std::ptr::eq((*ptr).dispatch, &DISPATCH) {
                return Err(ERR);
            }

            if (*ptr).type_err != ERR {
                return Err(ERR);
            }

            Ok(())
        }
    }
}

pub trait ReferenceCountedAPIPointer<T, const ERR: i32> {
    fn get_ptr(&self) -> CLResult<*const T>;

    // TODO:  I can't find a trait that would let me say T: pointer so that
    // I can do the cast in the main trait implementation.  So we need to
    // implement that as part of the macro where we know the real type.
    fn from_ptr(ptr: *const T) -> Self;

    fn leak_ref(ptr: *mut Self, r: &std::sync::Arc<T>)
    where
        Self: Sized,
    {
        if !ptr.is_null() {
            unsafe {
                ptr.write(Self::from_arc(r.clone()));
            }
        }
    }

    fn get_ref(&self) -> CLResult<&'static T> {
        unsafe { Ok(self.get_ptr()?.as_ref().unwrap()) }
    }

    fn get_arc(&self) -> CLResult<Arc<T>> {
        unsafe {
            let ptr = self.get_ptr()?;
            Arc::increment_strong_count(ptr);
            Ok(Arc::from_raw(ptr))
        }
    }

    fn from_arc(arc: Arc<T>) -> Self
    where
        Self: Sized,
    {
        Self::from_ptr(Arc::into_raw(arc))
    }

    fn get_arc_vec_from_arr(objs: *const Self, count: u32) -> CLResult<Vec<Arc<T>>>
    where
        Self: Sized,
    {
        // CL spec requires validation for obj arrays, both values have to make sense
        if objs.is_null() && count > 0 || !objs.is_null() && count == 0 {
            return Err(CL_INVALID_VALUE);
        }

        let mut res = Vec::new();
        if objs.is_null() || count == 0 {
            return Ok(res);
        }

        for i in 0..count as usize {
            unsafe {
                res.push((*objs.add(i)).get_arc()?);
            }
        }
        Ok(res)
    }

    fn retain(&self) -> CLResult<()> {
        unsafe {
            Arc::increment_strong_count(self.get_ptr()?);
            Ok(())
        }
    }

    fn release(&self) -> CLResult<Arc<T>> {
        unsafe { Ok(Arc::from_raw(self.get_ptr()?)) }
    }

    fn refcnt(&self) -> CLResult<u32> {
        Ok((Arc::strong_count(&self.get_arc()?) - 1) as u32)
    }
}

#[macro_export]
macro_rules! impl_cl_type_trait {
    ($cl: ident, $t: ty, $err: ident) => {
        impl $crate::api::icd::ReferenceCountedAPIPointer<$t, $err> for $cl {
            fn get_ptr(&self) -> CLResult<*const $t> {
                type Base = $crate::api::icd::CLObjectBase<$err>;
                Base::check_ptr(self.cast())?;

                // Now that we've verified the object, it should be safe to
                // dereference it.  As one more double check, make sure that
                // the CLObjectBase is at the start of the object
                let obj_ptr: *const $t = self.cast();
                unsafe {
                    let base_ptr = ::std::ptr::addr_of!((*obj_ptr).base);
                    assert!((obj_ptr as usize) == (base_ptr as usize));
                }

                Ok(obj_ptr)
            }

            fn from_ptr(ptr: *const $t) -> Self {
                ptr as Self
            }
        }

        // there are two reason to implement those traits for all objects
        //   1. it speeds up operations
        //   2. we want to check for real equality more explicit to stay conformant with the API
        //      and to not break in subtle ways e.g. using CL objects as keys in HashMaps.
        impl std::cmp::Eq for $t {}
        impl std::cmp::PartialEq for $t {
            fn eq(&self, other: &Self) -> bool {
                (self as *const Self) == (other as *const Self)
            }
        }

        impl std::hash::Hash for $t {
            fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
                (self as *const Self).hash(state);
            }
        }
    };
}

// We need those functions exported

#[no_mangle]
extern "C" fn clGetPlatformInfo(
    platform: cl_platform_id,
    param_name: cl_platform_info,
    param_value_size: usize,
    param_value: *mut ::std::ffi::c_void,
    param_value_size_ret: *mut usize,
) -> cl_int {
    cl_get_platform_info(
        platform,
        param_name,
        param_value_size,
        param_value,
        param_value_size_ret,
    )
}

#[no_mangle]
extern "C" fn clGetExtensionFunctionAddress(
    function_name: *const ::std::os::raw::c_char,
) -> *mut ::std::ffi::c_void {
    cl_get_extension_function_address(function_name)
}

#[no_mangle]
extern "C" fn clIcdGetPlatformIDsKHR(
    num_entries: cl_uint,
    platforms: *mut cl_platform_id,
    num_platforms: *mut cl_uint,
) -> cl_int {
    cl_icd_get_platform_ids_khr(num_entries, platforms, num_platforms)
}

// helper macros to make it less painful

macro_rules! match_err {
    ($exp: expr) => {
        match $exp {
            Ok(_) => CL_SUCCESS as cl_int,
            Err(e) => e,
        }
    };
}

macro_rules! match_obj {
    ($exp: expr, $err: ident) => {
        match $exp {
            Ok(o) => {
                $err.write_checked(CL_SUCCESS as cl_int);
                o
            }
            Err(e) => {
                $err.write_checked(e);
                ptr::null_mut()
            }
        }
    };
}

macro_rules! match_obj_expl {
    ($exp: expr, $err: ident) => {
        match $exp {
            Ok((o, c)) => {
                $err.write_checked(c as cl_int);
                o
            }
            Err(e) => {
                $err.write_checked(e);
                ptr::null_mut()
            }
        }
    };
}

// extern "C" function stubs in ICD and extension order

extern "C" fn cl_get_platform_ids(
    num_entries: cl_uint,
    platforms: *mut cl_platform_id,
    num_platforms: *mut cl_uint,
) -> cl_int {
    match_err!(get_platform_ids(num_entries, platforms, num_platforms))
}

extern "C" fn cl_get_platform_info(
    platform: cl_platform_id,
    param_name: cl_platform_info,
    param_value_size: usize,
    param_value: *mut ::std::ffi::c_void,
    param_value_size_ret: *mut usize,
) -> cl_int {
    match_err!(platform.get_info(
        param_name,
        param_value_size,
        param_value,
        param_value_size_ret,
    ))
}

extern "C" fn cl_get_device_ids(
    platform: cl_platform_id,
    device_type: cl_device_type,
    num_entries: cl_uint,
    devices: *mut cl_device_id,
    num_devices: *mut cl_uint,
) -> cl_int {
    match_err!(get_device_ids(
        platform,
        device_type,
        num_entries,
        devices,
        num_devices
    ))
}

extern "C" fn cl_get_device_info(
    device: cl_device_id,
    param_name: cl_device_info,
    param_value_size: usize,
    param_value: *mut ::std::os::raw::c_void,
    param_value_size_ret: *mut usize,
) -> cl_int {
    match_err!(device.get_info(
        param_name,
        param_value_size,
        param_value,
        param_value_size_ret,
    ))
}

extern "C" fn cl_create_context(
    properties: *const cl_context_properties,
    num_devices: cl_uint,
    devices: *const cl_device_id,
    pfn_notify: Option<CreateContextCB>,
    user_data: *mut ::std::os::raw::c_void,
    errcode_ret: *mut cl_int,
) -> cl_context {
    match_obj!(
        create_context(properties, num_devices, devices, pfn_notify, user_data),
        errcode_ret
    )
}

extern "C" fn cl_create_context_from_type(
    properties: *const cl_context_properties,
    device_type: cl_device_type,
    pfn_notify: Option<CreateContextCB>,
    user_data: *mut ::std::ffi::c_void,
    errcode_ret: *mut cl_int,
) -> cl_context {
    match_obj!(
        create_context_from_type(properties, device_type, pfn_notify, user_data),
        errcode_ret
    )
}

extern "C" fn cl_retain_context(context: cl_context) -> cl_int {
    match_err!(context.retain())
}

extern "C" fn cl_release_context(context: cl_context) -> cl_int {
    match_err!(context.release())
}

extern "C" fn cl_get_context_info(
    context: cl_context,
    param_name: cl_context_info,
    param_value_size: usize,
    param_value: *mut ::std::os::raw::c_void,
    param_value_size_ret: *mut usize,
) -> cl_int {
    match_err!(context.get_info(
        param_name,
        param_value_size,
        param_value,
        param_value_size_ret,
    ))
}

extern "C" fn cl_create_command_queue(
    context: cl_context,
    device: cl_device_id,
    properties: cl_command_queue_properties,
    errcode_ret: *mut cl_int,
) -> cl_command_queue {
    match_obj!(
        create_command_queue(context, device, properties),
        errcode_ret
    )
}

extern "C" fn cl_retain_command_queue(command_queue: cl_command_queue) -> cl_int {
    match_err!(command_queue.retain())
}

extern "C" fn cl_release_command_queue(command_queue: cl_command_queue) -> cl_int {
    match_err!(command_queue.release())
}

extern "C" fn cl_get_command_queue_info(
    command_queue: cl_command_queue,
    param_name: cl_command_queue_info,
    param_value_size: usize,
    param_value: *mut ::std::os::raw::c_void,
    param_value_size_ret: *mut usize,
) -> cl_int {
    match_err!(command_queue.get_info(
        param_name,
        param_value_size,
        param_value,
        param_value_size_ret,
    ))
}

extern "C" fn cl_create_buffer(
    context: cl_context,
    flags: cl_mem_flags,
    size: usize,
    host_ptr: *mut ::std::os::raw::c_void,
    errcode_ret: *mut cl_int,
) -> cl_mem {
    match_obj!(create_buffer(context, flags, size, host_ptr,), errcode_ret)
}

extern "C" fn cl_create_image_2d(
    _context: cl_context,
    _flags: cl_mem_flags,
    _image_format: *const cl_image_format,
    _image_width: usize,
    _image_height: usize,
    _image_row_pitch: usize,
    _host_ptr: *mut ::std::os::raw::c_void,
    errcode_ret: *mut cl_int,
) -> cl_mem {
    println!("cl_create_image_2d not implemented");
    errcode_ret.write_checked(CL_OUT_OF_HOST_MEMORY);
    ptr::null_mut()
}

extern "C" fn cl_create_image_3d(
    _context: cl_context,
    _flags: cl_mem_flags,
    _image_format: *const cl_image_format,
    _image_width: usize,
    _image_height: usize,
    _image_depth: usize,
    _image_row_pitch: usize,
    _image_slice_pitch: usize,
    _host_ptr: *mut ::std::os::raw::c_void,
    errcode_ret: *mut cl_int,
) -> cl_mem {
    println!("cl_create_image_3d not implemented");
    errcode_ret.write_checked(CL_OUT_OF_HOST_MEMORY);
    ptr::null_mut()
}

extern "C" fn cl_retain_mem_object(mem: cl_mem) -> cl_int {
    match_err!(mem.retain())
}

extern "C" fn cl_release_mem_object(mem: cl_mem) -> cl_int {
    match_err!(mem.release())
}

extern "C" fn cl_get_supported_image_formats(
    context: cl_context,
    flags: cl_mem_flags,
    image_type: cl_mem_object_type,
    num_entries: cl_uint,
    image_formats: *mut cl_image_format,
    num_image_formats: *mut cl_uint,
) -> cl_int {
    match_err!(get_supported_image_formats(
        context,
        flags,
        image_type,
        num_entries,
        image_formats,
        num_image_formats
    ))
}

extern "C" fn cl_get_mem_object_info(
    memobj: cl_mem,
    param_name: cl_mem_info,
    param_value_size: usize,
    param_value: *mut ::std::os::raw::c_void,
    param_value_size_ret: *mut usize,
) -> cl_int {
    match_err!(memobj.get_info(
        param_name,
        param_value_size,
        param_value,
        param_value_size_ret,
    ))
}

extern "C" fn cl_get_image_info(
    image: cl_mem,
    param_name: cl_image_info,
    param_value_size: usize,
    param_value: *mut ::std::os::raw::c_void,
    param_value_size_ret: *mut usize,
) -> cl_int {
    match_err!(image.get_info(
        param_name,
        param_value_size,
        param_value,
        param_value_size_ret,
    ))
}

extern "C" fn cl_create_sampler(
    context: cl_context,
    normalized_coords: cl_bool,
    addressing_mode: cl_addressing_mode,
    filter_mode: cl_filter_mode,
    errcode_ret: *mut cl_int,
) -> cl_sampler {
    match_obj!(
        create_sampler(context, normalized_coords, addressing_mode, filter_mode),
        errcode_ret
    )
}

extern "C" fn cl_retain_sampler(sampler: cl_sampler) -> cl_int {
    match_err!(sampler.retain())
}

extern "C" fn cl_release_sampler(sampler: cl_sampler) -> cl_int {
    match_err!(sampler.release())
}

extern "C" fn cl_get_sampler_info(
    sampler: cl_sampler,
    param_name: cl_sampler_info,
    param_value_size: usize,
    param_value: *mut ::std::os::raw::c_void,
    param_value_size_ret: *mut usize,
) -> cl_int {
    match_err!(sampler.get_info(
        param_name,
        param_value_size,
        param_value,
        param_value_size_ret,
    ))
}

extern "C" fn cl_create_program_with_source(
    context: cl_context,
    count: cl_uint,
    strings: *mut *const ::std::os::raw::c_char,
    lengths: *const usize,
    errcode_ret: *mut cl_int,
) -> cl_program {
    match_obj!(
        create_program_with_source(context, count, strings, lengths),
        errcode_ret
    )
}

extern "C" fn cl_retain_program(program: cl_program) -> cl_int {
    match_err!(program.retain())
}

extern "C" fn cl_release_program(program: cl_program) -> cl_int {
    match_err!(program.release())
}

extern "C" fn cl_build_program(
    program: cl_program,
    num_devices: cl_uint,
    device_list: *const cl_device_id,
    options: *const ::std::os::raw::c_char,
    pfn_notify: Option<ProgramCB>,
    user_data: *mut ::std::os::raw::c_void,
) -> cl_int {
    match_err!(build_program(
        program,
        num_devices,
        device_list,
        options,
        pfn_notify,
        user_data,
    ))
}

extern "C" fn cl_get_program_info(
    program: cl_program,
    param_name: cl_program_info,
    param_value_size: usize,
    param_value: *mut ::std::os::raw::c_void,
    param_value_size_ret: *mut usize,
) -> cl_int {
    match_err!(program.get_info(
        param_name,
        param_value_size,
        param_value,
        param_value_size_ret,
    ))
}

extern "C" fn cl_get_program_build_info(
    program: cl_program,
    device: cl_device_id,
    param_name: cl_program_build_info,
    param_value_size: usize,
    param_value: *mut ::std::os::raw::c_void,
    param_value_size_ret: *mut usize,
) -> cl_int {
    match_err!(program.get_info_obj(
        device,
        param_name,
        param_value_size,
        param_value,
        param_value_size_ret,
    ))
}

extern "C" fn cl_create_kernel(
    program: cl_program,
    kernel_name: *const ::std::os::raw::c_char,
    errcode_ret: *mut cl_int,
) -> cl_kernel {
    match_obj!(create_kernel(program, kernel_name), errcode_ret)
}

extern "C" fn cl_create_kernels_in_program(
    program: cl_program,
    num_kernels: cl_uint,
    kernels: *mut cl_kernel,
    num_kernels_ret: *mut cl_uint,
) -> cl_int {
    match_err!(create_kernels_in_program(
        program,
        num_kernels,
        kernels,
        num_kernels_ret
    ))
}

extern "C" fn cl_retain_kernel(kernel: cl_kernel) -> cl_int {
    match_err!(kernel.retain())
}

extern "C" fn cl_release_kernel(kernel: cl_kernel) -> cl_int {
    match_err!(kernel.release())
}

extern "C" fn cl_set_kernel_arg(
    kernel: cl_kernel,
    arg_index: cl_uint,
    arg_size: usize,
    arg_value: *const ::std::os::raw::c_void,
) -> cl_int {
    match_err!(set_kernel_arg(kernel, arg_index, arg_size, arg_value))
}

extern "C" fn cl_get_kernel_info(
    kernel: cl_kernel,
    param_name: cl_kernel_info,
    param_value_size: usize,
    param_value: *mut ::std::os::raw::c_void,
    param_value_size_ret: *mut usize,
) -> cl_int {
    match_err!(kernel.get_info(
        param_name,
        param_value_size,
        param_value,
        param_value_size_ret,
    ))
}

extern "C" fn cl_get_kernel_work_group_info(
    kernel: cl_kernel,
    device: cl_device_id,
    param_name: cl_kernel_work_group_info,
    param_value_size: usize,
    param_value: *mut ::std::os::raw::c_void,
    param_value_size_ret: *mut usize,
) -> cl_int {
    match_err!(kernel.get_info_obj(
        device,
        param_name,
        param_value_size,
        param_value,
        param_value_size_ret,
    ))
}

extern "C" fn cl_wait_for_events(num_events: cl_uint, event_list: *const cl_event) -> cl_int {
    match_err!(wait_for_events(num_events, event_list))
}

extern "C" fn cl_get_event_info(
    event: cl_event,
    param_name: cl_event_info,
    param_value_size: usize,
    param_value: *mut ::std::os::raw::c_void,
    param_value_size_ret: *mut usize,
) -> cl_int {
    match_err!(event.get_info(
        param_name,
        param_value_size,
        param_value,
        param_value_size_ret,
    ))
}

extern "C" fn cl_retain_event(event: cl_event) -> cl_int {
    match_err!(event.retain())
}

extern "C" fn cl_release_event(event: cl_event) -> cl_int {
    match_err!(event.release())
}

extern "C" fn cl_get_event_profiling_info(
    _event: cl_event,
    _param_name: cl_profiling_info,
    _param_value_size: usize,
    _param_value: *mut ::std::os::raw::c_void,
    _param_value_size_ret: *mut usize,
) -> cl_int {
    println!("cl_get_event_profiling_info not implemented");
    CL_OUT_OF_HOST_MEMORY
}

extern "C" fn cl_flush(command_queue: cl_command_queue) -> cl_int {
    match_err!(flush_queue(command_queue))
}

extern "C" fn cl_finish(command_queue: cl_command_queue) -> cl_int {
    match_err!(finish_queue(command_queue))
}

extern "C" fn cl_enqueue_read_buffer(
    command_queue: cl_command_queue,
    buffer: cl_mem,
    blocking_read: cl_bool,
    offset: usize,
    cb: usize,
    ptr: *mut ::std::os::raw::c_void,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> cl_int {
    match_err!(enqueue_read_buffer(
        command_queue,
        buffer,
        blocking_read,
        offset,
        cb,
        ptr,
        num_events_in_wait_list,
        event_wait_list,
        event
    ))
}

extern "C" fn cl_enqueue_write_buffer(
    command_queue: cl_command_queue,
    buffer: cl_mem,
    blocking_write: cl_bool,
    offset: usize,
    cb: usize,
    ptr: *const ::std::os::raw::c_void,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> cl_int {
    match_err!(enqueue_write_buffer(
        command_queue,
        buffer,
        blocking_write,
        offset,
        cb,
        ptr,
        num_events_in_wait_list,
        event_wait_list,
        event
    ))
}

extern "C" fn cl_enqueue_copy_buffer(
    _command_queue: cl_command_queue,
    _src_buffer: cl_mem,
    _dst_buffer: cl_mem,
    _src_offset: usize,
    _dst_offset: usize,
    _cb: usize,
    _num_events_in_wait_list: cl_uint,
    _event_wait_list: *const cl_event,
    _event: *mut cl_event,
) -> cl_int {
    println!("cl_enqueue_copy_buffer not implemented");
    CL_OUT_OF_HOST_MEMORY
}

extern "C" fn cl_enqueue_read_image(
    _command_queue: cl_command_queue,
    _image: cl_mem,
    _blocking_read: cl_bool,
    _origin: *const usize,
    _region: *const usize,
    _row_pitch: usize,
    _slice_pitch: usize,
    _ptr: *mut ::std::os::raw::c_void,
    _num_events_in_wait_list: cl_uint,
    _event_wait_list: *const cl_event,
    _event: *mut cl_event,
) -> cl_int {
    println!("cl_enqueue_read_image not implemented");
    CL_OUT_OF_HOST_MEMORY
}

extern "C" fn cl_enqueue_write_image(
    _command_queue: cl_command_queue,
    _image: cl_mem,
    _blocking_write: cl_bool,
    _origin: *const usize,
    _region: *const usize,
    _input_row_pitch: usize,
    _input_slice_pitch: usize,
    _ptr: *const ::std::os::raw::c_void,
    _num_events_in_wait_list: cl_uint,
    _event_wait_list: *const cl_event,
    _event: *mut cl_event,
) -> cl_int {
    println!("cl_enqueue_write_image not implemented");
    CL_OUT_OF_HOST_MEMORY
}

extern "C" fn cl_enqueue_copy_image(
    _command_queue: cl_command_queue,
    _src_image: cl_mem,
    _dst_image: cl_mem,
    _src_origin: *const usize,
    _dst_origin: *const usize,
    _region: *const usize,
    _num_events_in_wait_list: cl_uint,
    _event_wait_list: *const cl_event,
    _event: *mut cl_event,
) -> cl_int {
    println!("cl_enqueue_copy_image not implemented");
    CL_OUT_OF_HOST_MEMORY
}

extern "C" fn cl_enqueue_copy_image_to_buffer(
    _command_queue: cl_command_queue,
    _src_image: cl_mem,
    _dst_buffer: cl_mem,
    _src_origin: *const usize,
    _region: *const usize,
    _dst_offset: usize,
    _num_events_in_wait_list: cl_uint,
    _event_wait_list: *const cl_event,
    _event: *mut cl_event,
) -> cl_int {
    println!("cl_enqueue_copy_image_to_buffer not implemented");
    CL_OUT_OF_HOST_MEMORY
}

extern "C" fn cl_enqueue_copy_buffer_to_image(
    _command_queue: cl_command_queue,
    _src_buffer: cl_mem,
    _dst_image: cl_mem,
    _src_offset: usize,
    _dst_origin: *const usize,
    _region: *const usize,
    _num_events_in_wait_list: cl_uint,
    _event_wait_list: *const cl_event,
    _event: *mut cl_event,
) -> cl_int {
    println!("cl_enqueue_copy_buffer_to_image not implemented");
    CL_OUT_OF_HOST_MEMORY
}

extern "C" fn cl_enqueue_map_buffer(
    command_queue: cl_command_queue,
    buffer: cl_mem,
    blocking_map: cl_bool,
    map_flags: cl_map_flags,
    offset: usize,
    cb: usize,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
    errcode_ret: *mut cl_int,
) -> *mut ::std::os::raw::c_void {
    match_obj!(
        enqueue_map_buffer(
            command_queue,
            buffer,
            blocking_map,
            map_flags,
            offset,
            cb,
            num_events_in_wait_list,
            event_wait_list,
            event,
        ),
        errcode_ret
    )
}

extern "C" fn cl_enqueue_map_image(
    _command_queue: cl_command_queue,
    _image: cl_mem,
    _blocking_map: cl_bool,
    _map_flags: cl_map_flags,
    _origin: *const usize,
    _region: *const usize,
    _image_row_pitch: *mut usize,
    _image_slice_pitch: *mut usize,
    _num_events_in_wait_list: cl_uint,
    _event_wait_list: *const cl_event,
    _event: *mut cl_event,
    errcode_ret: *mut cl_int,
) -> *mut ::std::os::raw::c_void {
    println!("cl_enqueue_map_image not implemented");
    errcode_ret.write_checked(CL_OUT_OF_HOST_MEMORY);
    ptr::null_mut()
}

extern "C" fn cl_enqueue_unmap_mem_object(
    command_queue: cl_command_queue,
    memobj: cl_mem,
    mapped_ptr: *mut ::std::os::raw::c_void,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> cl_int {
    match_err!(enqueue_unmap_mem_object(
        command_queue,
        memobj,
        mapped_ptr,
        num_events_in_wait_list,
        event_wait_list,
        event
    ))
}

extern "C" fn cl_enqueue_ndrange_kernel(
    command_queue: cl_command_queue,
    kernel: cl_kernel,
    work_dim: cl_uint,
    global_work_offset: *const usize,
    global_work_size: *const usize,
    local_work_size: *const usize,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> cl_int {
    match_err!(enqueue_ndrange_kernel(
        command_queue,
        kernel,
        work_dim,
        global_work_offset,
        global_work_size,
        local_work_size,
        num_events_in_wait_list,
        event_wait_list,
        event
    ))
}

extern "C" fn cl_get_extension_function_address(
    function_name: *const ::std::os::raw::c_char,
) -> *mut ::std::ffi::c_void {
    if function_name.is_null() {
        return ptr::null_mut();
    }
    match unsafe { CStr::from_ptr(function_name) }.to_str().unwrap() {
        "clGetPlatformInfo" => cl_get_platform_info as *mut std::ffi::c_void,
        "clIcdGetPlatformIDsKHR" => cl_icd_get_platform_ids_khr as *mut std::ffi::c_void,
        _ => ptr::null_mut(),
    }
}

extern "C" fn cl_set_event_callback(
    event: cl_event,
    command_exec_callback_type: cl_int,
    pfn_notify: Option<EventCB>,
    user_data: *mut ::std::os::raw::c_void,
) -> cl_int {
    match_err!(set_event_callback(
        event,
        command_exec_callback_type,
        pfn_notify,
        user_data
    ))
}

extern "C" fn cl_create_sub_buffer(
    buffer: cl_mem,
    flags: cl_mem_flags,
    buffer_create_type: cl_buffer_create_type,
    buffer_create_info: *const ::std::os::raw::c_void,
    errcode_ret: *mut cl_int,
) -> cl_mem {
    match_obj!(
        create_sub_buffer(buffer, flags, buffer_create_type, buffer_create_info,),
        errcode_ret
    )
}

extern "C" fn cl_set_mem_object_destructor_callback(
    memobj: cl_mem,
    pfn_notify: Option<MemCB>,
    user_data: *mut ::std::os::raw::c_void,
) -> cl_int {
    match_err!(set_mem_object_destructor_callback(
        memobj, pfn_notify, user_data,
    ))
}

extern "C" fn cl_create_user_event(context: cl_context, errcode_ret: *mut cl_int) -> cl_event {
    match_obj!(create_user_event(context), errcode_ret)
}

extern "C" fn cl_set_user_event_status(event: cl_event, execution_status: cl_int) -> cl_int {
    match_err!(set_user_event_status(event, execution_status))
}

extern "C" fn cl_enqueue_read_buffer_rect(
    command_queue: cl_command_queue,
    buffer: cl_mem,
    blocking_read: cl_bool,
    buffer_origin: *const usize,
    host_origin: *const usize,
    region: *const usize,
    buffer_row_pitch: usize,
    buffer_slice_pitch: usize,
    host_row_pitch: usize,
    host_slice_pitch: usize,
    ptr: *mut ::std::os::raw::c_void,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> cl_int {
    match_err!(enqueue_read_buffer_rect(
        command_queue,
        buffer,
        blocking_read,
        buffer_origin,
        host_origin,
        region,
        buffer_row_pitch,
        buffer_slice_pitch,
        host_row_pitch,
        host_slice_pitch,
        ptr,
        num_events_in_wait_list,
        event_wait_list,
        event,
    ))
}

extern "C" fn cl_enqueue_write_buffer_rect(
    command_queue: cl_command_queue,
    buffer: cl_mem,
    blocking_write: cl_bool,
    buffer_origin: *const usize,
    host_origin: *const usize,
    region: *const usize,
    buffer_row_pitch: usize,
    buffer_slice_pitch: usize,
    host_row_pitch: usize,
    host_slice_pitch: usize,
    ptr: *const ::std::os::raw::c_void,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> cl_int {
    match_err!(enqueue_write_buffer_rect(
        command_queue,
        buffer,
        blocking_write,
        buffer_origin,
        host_origin,
        region,
        buffer_row_pitch,
        buffer_slice_pitch,
        host_row_pitch,
        host_slice_pitch,
        ptr,
        num_events_in_wait_list,
        event_wait_list,
        event,
    ))
}

extern "C" fn cl_enqueue_copy_buffer_rect(
    command_queue: cl_command_queue,
    src_buffer: cl_mem,
    dst_buffer: cl_mem,
    src_origin: *const usize,
    dst_origin: *const usize,
    region: *const usize,
    src_row_pitch: usize,
    src_slice_pitch: usize,
    dst_row_pitch: usize,
    dst_slice_pitch: usize,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> cl_int {
    match_err!(enqueue_copy_buffer_rect(
        command_queue,
        src_buffer,
        dst_buffer,
        src_origin,
        dst_origin,
        region,
        src_row_pitch,
        src_slice_pitch,
        dst_row_pitch,
        dst_slice_pitch,
        num_events_in_wait_list,
        event_wait_list,
        event,
    ))
}

extern "C" fn cl_create_image(
    context: cl_context,
    flags: cl_mem_flags,
    image_format: *const cl_image_format,
    image_desc: *const cl_image_desc,
    host_ptr: *mut ::std::os::raw::c_void,
    errcode_ret: *mut cl_int,
) -> cl_mem {
    match_obj!(
        create_image(context, flags, image_format, image_desc, host_ptr),
        errcode_ret
    )
}

extern "C" fn cl_compile_program(
    program: cl_program,
    num_devices: cl_uint,
    device_list: *const cl_device_id,
    options: *const ::std::os::raw::c_char,
    num_input_headers: cl_uint,
    input_headers: *const cl_program,
    header_include_names: *mut *const ::std::os::raw::c_char,
    pfn_notify: Option<ProgramCB>,
    user_data: *mut ::std::os::raw::c_void,
) -> cl_int {
    match_err!(compile_program(
        program,
        num_devices,
        device_list,
        options,
        num_input_headers,
        input_headers,
        header_include_names,
        pfn_notify,
        user_data,
    ))
}

extern "C" fn cl_link_program(
    context: cl_context,
    num_devices: cl_uint,
    device_list: *const cl_device_id,
    options: *const ::std::os::raw::c_char,
    num_input_programs: cl_uint,
    input_programs: *const cl_program,
    pfn_notify: Option<ProgramCB>,
    user_data: *mut ::std::os::raw::c_void,
    errcode_ret: *mut cl_int,
) -> cl_program {
    match_obj_expl!(
        link_program(
            context,
            num_devices,
            device_list,
            options,
            num_input_programs,
            input_programs,
            pfn_notify,
            user_data,
        ),
        errcode_ret
    )
}

extern "C" fn cl_unload_platform_compiler(_platform: cl_platform_id) -> cl_int {
    println!("cl_unload_platform_compiler not implemented");
    CL_OUT_OF_HOST_MEMORY
}

extern "C" fn cl_get_kernel_arg_info(
    kernel: cl_kernel,
    arg_indx: cl_uint,
    param_name: cl_kernel_arg_info,
    param_value_size: usize,
    param_value: *mut ::std::os::raw::c_void,
    param_value_size_ret: *mut usize,
) -> cl_int {
    match_err!(kernel.get_info_obj(
        arg_indx,
        param_name,
        param_value_size,
        param_value,
        param_value_size_ret,
    ))
}

extern "C" fn cl_enqueue_fill_image(
    _command_queue: cl_command_queue,
    _image: cl_mem,
    _fill_color: *const ::std::os::raw::c_void,
    _origin: *const [usize; 3usize],
    _region: *const [usize; 3usize],
    _num_events_in_wait_list: cl_uint,
    _event_wait_list: *const cl_event,
    _event: *mut cl_event,
) -> cl_int {
    println!("cl_enqueue_fill_image not implemented");
    CL_OUT_OF_HOST_MEMORY
}

extern "C" fn cl_create_command_queue_with_properties(
    context: cl_context,
    device: cl_device_id,
    _arg3: *const cl_queue_properties,
    errcode_ret: *mut cl_int,
) -> cl_command_queue {
    // TODO use own impl, this is enough to run the CL 3.0 CTS
    match_obj!(create_command_queue(context, device, 0), errcode_ret)
}

// cl_khr_icd
extern "C" fn cl_icd_get_platform_ids_khr(
    num_entries: cl_uint,
    platforms: *mut cl_platform_id,
    num_platforms: *mut cl_uint,
) -> cl_int {
    match_err!(get_platform_ids(num_entries, platforms, num_platforms))
}
