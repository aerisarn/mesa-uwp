extern crate mesa_rust_util;
extern crate rusticl_opencl_gen;

use crate::api::icd::*;
use crate::api::util::*;
use crate::core::kernel::*;

use self::mesa_rust_util::string::*;
use self::rusticl_opencl_gen::*;

use std::collections::HashSet;
use std::sync::Arc;

impl CLInfo<cl_kernel_info> for cl_kernel {
    fn query(&self, q: cl_kernel_info) -> CLResult<Vec<u8>> {
        let kernel = self.get_ref()?;
        Ok(match q {
            CL_KERNEL_CONTEXT => {
                let ptr = Arc::as_ptr(&kernel.prog.context);
                cl_prop::<cl_context>(cl_context::from_ptr(ptr))
            }
            CL_KERNEL_FUNCTION_NAME => cl_prop::<&str>(&kernel.name),
            CL_KERNEL_NUM_ARGS => cl_prop::<cl_uint>(kernel.args.len() as cl_uint),
            CL_KERNEL_PROGRAM => {
                let ptr = Arc::as_ptr(&kernel.prog);
                cl_prop::<cl_program>(cl_program::from_ptr(ptr))
            }
            CL_KERNEL_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refcnt()?),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}

impl CLInfoObj<cl_kernel_arg_info, cl_uint> for cl_kernel {
    fn query(&self, idx: cl_uint, q: cl_kernel_arg_info) -> CLResult<Vec<u8>> {
        let kernel = self.get_ref()?;

        // CL_INVALID_ARG_INDEX if arg_index is not a valid argument index.
        if idx as usize >= kernel.args.len() {
            return Err(CL_INVALID_ARG_INDEX);
        }

        Ok(match *q {
            CL_KERNEL_ARG_ACCESS_QUALIFIER => {
                cl_prop::<cl_kernel_arg_access_qualifier>(kernel.access_qualifier(idx))
            }
            CL_KERNEL_ARG_ADDRESS_QUALIFIER => {
                cl_prop::<cl_kernel_arg_address_qualifier>(kernel.address_qualifier(idx))
            }
            CL_KERNEL_ARG_NAME => cl_prop::<&str>(kernel.arg_name(idx)),
            CL_KERNEL_ARG_TYPE_NAME => cl_prop::<&str>(kernel.arg_type_name(idx)),
            CL_KERNEL_ARG_TYPE_QUALIFIER => {
                cl_prop::<cl_kernel_arg_type_qualifier>(kernel.type_qualifier(idx))
            }
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}

impl CLInfoObj<cl_kernel_work_group_info, cl_device_id> for cl_kernel {
    fn query(&self, dev: cl_device_id, q: cl_kernel_work_group_info) -> CLResult<Vec<u8>> {
        let _kernel = self.get_ref()?;
        let _dev = dev.get_ref()?;
        Ok(match *q {
            CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE => cl_prop::<usize>(1),
            // TODO
            CL_KERNEL_WORK_GROUP_SIZE => cl_prop::<usize>(1),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}

pub fn create_kernel(
    program: cl_program,
    kernel_name: *const ::std::os::raw::c_char,
) -> CLResult<cl_kernel> {
    let p = program.get_arc()?;
    let name = c_string_to_string(kernel_name);

    // CL_INVALID_VALUE if kernel_name is NULL.
    if kernel_name.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_PROGRAM_EXECUTABLE if there is no successfully built executable for program.
    if p.kernels().is_empty() {
        return Err(CL_INVALID_PROGRAM_EXECUTABLE);
    }

    // CL_INVALID_KERNEL_NAME if kernel_name is not found in program.
    if !p.kernels().contains(&name) {
        return Err(CL_INVALID_KERNEL_NAME);
    }

    // CL_INVALID_PROGRAM_EXECUTABLE if there is no successfully built executable for program.
    let devs: Vec<_> = p
        .devs
        .iter()
        .filter(|d| p.status(d) == CL_BUILD_SUCCESS as cl_build_status)
        .collect();
    if devs.is_empty() {
        return Err(CL_INVALID_PROGRAM_EXECUTABLE);
    }

    // CL_INVALID_KERNEL_DEFINITION if the function definition for __kernel function given by
    // kernel_name such as the number of arguments, the argument types are not the same for all
    // devices for which the program executable has been built.
    let kernel_args: HashSet<_> = devs.iter().map(|d| p.args(d, &name)).collect();
    if kernel_args.len() != 1 {
        return Err(CL_INVALID_KERNEL_DEFINITION);
    }

    Ok(cl_kernel::from_arc(Kernel::new(
        name,
        p,
        kernel_args.into_iter().next().unwrap(),
    )))
}

pub fn set_kernel_arg(
    kernel: cl_kernel,
    arg_index: cl_uint,
    _arg_size: usize,
    _arg_value: *const ::std::os::raw::c_void,
) -> CLResult<()> {
    let k = kernel.get_arc()?;

    // CL_INVALID_ARG_INDEX if arg_index is not a valid argument index.
    if arg_index as usize >= k.args.len() {
        return Err(CL_INVALID_ARG_INDEX);
    }

    //• CL_INVALID_ARG_VALUE if arg_value specified is not a valid value.
    //• CL_INVALID_MEM_OBJECT for an argument declared to be a memory object when the specified arg_value is not a valid memory object.
    //• CL_INVALID_SAMPLER for an argument declared to be of type sampler_t when the specified arg_value is not a valid sampler object.
    //• CL_INVALID_DEVICE_QUEUE for an argument declared to be of type queue_t when the specified arg_value is not a valid device queue object. This error code is missing before version 2.0.
    //• CL_INVALID_ARG_SIZE if arg_size does not match the size of the data type for an argument that is not a memory object or if the argument is a memory object and arg_size != sizeof(cl_mem) or if arg_size is zero and the argument is declared with the local qualifier or if the argument is a sampler and arg_size != sizeof(cl_sampler).
    //• CL_MAX_SIZE_RESTRICTION_EXCEEDED if the size in bytes of the memory object (if the argument is a memory object) or arg_size (if the argument is declared with local qualifier) exceeds a language- specified maximum size restriction for this argument, such as the MaxByteOffset SPIR-V decoration. This error code is missing before version 2.2.
    //• CL_INVALID_ARG_VALUE if the argument is an image declared with the read_only qualifier and arg_value refers to an image object created with cl_mem_flags of CL_MEM_WRITE_ONLY or if the image argument is declared with the write_only qualifier and arg_value refers to an image object created with cl_mem_flags of CL_MEM_READ_ONLY.

    println!("set_kernel_arg not implemented");
    Err(CL_OUT_OF_HOST_MEMORY)
}
