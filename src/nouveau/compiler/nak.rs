/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

mod nak_assign_regs;
mod nak_from_nir;
mod nak_ir;
mod nak_opt_copy_prop;
mod nak_opt_dce;
mod nir;
mod util;

use nak_bindings::*;
use nak_from_nir::*;

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
    nir: *mut nir_shader,
    nak: *const nak_compiler,
) -> *mut nak_shader_bin {
    unsafe { nak_postprocess_nir(nir, nak) };
    let nir = unsafe { &*nir };

    let mut s = nak_shader_from_nir(nir);

    println!("NAK IR:\n{}", &s);

    s.opt_copy_prop();

    println!("NAK IR:\n{}", &s);

    s.opt_dce();

    println!("NAK IR:\n{}", &s);

    s.assign_regs_trivial();
    s.lower_vec_split();
    s.lower_zero_to_gpr255();

    println!("NAK IR:\n{}", &s);

    std::ptr::null_mut()
}
