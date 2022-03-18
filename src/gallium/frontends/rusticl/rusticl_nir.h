struct rusticl_lower_state {
    nir_variable *base_global_invoc_id;
    nir_variable *const_buf;
    nir_variable *printf_buf;
};

bool rusticl_lower_intrinsics(nir_shader *nir, struct rusticl_lower_state *state);
