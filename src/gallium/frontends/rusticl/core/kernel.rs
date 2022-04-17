use crate::api::icd::*;
use crate::api::util::cl_prop;
use crate::core::device::*;
use crate::core::event::*;
use crate::core::format::*;
use crate::core::memory::*;
use crate::core::program::*;
use crate::core::queue::*;
use crate::impl_cl_type_trait;

use mesa_rust::compiler::clc::*;
use mesa_rust::compiler::nir::*;
use mesa_rust_gen::*;
use rusticl_opencl_gen::*;

use std::cell::RefCell;
use std::collections::HashMap;
use std::collections::HashSet;
use std::convert::TryInto;
use std::os::raw::c_void;
use std::ptr;
use std::slice;
use std::sync::Arc;

// ugh, we are not allowed to take refs, so...
#[derive(Clone)]
pub enum KernelArgValue {
    None,
    Constant(Vec<u8>),
    MemObject(&'static Mem),
    Sampler(&'static Sampler),
    LocalMem(usize),
}

#[derive(Hash, PartialEq, Eq, Clone)]
pub enum KernelArgType {
    Constant, // for anything passed by value
    Image,
    Sampler,
    Texture,
    MemGlobal,
    MemConstant,
    MemLocal,
}

#[derive(Hash, PartialEq, Eq, Clone)]
pub enum InternalKernelArgType {
    ConstantBuffer,
    GlobalWorkOffsets,
    PrintfBuffer,
    InlineSampler((cl_addressing_mode, cl_filter_mode, bool)),
    FormatArray,
    OrderArray,
}

#[derive(Hash, PartialEq, Eq, Clone)]
pub struct KernelArg {
    spirv: spirv::SPIRVKernelArg,
    pub kind: KernelArgType,
    pub size: usize,
    pub offset: usize,
    pub dead: bool,
}

#[derive(Hash, PartialEq, Eq, Clone)]
pub struct InternalKernelArg {
    pub kind: InternalKernelArgType,
    pub size: usize,
    pub offset: usize,
}

impl KernelArg {
    fn from_spirv_nir(spirv: &[spirv::SPIRVKernelArg], nir: &mut NirShader) -> Vec<Self> {
        let nir_arg_map: HashMap<_, _> = nir
            .variables_with_mode(
                nir_variable_mode::nir_var_uniform | nir_variable_mode::nir_var_image,
            )
            .map(|v| (v.data.location, v))
            .collect();
        let mut res = Vec::new();

        for (i, s) in spirv.iter().enumerate() {
            let nir = nir_arg_map.get(&(i as i32)).unwrap();
            let kind = match s.address_qualifier {
                clc_kernel_arg_address_qualifier::CLC_KERNEL_ARG_ADDRESS_PRIVATE => {
                    if unsafe { glsl_type_is_sampler(nir.type_) } {
                        KernelArgType::Sampler
                    } else {
                        KernelArgType::Constant
                    }
                }
                clc_kernel_arg_address_qualifier::CLC_KERNEL_ARG_ADDRESS_CONSTANT => {
                    KernelArgType::MemConstant
                }
                clc_kernel_arg_address_qualifier::CLC_KERNEL_ARG_ADDRESS_LOCAL => {
                    KernelArgType::MemLocal
                }
                clc_kernel_arg_address_qualifier::CLC_KERNEL_ARG_ADDRESS_GLOBAL => {
                    if unsafe { glsl_type_is_image(nir.type_) } {
                        if nir.data.access() == gl_access_qualifier::ACCESS_NON_WRITEABLE.0 {
                            KernelArgType::Texture
                        } else {
                            KernelArgType::Image
                        }
                    } else {
                        KernelArgType::MemGlobal
                    }
                }
            };

            res.push(Self {
                spirv: s.clone(),
                size: unsafe { glsl_get_cl_size(nir.type_) } as usize,
                // we'll update it later in the 2nd pass
                kind: kind,
                offset: 0,
                dead: true,
            });
        }
        res
    }

    fn assign_locations(
        args: &mut [Self],
        internal_args: &mut [InternalKernelArg],
        nir: &mut NirShader,
    ) {
        for var in nir.variables_with_mode(
            nir_variable_mode::nir_var_uniform | nir_variable_mode::nir_var_image,
        ) {
            if let Some(arg) = args.get_mut(var.data.location as usize) {
                arg.offset = var.data.driver_location as usize;
                arg.dead = false;
            } else {
                internal_args
                    .get_mut(var.data.location as usize - args.len())
                    .unwrap()
                    .offset = var.data.driver_location as usize;
            }
        }
    }
}

#[repr(C)]
pub struct Kernel {
    pub base: CLObjectBase<CL_INVALID_KERNEL>,
    pub prog: Arc<Program>,
    pub name: String,
    pub args: Vec<KernelArg>,
    pub values: Vec<RefCell<Option<KernelArgValue>>>,
    pub work_group_size: [usize; 3],
    internal_args: Vec<InternalKernelArg>,
    nirs: HashMap<Arc<Device>, NirShader>,
}

impl_cl_type_trait!(cl_kernel, Kernel, CL_INVALID_KERNEL);

fn create_kernel_arr<T>(vals: &[usize], val: T) -> [T; 3]
where
    T: std::convert::TryFrom<usize> + Copy,
    <T as std::convert::TryFrom<usize>>::Error: std::fmt::Debug,
{
    let mut res = [val; 3];
    for (i, v) in vals.iter().enumerate() {
        res[i] = (*v).try_into().expect("64 bit work groups not supported");
    }
    res
}

fn opt_nir(nir: &mut NirShader, dev: &Device) {
    let nir_options = unsafe {
        &*dev
            .screen
            .nir_shader_compiler_options(pipe_shader_type::PIPE_SHADER_COMPUTE)
    };

    while {
        let mut progress = false;

        progress |= nir.pass0(nir_copy_prop);
        progress |= nir.pass0(nir_opt_copy_prop_vars);
        progress |= nir.pass0(nir_opt_dead_write_vars);

        if nir_options.lower_to_scalar {
            nir.pass2(
                nir_lower_alu_to_scalar,
                nir_options.lower_to_scalar_filter,
                ptr::null(),
            );
            nir.pass1(nir_lower_phis_to_scalar, false);
        }

        progress |= nir.pass0(nir_opt_deref);
        progress |= nir.pass0(nir_opt_memcpy);
        progress |= nir.pass0(nir_opt_dce);
        progress |= nir.pass0(nir_opt_undef);
        progress |= nir.pass0(nir_opt_constant_folding);
        progress |= nir.pass0(nir_opt_cse);
        nir.pass0(nir_split_var_copies);
        progress |= nir.pass0(nir_lower_var_copies);
        progress |= nir.pass0(nir_lower_vars_to_ssa);
        nir.pass0(nir_lower_alu);
        nir.pass0(nir_lower_pack);
        progress |= nir.pass0(nir_opt_phi_precision);
        progress |= nir.pass0(nir_opt_algebraic);
        progress |= nir.pass1(
            nir_opt_if,
            nir_opt_if_options::nir_opt_if_aggressive_last_continue
                | nir_opt_if_options::nir_opt_if_optimize_phi_true_false,
        );
        progress |= nir.pass0(nir_opt_dead_cf);
        progress |= nir.pass0(nir_opt_remove_phis);
        // we don't want to be too aggressive here, but it kills a bit of CFG
        progress |= nir.pass3(nir_opt_peephole_select, 8, true, true);
        progress |= nir.pass1(
            nir_lower_vec3_to_vec4,
            nir_variable_mode::nir_var_mem_generic | nir_variable_mode::nir_var_uniform,
        );

        if nir_options.max_unroll_iterations != 0 {
            progress |= nir.pass0(nir_opt_loop_unroll);
        }
        nir.sweep_mem();
        progress
    } {}
}

fn lower_and_optimize_nir_pre_inputs(dev: &Device, nir: &mut NirShader, lib_clc: &NirShader) {
    nir.pass0(nir_scale_fdiv);
    nir.set_workgroup_size_variable_if_zero();
    nir.structurize();
    while {
        let mut progress = false;
        nir.pass0(nir_split_var_copies);
        progress |= nir.pass0(nir_copy_prop);
        progress |= nir.pass0(nir_opt_copy_prop_vars);
        progress |= nir.pass0(nir_opt_dead_write_vars);
        progress |= nir.pass0(nir_opt_deref);
        progress |= nir.pass0(nir_opt_dce);
        progress |= nir.pass0(nir_opt_undef);
        progress |= nir.pass0(nir_opt_constant_folding);
        progress |= nir.pass0(nir_opt_cse);
        progress |= nir.pass0(nir_lower_vars_to_ssa);
        progress |= nir.pass0(nir_opt_algebraic);
        progress
    } {}
    nir.inline(lib_clc);
    nir.remove_non_entrypoints();
    // that should free up tons of memory
    nir.sweep_mem();

    nir.pass0(nir_dedup_inline_samplers);
    nir.pass2(
        nir_lower_vars_to_explicit_types,
        nir_variable_mode::nir_var_uniform
            | nir_variable_mode::nir_var_function_temp
            | nir_variable_mode::nir_var_shader_temp
            | nir_variable_mode::nir_var_mem_shared
            | nir_variable_mode::nir_var_mem_generic
            | nir_variable_mode::nir_var_mem_global,
        Some(glsl_get_cl_type_size_align),
    );

    let mut printf_opts = nir_lower_printf_options::default();
    printf_opts.set_treat_doubles_as_floats(false);
    printf_opts.max_buffer_size = dev.printf_buffer_size() as u32;
    nir.pass1(nir_lower_printf, &printf_opts);

    opt_nir(nir, dev);
}

extern "C" fn can_remove_var(var: *mut nir_variable, _: *mut c_void) -> bool {
    unsafe {
        let var = var.as_ref().unwrap();
        !glsl_type_is_image(var.type_)
    }
}

fn lower_and_optimize_nir_late(
    dev: &Device,
    nir: &mut NirShader,
    args: usize,
) -> Vec<InternalKernelArg> {
    let mut res = Vec::new();
    let nir_options = unsafe {
        &*dev
            .screen
            .nir_shader_compiler_options(pipe_shader_type::PIPE_SHADER_COMPUTE)
    };
    let mut lower_state = rusticl_lower_state::default();

    nir.pass0(nir_lower_memcpy);

    let dv_opts = nir_remove_dead_variables_options {
        can_remove_var: Some(can_remove_var),
        can_remove_var_data: ptr::null_mut(),
    };
    nir.pass2(
        nir_remove_dead_variables,
        nir_variable_mode::nir_var_uniform
            | nir_variable_mode::nir_var_image
            | nir_variable_mode::nir_var_mem_constant
            | nir_variable_mode::nir_var_mem_shared
            | nir_variable_mode::nir_var_function_temp,
        &dv_opts,
    );

    // asign locations for inline samplers
    let mut last_loc = -1;
    for v in nir
        .variables_with_mode(nir_variable_mode::nir_var_uniform | nir_variable_mode::nir_var_image)
    {
        if unsafe { !glsl_type_is_sampler(v.type_) } {
            last_loc = v.data.location;
            continue;
        }
        let s = unsafe { v.data.anon_1.sampler };
        if s.is_inline_sampler() != 0 {
            last_loc += 1;
            v.data.location = last_loc;

            res.push(InternalKernelArg {
                kind: InternalKernelArgType::InlineSampler(Sampler::nir_to_cl(
                    s.addressing_mode(),
                    s.filter_mode(),
                    s.normalized_coordinates(),
                )),
                offset: 0,
                size: 0,
            });
        } else {
            last_loc = v.data.location;
        }
    }

    nir.pass1(nir_lower_readonly_images_to_tex, false);
    nir.pass0(nir_lower_cl_images);

    nir.reset_scratch_size();
    nir.pass2(
        nir_lower_vars_to_explicit_types,
        nir_variable_mode::nir_var_mem_constant,
        Some(glsl_get_cl_type_size_align),
    );
    nir.extract_constant_initializers();

    // TODO 32 bit devices
    // add vars for global offsets
    res.push(InternalKernelArg {
        kind: InternalKernelArgType::GlobalWorkOffsets,
        offset: 0,
        size: 24,
    });
    lower_state.base_global_invoc_id = nir.add_var(
        nir_variable_mode::nir_var_uniform,
        unsafe { glsl_vector_type(glsl_base_type::GLSL_TYPE_UINT64, 3) },
        args + res.len() - 1,
        "base_global_invocation_id",
    );
    if nir.has_constant() {
        res.push(InternalKernelArg {
            kind: InternalKernelArgType::ConstantBuffer,
            offset: 0,
            size: 8,
        });
        lower_state.const_buf = nir.add_var(
            nir_variable_mode::nir_var_uniform,
            unsafe { glsl_uint64_t_type() },
            args + res.len() - 1,
            "constant_buffer_addr",
        );
    }
    if nir.has_printf() {
        res.push(InternalKernelArg {
            kind: InternalKernelArgType::PrintfBuffer,
            offset: 0,
            size: 8,
        });
        lower_state.printf_buf = nir.add_var(
            nir_variable_mode::nir_var_uniform,
            unsafe { glsl_uint64_t_type() },
            args + res.len() - 1,
            "printf_buffer_addr",
        );
    }

    nir.pass1(nir_shader_gather_info, nir.entrypoint());
    if nir.num_images() > 0 {
        res.push(InternalKernelArg {
            kind: InternalKernelArgType::FormatArray,
            offset: 0,
            size: 2 * nir.num_images() as usize,
        });

        res.push(InternalKernelArg {
            kind: InternalKernelArgType::OrderArray,
            offset: 0,
            size: 2 * nir.num_images() as usize,
        });

        lower_state.format_arr = nir.add_var(
            nir_variable_mode::nir_var_uniform,
            unsafe { glsl_array_type(glsl_int16_t_type(), nir.num_images() as u32, 2) },
            args + res.len() - 2,
            "image_formats",
        );

        lower_state.order_arr = nir.add_var(
            nir_variable_mode::nir_var_uniform,
            unsafe { glsl_array_type(glsl_int16_t_type(), nir.num_images() as u32, 2) },
            args + res.len() - 1,
            "image_orders",
        );
    }

    nir.pass2(
        nir_lower_vars_to_explicit_types,
        nir_variable_mode::nir_var_mem_shared
            | nir_variable_mode::nir_var_function_temp
            | nir_variable_mode::nir_var_uniform
            | nir_variable_mode::nir_var_mem_global,
        Some(glsl_get_cl_type_size_align),
    );
    nir.pass2(
        nir_lower_explicit_io,
        nir_variable_mode::nir_var_mem_global | nir_variable_mode::nir_var_mem_constant,
        nir_address_format::nir_address_format_64bit_global,
    );
    nir.pass0(nir_lower_system_values);
    let mut compute_options = nir_lower_compute_system_values_options::default();
    compute_options.set_has_base_global_invocation_id(true);
    nir.pass1(nir_lower_compute_system_values, &compute_options);

    nir.pass1(rusticl_lower_intrinsics, &mut lower_state);
    nir.pass2(
        nir_lower_explicit_io,
        nir_variable_mode::nir_var_mem_shared
            | nir_variable_mode::nir_var_function_temp
            | nir_variable_mode::nir_var_uniform,
        nir_address_format::nir_address_format_32bit_offset_as_64bit,
    );

    if nir_options.lower_int64_options.0 != 0 {
        nir.pass0(nir_lower_int64);
    }

    nir.pass1(nir_lower_convert_alu_types, None);

    opt_nir(nir, dev);
    dev.screen.finalize_nir(nir);

    nir.pass0(nir_opt_dce);
    nir.sweep_mem();
    res
}

fn convert_spirv_to_nir(
    p: &Program,
    name: &str,
    args: Vec<spirv::SPIRVKernelArg>,
) -> (
    HashMap<Arc<Device>, NirShader>,
    Vec<KernelArg>,
    Vec<InternalKernelArg>,
) {
    let mut nirs = HashMap::new();
    let mut args_set = HashSet::new();
    let mut internal_args_set = HashSet::new();

    // TODO: we could run this in parallel?
    for d in p.devs_with_build() {
        let mut nir = p.to_nir(name, d);

        lower_and_optimize_nir_pre_inputs(d, &mut nir, &d.lib_clc);

        let mut args = KernelArg::from_spirv_nir(&args, &mut nir);
        let mut internal_args = lower_and_optimize_nir_late(d, &mut nir, args.len());
        KernelArg::assign_locations(&mut args, &mut internal_args, &mut nir);

        args_set.insert(args);
        internal_args_set.insert(internal_args);
        nirs.insert(d.clone(), nir);
    }

    // we want the same (internal) args for every compiled kernel, for now
    assert!(args_set.len() == 1);
    assert!(internal_args_set.len() == 1);
    let args = args_set.into_iter().next().unwrap();
    let internal_args = internal_args_set.into_iter().next().unwrap();

    (nirs, args, internal_args)
}

fn extract<'a, const S: usize>(buf: &'a mut &[u8]) -> &'a [u8; S] {
    let val;
    (val, *buf) = (*buf).split_at(S);
    // we split of 4 bytes and convert to [u8; 4], so this should be safe
    // use split_array_ref once it's stable
    val.try_into().unwrap()
}

impl Kernel {
    pub fn new(name: String, prog: Arc<Program>, args: Vec<spirv::SPIRVKernelArg>) -> Arc<Kernel> {
        let (mut nirs, args, internal_args) = convert_spirv_to_nir(&prog, &name, args);

        let nir = nirs.values_mut().next().unwrap();
        let wgs = nir.workgroup_size();
        let work_group_size = [wgs[0] as usize, wgs[1] as usize, wgs[2] as usize];

        // can't use vec!...
        let values = args.iter().map(|_| RefCell::new(None)).collect();

        Arc::new(Self {
            base: CLObjectBase::new(),
            prog: prog,
            name: name,
            args: args,
            work_group_size: work_group_size,
            values: values,
            internal_args: internal_args,
            // caller has to verify all kernels have the same sig
            nirs: nirs,
        })
    }

    // the painful part is, that host threads are allowed to modify the kernel object once it was
    // enqueued, so return a closure with all req data included.
    pub fn launch(
        self: &Arc<Self>,
        q: &Arc<Queue>,
        work_dim: u32,
        block: &[usize],
        grid: &[usize],
        offsets: &[usize],
    ) -> CLResult<EventSig> {
        let nir = self.nirs.get(&q.device).unwrap();
        let mut block = create_kernel_arr::<u32>(block, 1);
        let mut grid = create_kernel_arr::<u32>(grid, 1);
        let offsets = create_kernel_arr::<u64>(offsets, 0);
        let mut input: Vec<u8> = Vec::new();
        let mut resource_info = Vec::new();
        let mut local_size: u32 = nir.shared_size();
        let printf_size = q.device.printf_buffer_size() as u32;
        let mut samplers = Vec::new();
        let mut iviews = Vec::new();
        let mut sviews = Vec::new();
        let mut tex_formats: Vec<u16> = Vec::new();
        let mut tex_orders: Vec<u16> = Vec::new();
        let mut img_formats: Vec<u16> = Vec::new();
        let mut img_orders: Vec<u16> = Vec::new();

        for i in 0..3 {
            if block[i] == 0 {
                block[i] = 1;
            } else {
                grid[i] /= block[i];
            }
        }

        for (arg, val) in self.args.iter().zip(&self.values) {
            if arg.dead {
                continue;
            }

            if arg.kind != KernelArgType::Image
                && arg.kind != KernelArgType::Texture
                && arg.kind != KernelArgType::Sampler
            {
                input.resize(arg.offset, 0);
            }
            match val.borrow().as_ref().unwrap() {
                KernelArgValue::Constant(c) => input.extend_from_slice(c),
                KernelArgValue::MemObject(mem) => {
                    let res = mem.get_res_of_dev(&q.device)?;
                    if mem.is_buffer() {
                        input.extend_from_slice(&mem.offset.to_ne_bytes());
                        resource_info.push((Some(res.clone()), arg.offset));
                    } else {
                        let format = mem.image_format.to_pipe_format().unwrap();
                        let (formats, orders) = if arg.kind == KernelArgType::Image {
                            iviews.push(res.pipe_image_view(format));
                            (&mut img_formats, &mut img_orders)
                        } else {
                            sviews.push((res.clone(), format));
                            (&mut tex_formats, &mut tex_orders)
                        };

                        assert!(arg.offset >= formats.len());

                        formats.resize(arg.offset, 0);
                        orders.resize(arg.offset, 0);

                        formats.push(mem.image_format.image_channel_data_type as u16);
                        orders.push(mem.image_format.image_channel_order as u16);
                    }
                }
                KernelArgValue::LocalMem(size) => {
                    // TODO 32 bit
                    input.extend_from_slice(&[0; 8]);
                    local_size += *size as u32;
                }
                KernelArgValue::Sampler(sampler) => {
                    samplers.push(sampler.pipe());
                }
                KernelArgValue::None => {
                    assert!(
                        arg.kind == KernelArgType::MemGlobal
                            || arg.kind == KernelArgType::MemConstant
                    );
                    input.extend_from_slice(&[0; 8]);
                }
            }
        }

        let mut printf_buf = None;
        for arg in &self.internal_args {
            if arg.offset > input.len() {
                input.resize(arg.offset, 0);
            }
            match arg.kind {
                InternalKernelArgType::ConstantBuffer => {
                    input.extend_from_slice(&[0; 8]);
                    let buf = nir.get_constant_buffer();
                    let res = Arc::new(
                        q.device
                            .screen()
                            .resource_create_buffer(buf.len() as u32)
                            .unwrap(),
                    );
                    q.device
                        .helper_ctx()
                        .exec(|ctx| {
                            ctx.buffer_subdata(&res, 0, buf.as_ptr().cast(), buf.len() as u32)
                        })
                        .wait();
                    resource_info.push((Some(res), arg.offset));
                }
                InternalKernelArgType::GlobalWorkOffsets => {
                    input.extend_from_slice(&cl_prop::<[u64; 3]>(offsets));
                }
                InternalKernelArgType::PrintfBuffer => {
                    let buf =
                        Arc::new(q.device.screen.resource_create_buffer(printf_size).unwrap());

                    input.extend_from_slice(&[0; 8]);
                    resource_info.push((Some(buf.clone()), arg.offset));

                    printf_buf = Some(buf);
                }
                InternalKernelArgType::InlineSampler(cl) => {
                    samplers.push(Sampler::cl_to_pipe(cl));
                }
                InternalKernelArgType::FormatArray => {
                    input.extend_from_slice(&cl_prop::<&Vec<u16>>(&tex_formats));
                    input.extend_from_slice(&cl_prop::<&Vec<u16>>(&img_formats));
                }
                InternalKernelArgType::OrderArray => {
                    input.extend_from_slice(&cl_prop::<&Vec<u16>>(&tex_orders));
                    input.extend_from_slice(&cl_prop::<&Vec<u16>>(&img_orders));
                }
            }
        }

        let k = Arc::clone(self);
        Ok(Box::new(move |q, ctx| {
            let nir = k.nirs.get(&q.device).unwrap();
            let mut input = input.clone();
            let mut resources = Vec::with_capacity(resource_info.len());
            let mut globals: Vec<*mut u32> = Vec::new();
            let printf_format = nir.printf_format();

            let mut sviews: Vec<_> = sviews
                .iter()
                .map(|(s, f)| ctx.create_sampler_view(s, *f))
                .collect();
            let samplers: Vec<_> = samplers
                .iter()
                .map(|s| ctx.create_sampler_state(s))
                .collect();

            for (res, offset) in resource_info.clone() {
                resources.push(res);
                globals.push(unsafe { input.as_mut_ptr().add(offset) }.cast());
            }

            if let Some(printf_buf) = &printf_buf {
                let init_data: [u8; 1] = [4];
                ctx.buffer_subdata(
                    printf_buf,
                    0,
                    init_data.as_ptr().cast(),
                    init_data.len() as u32,
                );
            }
            let cso = ctx.create_compute_state(nir, input.len() as u32, local_size);

            ctx.bind_compute_state(cso);
            ctx.bind_sampler_states(&samplers);
            ctx.set_sampler_views(&mut sviews);
            ctx.set_shader_images(&iviews);
            ctx.set_global_binding(resources.as_slice(), &mut globals);

            ctx.launch_grid(work_dim, block, grid, &input);

            ctx.clear_global_binding(globals.len() as u32);
            ctx.clear_shader_images(iviews.len() as u32);
            ctx.clear_sampler_views(sviews.len() as u32);
            ctx.clear_sampler_states(samplers.len() as u32);
            ctx.delete_compute_state(cso);
            ctx.memory_barrier(PIPE_BARRIER_GLOBAL_BUFFER);

            samplers.iter().for_each(|s| ctx.delete_sampler_state(*s));
            sviews.iter().for_each(|v| ctx.sampler_view_destroy(*v));

            if let Some(printf_buf) = &printf_buf {
                let tx = ctx
                    .buffer_map(printf_buf, 0, printf_size as i32, true)
                    .with_ctx(ctx);
                let mut buf: &[u8] =
                    unsafe { slice::from_raw_parts(tx.ptr().cast(), printf_size as usize) };
                let length = u32::from_ne_bytes(*extract(&mut buf));

                // update our slice to make sure we don't go out of bounds
                buf = &buf[0..(length - 4) as usize];

                unsafe {
                    u_printf(
                        stdout,
                        buf.as_ptr().cast(),
                        buf.len(),
                        printf_format.as_ptr(),
                        printf_format.len() as u32,
                    );
                }
            }

            Ok(())
        }))
    }

    pub fn access_qualifier(&self, idx: cl_uint) -> cl_kernel_arg_access_qualifier {
        let aq = self.args[idx as usize].spirv.access_qualifier;

        if aq
            == clc_kernel_arg_access_qualifier::CLC_KERNEL_ARG_ACCESS_READ
                | clc_kernel_arg_access_qualifier::CLC_KERNEL_ARG_ACCESS_WRITE
        {
            CL_KERNEL_ARG_ACCESS_READ_WRITE
        } else if aq == clc_kernel_arg_access_qualifier::CLC_KERNEL_ARG_ACCESS_READ {
            CL_KERNEL_ARG_ACCESS_READ_ONLY
        } else if aq == clc_kernel_arg_access_qualifier::CLC_KERNEL_ARG_ACCESS_WRITE {
            CL_KERNEL_ARG_ACCESS_WRITE_ONLY
        } else {
            CL_KERNEL_ARG_ACCESS_NONE
        }
    }

    pub fn address_qualifier(&self, idx: cl_uint) -> cl_kernel_arg_address_qualifier {
        match self.args[idx as usize].spirv.address_qualifier {
            clc_kernel_arg_address_qualifier::CLC_KERNEL_ARG_ADDRESS_PRIVATE => {
                CL_KERNEL_ARG_ADDRESS_PRIVATE
            }
            clc_kernel_arg_address_qualifier::CLC_KERNEL_ARG_ADDRESS_CONSTANT => {
                CL_KERNEL_ARG_ADDRESS_CONSTANT
            }
            clc_kernel_arg_address_qualifier::CLC_KERNEL_ARG_ADDRESS_LOCAL => {
                CL_KERNEL_ARG_ADDRESS_LOCAL
            }
            clc_kernel_arg_address_qualifier::CLC_KERNEL_ARG_ADDRESS_GLOBAL => {
                CL_KERNEL_ARG_ADDRESS_GLOBAL
            }
        }
    }

    pub fn type_qualifier(&self, idx: cl_uint) -> cl_kernel_arg_type_qualifier {
        let tq = self.args[idx as usize].spirv.type_qualifier;
        let zero = clc_kernel_arg_type_qualifier(0);
        let mut res = CL_KERNEL_ARG_TYPE_NONE;

        if tq & clc_kernel_arg_type_qualifier::CLC_KERNEL_ARG_TYPE_CONST != zero {
            res |= CL_KERNEL_ARG_TYPE_CONST;
        }

        if tq & clc_kernel_arg_type_qualifier::CLC_KERNEL_ARG_TYPE_RESTRICT != zero {
            res |= CL_KERNEL_ARG_TYPE_RESTRICT;
        }

        if tq & clc_kernel_arg_type_qualifier::CLC_KERNEL_ARG_TYPE_VOLATILE != zero {
            res |= CL_KERNEL_ARG_TYPE_VOLATILE;
        }

        res.into()
    }

    pub fn arg_name(&self, idx: cl_uint) -> &String {
        &self.args[idx as usize].spirv.name
    }

    pub fn arg_type_name(&self, idx: cl_uint) -> &String {
        &self.args[idx as usize].spirv.type_name
    }

    pub fn priv_mem_size(&self, dev: &Arc<Device>) -> cl_ulong {
        self.nirs.get(dev).unwrap().scratch_size() as cl_ulong
    }

    pub fn local_mem_size(&self, dev: &Arc<Device>) -> cl_ulong {
        // TODO include args
        self.nirs.get(dev).unwrap().shared_size() as cl_ulong
    }
}

impl Clone for Kernel {
    fn clone(&self) -> Self {
        Self {
            base: CLObjectBase::new(),
            prog: self.prog.clone(),
            name: self.name.clone(),
            args: self.args.clone(),
            values: self.values.clone(),
            work_group_size: self.work_group_size,
            internal_args: self.internal_args.clone(),
            nirs: self.nirs.clone(),
        }
    }
}
