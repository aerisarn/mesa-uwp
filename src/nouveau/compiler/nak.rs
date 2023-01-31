/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

mod bitset;
mod nak_assign_regs;
mod nak_encode_tu102;
mod nak_from_nir;
mod nak_ir;
mod nak_opt_copy_prop;
mod nak_opt_dce;
mod nir;
mod util;

use nak_bindings::*;
use nak_from_nir::*;
use std::os::raw::c_void;

#[repr(C)]
struct ShaderBin {
    bin: nak_shader_bin,
    code: Vec<u32>,
}

impl ShaderBin {
    pub fn new(info: nak_shader_info, code: Vec<u32>) -> ShaderBin {
        let bin = nak_shader_bin {
            info: info,
            code_size: (code.len() * 4).try_into().unwrap(),
            code: code.as_ptr() as *const c_void,
        };
        ShaderBin {
            bin: bin,
            code: code,
        }
    }
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

    let info = nak_shader_info {
        num_gprs: 255,
        tls_size: 0,
        cs: nak_shader_info__bindgen_ty_1 {
            local_size: [0; 3],
            smem_size: 0,
        },
        hdr: [0; 32],
    };

    let code = nak_encode_tu102::encode_shader(&s);

    print!("Encoded shader:");
    for i in 0..code.len() {
        if (i % 8) == 0 {
            print!("\n  ");
        }
        print!("  {:#x}", code[i]);
    }
    print!("\n\n");

    Box::into_raw(Box::new(ShaderBin::new(info, code))) as *mut nak_shader_bin
}
