use crate::api::icd::*;

use libc_rust_gen::dlsym;
use rusticl_opencl_gen::*;

use std::ffi::CString;
use std::mem;
use std::ptr;

pub struct XPlatManager {
    glx_get_proc_addr: PFNGLXGETPROCADDRESSPROC,
    egl_get_proc_addr: PFNEGLGETPROCADDRESSPROC,
}

impl Default for XPlatManager {
    fn default() -> Self {
        Self::new()
    }
}

impl XPlatManager {
    pub fn new() -> Self {
        Self {
            glx_get_proc_addr: Self::get_proc_address_func("glXGetProcAddress"),
            egl_get_proc_addr: Self::get_proc_address_func("eglGetProcAddress"),
        }
    }

    fn get_proc_address_func<T>(name: &str) -> T {
        let cname = CString::new(name).unwrap();
        unsafe {
            let pfn = dlsym(ptr::null_mut(), cname.as_ptr());
            mem::transmute_copy(&pfn)
        }
    }

    fn get_func<T>(&self, name: &str) -> CLResult<T> {
        let cname = CString::new(name).unwrap();
        unsafe {
            let raw_func = if name.starts_with("glX") {
                self.glx_get_proc_addr
                    .ok_or(CL_INVALID_GL_SHAREGROUP_REFERENCE_KHR)?(
                    cname.as_ptr().cast()
                )
            } else if name.starts_with("egl") {
                self.egl_get_proc_addr
                    .ok_or(CL_INVALID_GL_SHAREGROUP_REFERENCE_KHR)?(
                    cname.as_ptr().cast()
                )
            } else {
                panic!();
            };

            Ok(mem::transmute_copy(&raw_func))
        }
    }

    #[allow(non_snake_case)]
    pub fn MesaGLInteropEGLQueryDeviceInfo(
        &self,
    ) -> CLResult<PFNMESAGLINTEROPEGLQUERYDEVICEINFOPROC> {
        self.get_func::<PFNMESAGLINTEROPEGLQUERYDEVICEINFOPROC>("eglGLInteropQueryDeviceInfoMESA")
    }

    #[allow(non_snake_case)]
    pub fn MesaGLInteropGLXQueryDeviceInfo(
        &self,
    ) -> CLResult<PFNMESAGLINTEROPGLXQUERYDEVICEINFOPROC> {
        self.get_func::<PFNMESAGLINTEROPGLXQUERYDEVICEINFOPROC>("glXGLInteropQueryDeviceInfoMESA")
    }

    #[allow(non_snake_case)]
    pub fn MesaGLInteropEGLExportObject(&self) -> CLResult<PFNMESAGLINTEROPEGLEXPORTOBJECTPROC> {
        self.get_func::<PFNMESAGLINTEROPEGLEXPORTOBJECTPROC>("eglGLInteropExportObjectMESA")
    }

    #[allow(non_snake_case)]
    pub fn MesaGLInteropGLXExportObject(&self) -> CLResult<PFNMESAGLINTEROPGLXEXPORTOBJECTPROC> {
        self.get_func::<PFNMESAGLINTEROPGLXEXPORTOBJECTPROC>("glXGLInteropExportObjectMESA")
    }

    #[allow(non_snake_case)]
    pub fn MesaGLInteropEGLFlushObjects(&self) -> CLResult<PFNMESAGLINTEROPEGLFLUSHOBJECTSPROC> {
        self.get_func::<PFNMESAGLINTEROPEGLFLUSHOBJECTSPROC>("eglGLInteropFlushObjectsMESA")
    }

    #[allow(non_snake_case)]
    pub fn MesaGLInteropGLXFlushObjects(&self) -> CLResult<PFNMESAGLINTEROPGLXFLUSHOBJECTSPROC> {
        self.get_func::<PFNMESAGLINTEROPGLXFLUSHOBJECTSPROC>("glXGLInteropFlushObjectsMESA")
    }
}
