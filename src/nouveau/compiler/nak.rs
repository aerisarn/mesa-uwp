/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

mod bitset;
mod bitview;
mod nak_assign_regs;
mod nak_builder;
mod nak_calc_instr_deps;
mod nak_cfg;
mod nak_encode_sm75;
mod nak_from_nir;
mod nak_ir;
mod nak_legalize;
mod nak_liveness;
mod nak_lower_copy_swap;
mod nak_lower_par_copies;
mod nak_opt_copy_prop;
mod nak_opt_dce;
mod nak_opt_lop;
mod nak_repair_ssa;
mod nak_spill_values;
mod nak_to_cssa;
mod nir;
mod util;

use crate::nak_ir::{ShaderInfo, ShaderStageInfo};
use crate::nir::NirShader;

use bitview::*;
use nak_bindings::*;
use nak_from_nir::*;
use std::env;
use std::ffi::CStr;
use std::os::raw::c_void;
use std::sync::OnceLock;
use util::NextMultipleOf;

#[repr(u8)]
enum DebugFlags {
    Print,
    Serial,
    Spill,
}

struct Debug {
    flags: u32,
}

impl Debug {
    fn new() -> Debug {
        let debug_var = "NAK_DEBUG";
        let debug_str = match env::var(debug_var) {
            Ok(s) => s,
            Err(_) => {
                return Debug { flags: 0 };
            }
        };

        let mut flags = 0;
        for flag in debug_str.split(',') {
            match flag.trim() {
                "print" => flags |= 1 << DebugFlags::Print as u8,
                "serial" => flags |= 1 << DebugFlags::Serial as u8,
                "spill" => flags |= 1 << DebugFlags::Spill as u8,
                unk => eprintln!("Unknown NAK_DEBUG flag \"{}\"", unk),
            }
        }
        Debug { flags: flags }
    }
}

trait GetDebugFlags {
    fn debug_flags(&self) -> u32;

    fn print(&self) -> bool {
        self.debug_flags() & (1 << DebugFlags::Print as u8) != 0
    }

    fn serial(&self) -> bool {
        self.debug_flags() & (1 << DebugFlags::Serial as u8) != 0
    }

    fn spill(&self) -> bool {
        self.debug_flags() & (1 << DebugFlags::Spill as u8) != 0
    }
}

static DEBUG: OnceLock<Debug> = OnceLock::new();

impl GetDebugFlags for OnceLock<Debug> {
    fn debug_flags(&self) -> u32 {
        self.get().unwrap().flags
    }
}

#[no_mangle]
pub extern "C" fn nak_should_print_nir() -> bool {
    DEBUG.print()
}

fn nir_options(_dev: &nv_device_info) -> nir_shader_compiler_options {
    let mut op: nir_shader_compiler_options = unsafe { std::mem::zeroed() };

    op.lower_fdiv = true;
    op.lower_flrp16 = true;
    op.lower_flrp32 = true;
    op.lower_flrp64 = true;
    op.lower_bitfield_extract = true;
    op.lower_bitfield_insert = true;
    op.lower_pack_half_2x16 = true;
    op.lower_pack_unorm_2x16 = true;
    op.lower_pack_snorm_2x16 = true;
    op.lower_pack_unorm_4x8 = true;
    op.lower_pack_snorm_4x8 = true;
    op.lower_unpack_half_2x16 = true;
    op.lower_unpack_unorm_2x16 = true;
    op.lower_unpack_snorm_2x16 = true;
    op.lower_unpack_unorm_4x8 = true;
    op.lower_unpack_snorm_4x8 = true;
    op.lower_extract_byte = true;
    op.lower_extract_word = true;
    op.lower_insert_byte = true;
    op.lower_insert_word = true;
    op.lower_cs_local_index_to_id = true;
    op.lower_device_index_to_zero = true;
    op.lower_uadd_sat = true; // TODO
    op.lower_usub_sat = true; // TODO
    op.lower_iadd_sat = true; // TODO
    op.use_interpolated_input_intrinsics = true;
    op.lower_int64_options = !nir_lower_iadd64;
    op.lower_ldexp = true;
    op.lower_fmod = true;
    op.lower_ffract = true;
    op.lower_fpow = true;
    op.lower_scmp = true;
    op.lower_uadd_carry = true;
    op.lower_usub_borrow = true;

    op
}

#[no_mangle]
pub extern "C" fn nak_compiler_create(
    dev: *const nv_device_info,
) -> *mut nak_compiler {
    assert!(!dev.is_null());
    let dev = unsafe { &*dev };

    DEBUG.get_or_init(|| Debug::new());

    let nak = Box::new(nak_compiler {
        sm: dev.sm,
        nir_options: nir_options(dev),
    });

    Box::into_raw(nak)
}

#[no_mangle]
pub extern "C" fn nak_compiler_destroy(nak: *mut nak_compiler) {
    unsafe { Box::from_raw(nak) };
}

#[no_mangle]
pub extern "C" fn nak_debug_flags(nak: *const nak_compiler) -> u64 {
    DEBUG.debug_flags().into()
}

#[no_mangle]
pub extern "C" fn nak_nir_options(
    nak: *const nak_compiler,
) -> *const nir_shader_compiler_options {
    assert!(!nak.is_null());
    let nak = unsafe { &*nak };
    &nak.nir_options
}

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
        _ = Box::from_raw(bin as *mut ShaderBin);
    };
}

fn encode_hdr_for_nir(
    nir: &nir_shader,
    shader_info: &ShaderInfo,
    fs_key: Option<&nak_fs_key>,
) -> [u32; 32] {
    if nir.info.stage() == MESA_SHADER_COMPUTE {
        return [0_u32; 32];
    }

    let mut hdr = [0_u32; 32];
    let mut hdr_view = BitMutView::new(&mut hdr);

    /* [0, 31]: CommonWord0 */
    let mut cw0 = hdr_view.subset_mut(0..32);
    let sph_type = if nir.info.stage() == MESA_SHADER_FRAGMENT {
        2_u32 /* SPH_TYPE_01_PS */
    } else {
        1_u32 /* SPH_TYPE_01_VTG */
    };
    cw0.set_field(0..5, sph_type);
    cw0.set_field(5..10, 3_u32);
    let shader_type = match nir.info.stage() {
        MESA_SHADER_VERTEX => 1_u32,
        MESA_SHADER_TESS_CTRL => 2_u32,
        MESA_SHADER_TESS_EVAL => 3_u32,
        MESA_SHADER_GEOMETRY => 4_u32,
        MESA_SHADER_FRAGMENT => 5_u32,
        _ => panic!("Unknown shader stage"),
    };
    cw0.set_field(10..14, shader_type);
    if let ShaderStageInfo::Fragment(fs_info) = &shader_info.stage {
        cw0.set_bit(14, fs_info.writes_color > 0xf);
        let info_fs = unsafe { &nir.info.__bindgen_anon_1.fs };
        let zs_self_dep = fs_key.map_or(false, |key| key.zs_self_dep);
        cw0.set_bit(15, info_fs.uses_discard() || zs_self_dep);
    }
    cw0.set_bit(16, false /* TODO: DoesGlobalStore */);
    cw0.set_field(17..21, 1_u32 /* SassVersion */);
    cw0.set_field(21..26, 0_u32 /* Reserved */);
    cw0.set_bit(26, false /* TODO: DoesLoadOrStore */);
    cw0.set_bit(27, false /* TODO: DoesFp64 */);
    cw0.set_field(28..32, 0_u32 /* StreamOutMask */);

    /* [32, 63]: CommonWord1 */
    let mut cw1 = hdr_view.subset_mut(32..64);
    let tls_size_aligned =
        NextMultipleOf::next_multiple_of(&shader_info.tls_size, 0x10);
    cw1.set_field(0..24, tls_size_aligned);
    if nir.info.stage() == MESA_SHADER_TESS_CTRL {
        cw1.set_field(24..32, 0_u32 /* TODO: PerPatchAttributeCount */);
    }

    /* [64, 95]: CommonWord2 */
    let mut cw2 = hdr_view.subset_mut(64..96);
    cw2.set_field(0..24, 0_u32 /* ShaderLocalMemoryHighSize */);
    cw2.set_field(24..32, 0_u32 /* TODO: ThreadsPerInputPrimitive */);

    /* [96, 127]: CommonWord3 */
    let mut cw3 = hdr_view.subset_mut(96..128);
    cw3.set_field(0..24, 0_u32 /* ShaderLocalMemoryCrsSize */);
    if nir.info.stage() == MESA_SHADER_GEOMETRY {
        let info_gs = unsafe { &nir.info.__bindgen_anon_1.gs };
        let output_topology = match info_gs.output_primitive {
            MESA_PRIM_POINTS => 1_u8,    /* POINTLIST */
            MESA_PRIM_LINES => 6_u8,     /* LINESTRIP */
            MESA_PRIM_TRIANGLES => 6_u8, /* TRIANGLESTRIP */
            _ => panic!("Invalid geometry output primitive type"),
        };
        cw3.set_field(24..28, output_topology);
    }
    cw3.set_field(28..32, 0_u32 /* Reserved */);

    /* [128, 159]: CommonWord4 */
    let mut cw4 = hdr_view.subset_mut(128..160);
    if nir.info.stage() == MESA_SHADER_GEOMETRY {
        let info_gs = unsafe { &nir.info.__bindgen_anon_1.gs };
        cw4.set_field(0..12, info_gs.vertices_out);
    }
    /* TODO */
    if nir.info.stage() == MESA_SHADER_VERTEX {
        cw4.set_field(12..20, 0xff_u32 /* TODO: StoreReqStart */);
    }
    cw4.set_field(20..24, 0_u32 /* Reserved */);
    cw4.set_field(24..32, 0_u32 /* TODO: StoreReqEnd */);

    let nir_sv = BitView::new(&nir.info.system_values_read);

    if nir.info.stage() != MESA_SHADER_FRAGMENT {
        assert!(sph_type == 1);

        /* [160, 183]: ImapSystemValuesA */
        /* [184, 191]: ImapSystemValuesB */
        let mut imap_sv = hdr_view.subset_mut(160..192);
        if nir.info.stage() == MESA_SHADER_TESS_CTRL { /* TODO */ }
        let has_primitive_id =
            nir_sv.get_bit(SYSTEM_VALUE_PRIMITIVE_ID.try_into().unwrap());
        imap_sv.set_bit(24, has_primitive_id);
        imap_sv.set_bit(25, false /* TODO: RtArrayIndex */);
        imap_sv.set_bit(26, false /* TODO: ViewportIndex */);
        imap_sv.set_bit(27, false /* TODO: PointSize */);

        /* [192, 319]: ImapGenericVector[32] */
        let mut imap_g = hdr_view.subset_mut(192..320);

        let nir_ir = BitView::new(&nir.info.inputs_read);
        let input0: usize = if nir.info.stage() == MESA_SHADER_VERTEX {
            VERT_ATTRIB_GENERIC0.try_into().unwrap()
        } else {
            VARYING_SLOT_VAR0.try_into().unwrap()
        };
        for i in 0..32 {
            if nir_ir.get_bit(input0 + i) {
                imap_g.set_field((i * 4)..(i * 4 + 4), 0xf_u32);
            }
        }

        /* [320, 335]: ImapColor */
        /* [336, 352]: ImapSystemValuesC */
        /* [352, 391]: ImapFixedFncTexture[10] */
        /* [392, 399]: ImapReserved */

        let nir_ow = BitView::new(&nir.info.outputs_written);

        /* [400, 423]: TODO: OmapSystemValuesA */

        /* [424, 431]: OmapSystemValuesB */
        let mut omap_sv_b = hdr_view.subset_mut(424..432);
        omap_sv_b.set_bit(0, false /* TODO: PrimitiveId */);
        omap_sv_b.set_bit(1, false /* TODO: RtAttayIndex */);
        let has_viewport =
            nir_ow.get_bit(VARYING_SLOT_VIEWPORT.try_into().unwrap());
        omap_sv_b.set_bit(2, has_viewport);
        let has_point_size =
            nir_ow.get_bit(VARYING_SLOT_PSIZ.try_into().unwrap());
        omap_sv_b.set_bit(3, has_point_size);
        if nir_ow.get_bit(VARYING_SLOT_POS.try_into().unwrap()) {
            omap_sv_b.set_field(4..8, 0xf_u32);
        }

        /* [432, 559]: OmapGenericVector[32] */
        let mut omap_g = hdr_view.subset_mut(432..560);
        let output0 = usize::try_from(VARYING_SLOT_VAR0).unwrap();
        for i in 0..32 {
            if nir_ow.get_bit(output0 + i) {
                omap_g.set_field((i * 4)..(i * 4 + 4), 0xf_u32);
            }
        }

        /* [560, 575]: OmapColor */
        /* [576, 591]: OmapSystemValuesC */
        /* [592, 631]: OmapFixedFncTexture[10] */
        /* [632, 639]: OmapReserved */
    } else {
        assert!(nir.info.stage() == MESA_SHADER_FRAGMENT);
        assert!(sph_type == 2);

        /* [160, 183]: ImapSystemValuesA */
        /* [184, 191]: ImapSystemValuesB */
        let mut imap_sv = hdr_view.subset_mut(160..192);
        let has_frag_coord =
            nir_sv.get_bit(SYSTEM_VALUE_FRAG_COORD.try_into().unwrap());
        imap_sv.set_bit(28, has_frag_coord);
        imap_sv.set_bit(29, has_frag_coord);
        imap_sv.set_bit(30, has_frag_coord);
        imap_sv.set_bit(31, true); //has_frag_coord);

        /* [192, 447]: ImapGenericVector[32] */
        /* TODO: Non-perspective */
        let mut imap_g = hdr_view.subset_mut(192..447);

        for var in nir.iter_variables() {
            if var.data.mode() != nir_var_shader_in {
                continue;
            }
            let loc_u32 = u32::try_from(var.data.location).unwrap();
            let slot =
                (loc_u32 - VARYING_SLOT_VAR0) * 4 + var.data.location_frac();
            let num_slots =
                unsafe { glsl_count_attribute_slots(var.type_, false) * 4 };
            let mode: u8 = match var.data.interpolation() {
                INTERP_MODE_NONE | INTERP_MODE_SMOOTH => 2, /* Perspective */
                INTERP_MODE_FLAT => 1,                      /* Constant */
                INTERP_MODE_NOPERSPECTIVE => 3,             /* ScreenLinear */
                INTERP_MODE_EXPLICIT => 0,                  /* Unused */
                _ => panic!("Unsupported INTERP_MODE"),
            };
            for s in slot..(slot + num_slots) {
                let s = usize::try_from(s).unwrap();
                imap_g.set_field((s * 2)..(s * 2 + 2), mode);
            }
        }

        /* [448, 463]: ImapColor */
        /* [464, 479]: ImapSystemValuesC */
        /* [480, 559]: ImapFixedFncTexture[10] */
        /* [560, 575]: ImapReserved */

        let ShaderStageInfo::Fragment(fs_info) = &shader_info.stage else {
            panic!("Not a fragment shader");
        };

        /* [576, 607]: OmapTarget[8] */
        hdr_view.set_field(576..608, fs_info.writes_color);

        /* [608]: OmapSampleMask */
        hdr_view.set_bit(608, fs_info.writes_sample_mask);

        /* [609]: OmapDepth */
        hdr_view.set_bit(609, fs_info.writes_depth);

        /* [610, 639]: Reserved */
    }

    hdr
}

fn eprint_hex(label: &str, data: &[u32]) {
    eprint!("{}:", label);
    for i in 0..data.len() {
        if (i % 8) == 0 {
            eprintln!("");
            eprint!(" ");
        }
        eprint!(" {:08x}", data[i]);
    }
    eprintln!("");
}

#[no_mangle]
pub extern "C" fn nak_compile_shader(
    nir: *mut nir_shader,
    nak: *const nak_compiler,
    fs_key: *const nak_fs_key,
) -> *mut nak_shader_bin {
    unsafe { nak_postprocess_nir(nir, nak) };
    let nak = unsafe { &*nak };
    let nir = unsafe { &*nir };
    let fs_key = if fs_key.is_null() {
        None
    } else {
        Some(unsafe { &*fs_key })
    };

    let mut s = nak_shader_from_nir(nir, nak.sm);

    if DEBUG.print() {
        eprintln!("NAK IR:\n{}", &s);
    }

    s.opt_copy_prop();
    if DEBUG.print() {
        eprintln!("NAK IR:\n{}", &s);
    }

    s.opt_lop();
    if DEBUG.print() {
        eprintln!("NAK IR:\n{}", &s);
    }

    s.opt_dce();
    if DEBUG.print() {
        eprintln!("NAK IR:\n{}", &s);
    }

    s.legalize();
    if DEBUG.print() {
        eprintln!("NAK IR:\n{}", &s);
    }

    s.assign_regs();
    if DEBUG.print() {
        eprintln!("NAK IR:\n{}", &s);
    }

    s.lower_vec_split();
    s.lower_par_copies();
    s.lower_copy_swap();
    s.calc_instr_deps();

    if DEBUG.print() {
        eprintln!("NAK IR:\n{}", &s);
    }

    let info = nak_shader_info {
        stage: nir.info.stage(),
        num_gprs: s.info.num_gprs,
        num_barriers: if nir.info.uses_control_barrier() {
            1
        } else {
            0
        },
        tls_size: s.info.tls_size,
        cs: nak_shader_info__bindgen_ty_1 {
            local_size: [
                nir.info.workgroup_size[0].into(),
                nir.info.workgroup_size[1].into(),
                nir.info.workgroup_size[2].into(),
            ],
            smem_size: nir.info.shared_size.try_into().unwrap(),
        },
        hdr: encode_hdr_for_nir(nir, &s.info, fs_key),
    };

    let code = if nak.sm >= 75 {
        nak_encode_sm75::encode_shader(&s)
    } else {
        panic!("Unsupported shader model");
    };

    if DEBUG.print() {
        let stage_name = unsafe {
            let c_name = _mesa_shader_stage_to_string(info.stage as u32);
            CStr::from_ptr(c_name).to_str().expect("Invalid UTF-8")
        };
        eprintln!("Stage: {}", stage_name);
        eprintln!("Num GPRs: {}", info.num_gprs);
        eprintln!("TLS size: {}", info.tls_size);
        if info.stage == MESA_SHADER_COMPUTE {
            eprintln!(
                "Local size: {}x{}x{}",
                info.cs.local_size[0],
                info.cs.local_size[1],
                info.cs.local_size[2],
            );
            eprintln!("Shared memory size: {:#x}", info.cs.smem_size);
        }

        if info.stage != MESA_SHADER_COMPUTE {
            eprint_hex("Header", &info.hdr);
        }

        eprint_hex("Encoded shader", &code);
    }

    Box::into_raw(Box::new(ShaderBin::new(info, code))) as *mut nak_shader_bin
}
