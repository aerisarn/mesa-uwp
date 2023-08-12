use crate::api::icd::*;

use libc_rust_gen::dlsym;
use rusticl_opencl_gen::*;

use std::ffi::CString;
use std::mem;
use std::os::raw::c_void;
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

#[allow(clippy::upper_case_acronyms)]
#[derive(PartialEq, Eq)]
enum GLCtx {
    EGL(EGLDisplay, EGLContext),
    GLX(*mut _XDisplay, *mut __GLXcontextRec),
}

pub struct GLCtxManager {
    pub interop_dev_info: mesa_glinterop_device_info,
    pub xplat_manager: XPlatManager,
    gl_ctx: GLCtx,
}

impl GLCtxManager {
    pub fn new(
        gl_context: *mut c_void,
        glx_display: *mut _XDisplay,
        egl_display: EGLDisplay,
    ) -> CLResult<Option<Self>> {
        let mut info = mesa_glinterop_device_info {
            version: 3,
            ..Default::default()
        };
        let xplat_manager = XPlatManager::new();

        // More than one of the attributes CL_CGL_SHAREGROUP_KHR, CL_EGL_DISPLAY_KHR,
        // CL_GLX_DISPLAY_KHR, and CL_WGL_HDC_KHR is set to a non-default value.
        if !egl_display.is_null() && !glx_display.is_null() {
            return Err(CL_INVALID_OPERATION);
        }

        if gl_context.is_null() {
            return Ok(None);
        }

        if !egl_display.is_null() {
            let egl_query_device_info_func = xplat_manager
                .MesaGLInteropEGLQueryDeviceInfo()?
                .ok_or(CL_INVALID_GL_SHAREGROUP_REFERENCE_KHR)?;

            let err = unsafe {
                egl_query_device_info_func(egl_display.cast(), gl_context.cast(), &mut info)
            };

            if err != MESA_GLINTEROP_SUCCESS as i32 {
                return Err(interop_to_cl_error(err));
            }

            Ok(Some(GLCtxManager {
                gl_ctx: GLCtx::EGL(egl_display.cast(), gl_context),
                interop_dev_info: info,
                xplat_manager: xplat_manager,
            }))
        } else if !glx_display.is_null() {
            let glx_query_device_info_func = xplat_manager
                .MesaGLInteropGLXQueryDeviceInfo()?
                .ok_or(CL_INVALID_GL_SHAREGROUP_REFERENCE_KHR)?;

            let err = unsafe {
                glx_query_device_info_func(glx_display.cast(), gl_context.cast(), &mut info)
            };

            if err != MESA_GLINTEROP_SUCCESS as i32 {
                return Err(interop_to_cl_error(err));
            }

            Ok(Some(GLCtxManager {
                gl_ctx: GLCtx::GLX(glx_display.cast(), gl_context.cast()),
                interop_dev_info: info,
                xplat_manager: xplat_manager,
            }))
        } else {
            Err(CL_INVALID_GL_SHAREGROUP_REFERENCE_KHR)
        }
    }
}

pub fn interop_to_cl_error(error: i32) -> CLError {
    match error.try_into().unwrap() {
        MESA_GLINTEROP_OUT_OF_RESOURCES => CL_OUT_OF_RESOURCES,
        MESA_GLINTEROP_OUT_OF_HOST_MEMORY => CL_OUT_OF_HOST_MEMORY,
        MESA_GLINTEROP_INVALID_OPERATION => CL_INVALID_OPERATION,
        MESA_GLINTEROP_INVALID_CONTEXT | MESA_GLINTEROP_INVALID_DISPLAY => {
            CL_INVALID_GL_SHAREGROUP_REFERENCE_KHR
        }
        MESA_GLINTEROP_INVALID_TARGET | MESA_GLINTEROP_INVALID_OBJECT => CL_INVALID_GL_OBJECT,
        MESA_GLINTEROP_INVALID_MIP_LEVEL => CL_INVALID_MIP_LEVEL,
        _ => CL_OUT_OF_HOST_MEMORY,
    }
}
