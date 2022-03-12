extern crate rusticl_opencl_gen;

use crate::api::icd::*;
use crate::api::util::*;
use crate::core::event::*;
use crate::core::queue::*;

use self::rusticl_opencl_gen::*;

use std::collections::HashSet;
use std::ptr;
use std::sync::Arc;

impl CLInfo<cl_event_info> for cl_event {
    fn query(&self, q: cl_event_info) -> CLResult<Vec<u8>> {
        let event = self.get_ref()?;
        Ok(match q {
            CL_EVENT_COMMAND_EXECUTION_STATUS => cl_prop::<cl_int>(event.status()),
            CL_EVENT_CONTEXT => {
                // Note we use as_ptr here which doesn't increase the reference count.
                let ptr = Arc::as_ptr(&event.context);
                cl_prop::<cl_context>(cl_context::from_ptr(ptr))
            }
            CL_EVENT_COMMAND_QUEUE => {
                let ptr = match event.queue.as_ref() {
                    // Note we use as_ptr here which doesn't increase the reference count.
                    Some(queue) => Arc::as_ptr(queue),
                    None => ptr::null_mut(),
                };
                cl_prop::<cl_command_queue>(cl_command_queue::from_ptr(ptr))
            }
            CL_EVENT_REFERENCE_COUNT => cl_prop::<cl_uint>(self.refcnt()?),
            CL_EVENT_COMMAND_TYPE => cl_prop::<cl_command_type>(event.cmd_type),
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}

pub fn create_user_event(context: cl_context) -> CLResult<cl_event> {
    let c = context.get_arc()?;
    Ok(cl_event::from_arc(Event::new_user(c)))
}

pub fn wait_for_events(num_events: cl_uint, event_list: *const cl_event) -> CLResult<()> {
    let evs = cl_event::get_arc_vec_from_arr(event_list, num_events)?;

    // CL_INVALID_VALUE if num_events is zero or event_list is NULL.
    if evs.is_empty() {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_CONTEXT if events specified in event_list do not belong to the same context.
    let contexts: HashSet<_> = evs.iter().map(|e| &e.context).collect();
    if contexts.len() != 1 {
        return Err(CL_INVALID_CONTEXT);
    }

    // TODO better impl
    let mut err = false;
    for e in evs {
        if let Some(q) = &e.queue {
            q.flush(true)?;
        }

        err |= e.status() < 0;
    }

    // CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST if the execution status of any of the events
    // in event_list is a negative integer value.
    if err {
        return Err(CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST);
    }

    Ok(())
}

pub fn create_and_queue(
    q: Arc<Queue>,
    cmd_type: cl_command_type,
    deps: Vec<Arc<Event>>,
    event: *mut cl_event,
    block: bool,
    work: EventSig,
) -> CLResult<()> {
    let e = Event::new(&q, cmd_type, deps, work);
    cl_event::leak_ref(event, &e);
    q.queue(&e);
    if block {
        q.flush(true)?;
    }
    Ok(())
}
