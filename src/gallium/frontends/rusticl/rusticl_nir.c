#include "nir.h"
#include "nir_builder.h"

#include "rusticl_nir.h"

static bool
rusticl_lower_intrinsics_filter(const nir_instr* instr, const void* state)
{
    return instr->type == nir_instr_type_intrinsic;
}

static nir_ssa_def*
rusticl_lower_intrinsics_instr(
    nir_builder *b,
    nir_instr *instr,
    void* _state
) {
    nir_intrinsic_instr *intrinsic = nir_instr_as_intrinsic(instr);
    struct rusticl_lower_state *state = _state;

    switch (intrinsic->intrinsic) {
    case nir_intrinsic_load_base_global_invocation_id:
        return nir_load_var(b, state->base_global_invoc_id);
    case nir_intrinsic_load_constant_base_ptr:
        return nir_load_var(b, state->const_buf);
    case nir_intrinsic_load_printf_buffer_address:
        return nir_load_var(b, state->printf_buf);
    default:
        return NULL;
    }
}

bool
rusticl_lower_intrinsics(nir_shader *nir, struct rusticl_lower_state* state)
{
    return nir_shader_lower_instructions(
        nir,
        rusticl_lower_intrinsics_filter,
        rusticl_lower_intrinsics_instr,
        state
    );
}
