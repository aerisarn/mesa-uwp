#ifndef OPTIMIZER_H
#define OPTIMIZER_H

#include "sfn_shader.h"

namespace r600 {

bool dead_code_elimination(Shader& shader);
bool copy_propagation_fwd(Shader& shader);
bool copy_propagation_backward(Shader& shader);
bool simplify_source_vectors(Shader& sh);

bool optimize(Shader& shader);

}

#endif // OPTIMIZER_H
