/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use nak_bindings::*;

#[repr(C)]
struct ShaderBin {
    bin: nak_shader_bin,
    instrs: Vec<u32>,
}

#[no_mangle]
pub extern "C" fn nak_shader_bin_destroy(bin: *mut nak_shader_bin) {
    unsafe {
        Box::from_raw(bin as *mut ShaderBin);
    };
}

#[no_mangle]
pub extern "C" fn nak_compile_shader(
    _nir: *mut nir_shader,
    _nak: *const nak_compiler,
) -> *mut nak_shader_bin {
    println!("Hello from rust!");
    std::ptr::null_mut()
}
