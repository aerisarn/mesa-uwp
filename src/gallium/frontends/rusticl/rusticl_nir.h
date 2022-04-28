struct rusticl_lower_state {
    nir_variable *const_buf;
};

bool rusticl_lower_intrinsics(nir_shader *nir, struct rusticl_lower_state *state);
