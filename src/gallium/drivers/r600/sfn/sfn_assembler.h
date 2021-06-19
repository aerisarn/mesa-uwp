#ifndef ASSEMBLER_H
#define ASSEMBLER_H

#include "../r600_pipe.h"
#include "../r600_shader.h"

#include "sfn_shader.h"

namespace r600 {

class Assembler
{
public:
   Assembler(r600_shader *sh, const r600_shader_key& key);

   bool lower(Shader *shader);
private:
   r600_shader *m_sh;
   const r600_shader_key& m_key;
};



}

#endif // ASSAMBLY_H
