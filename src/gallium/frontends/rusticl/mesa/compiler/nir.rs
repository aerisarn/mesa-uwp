extern crate mesa_rust_gen;

use self::mesa_rust_gen::*;

use std::ptr::NonNull;

pub struct NirShader {
    nir: NonNull<nir_shader>,
}

impl Drop for NirShader {
    fn drop(&mut self) {
        unsafe { ralloc_free(self.nir.as_ptr().cast()) };
    }
}
