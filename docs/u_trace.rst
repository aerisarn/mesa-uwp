u_trace GPU Performance Tracing
===============================

Mesa has its own GPU performance tracing framework which drivers may
choose to implement. ``gpu.renderstages.*`` producer for
:doc:`Perfetto Tracing <perfetto>` is based on u_trace.

It doesn't require external dependencies and much simpler to use. Though
it provides information only about GPU timings and is harder to analyze
for complex rendering.

u_trace is useful when one needs to quickly identify performance bottleneck,
or to build a tool to analyze the raw performance data.

Drivers which support u_trace:
   - Intel drivers: Anv, Iris
   - Adreno drivers: Freedreno, Turnip

Usage
-----

u_trace is controlled by environment variables:

:envvar:`GPU_TRACE`
   if set to ``1`` enables tracing and outputs the data into ``stdout``

:envvar:`GPU_TRACEFILE`
   specifies a file where to write the output instead of ``stdout``

:envvar:`GPU_TRACE_FORMAT`
   controls a format of the output

   ``txt``
      human readable text format
   ``json``
      json format, suitable for parsing. Application should appropriately
      finish its rendering in order for trace's json to be valid.
      For Vulkan api it is expected to destroy the device, for GL it is
      expected to destroy the context.

:envvar:`GPU_TRACE_INSTRUMENT`
   Meaningful only for Perfetto tracing. If set to ``1`` enables
   instrumentation of GPU commands before the tracing is enabled.
