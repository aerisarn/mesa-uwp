extern crate rusticl_opencl_gen;

use crate::api::icd::*;
use crate::api::util::*;
use crate::core::queue::*;

use self::rusticl_opencl_gen::*;

use std::sync::Arc;

impl CLInfo<cl_command_queue_info> for cl_command_queue {
    fn query(&self, q: cl_command_queue_info) -> CLResult<Vec<u8>> {
        let queue = self.get_ref()?;
        Ok(match q {
            CL_QUEUE_CONTEXT => {
                // Note we use as_ptr here which doesn't increase the reference count.
                let ptr = Arc::as_ptr(&queue.context);
                cl_prop::<cl_context>(cl_context::from_ptr(ptr))
            }
            CL_QUEUE_DEVICE => {
                // Note we use as_ptr here which doesn't increase the reference count.
                let ptr = Arc::as_ptr(&queue.device);
                cl_prop::<cl_device_id>(cl_device_id::from_ptr(ptr))
            }
            CL_QUEUE_PROPERTIES => cl_prop::<cl_command_queue_properties>(queue.props),
            CL_QUEUE_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refcnt()?),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}

fn valid_command_queue_properties(properties: cl_command_queue_properties) -> bool {
    let valid_flags =
        cl_bitfield::from(CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE | CL_QUEUE_PROFILING_ENABLE);
    properties & !valid_flags == 0
}

fn supported_command_queue_properties(properties: cl_command_queue_properties) -> bool {
    let valid_flags = cl_bitfield::from(CL_QUEUE_PROFILING_ENABLE);
    properties & !valid_flags == 0
}

pub fn create_command_queue(
    context: cl_context,
    device: cl_device_id,
    properties: cl_command_queue_properties,
) -> CLResult<cl_command_queue> {
    // CL_INVALID_CONTEXT if context is not a valid context.
    let c = context.get_arc()?;

    // CL_INVALID_DEVICE if device is not a valid device
    let d = device.get_arc()?;

    // ... or is not associated with context.
    if !c.devs.contains(&d) {
        return Err(CL_INVALID_DEVICE);
    }

    // CL_INVALID_VALUE if values specified in properties are not valid.
    if !valid_command_queue_properties(properties) {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_QUEUE_PROPERTIES if values specified in properties are valid but are not supported by the device.
    if !supported_command_queue_properties(properties) {
        return Err(CL_INVALID_QUEUE_PROPERTIES);
    }

    Ok(cl_command_queue::from_arc(Queue::new(c, d, properties)?))
}

pub fn flush_queue(command_queue: cl_command_queue) -> CLResult<()> {
    // CL_INVALID_COMMAND_QUEUE if command_queue is not a valid host command-queue.
    command_queue.get_ref()?.flush(false)
}

pub fn finish_queue(command_queue: cl_command_queue) -> CLResult<()> {
    // CL_INVALID_COMMAND_QUEUE if command_queue is not a valid host command-queue.
    command_queue.get_ref()?.flush(true)
}

pub fn release_command_queue(command_queue: cl_command_queue) -> CLResult<()> {
    // clReleaseCommandQueue performs an implicit flush to issue any previously queued OpenCL
    // commands in command_queue.
    flush_queue(command_queue)?;
    command_queue.release()?;
    Ok(())
}
