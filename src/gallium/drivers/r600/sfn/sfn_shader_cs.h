#ifndef COMPUTE_H
#define COMPUTE_H

#include "sfn_shader.h"

namespace r600 {

class ComputeShader : public Shader
{
public:
   ComputeShader(const r600_shader_key& key);

private:
   bool do_scan_instruction(nir_instr *instr) override;
   int do_allocate_reserved_registers() override;

   bool process_stage_intrinsic(nir_intrinsic_instr *intr) override;
   void do_get_shader_info(r600_shader *sh_info) override;

   bool load_input(UNUSED nir_intrinsic_instr *intr) override {
      unreachable("compute shaders  have bno inputs");
   };
   bool store_output(UNUSED nir_intrinsic_instr *intr) override {
      unreachable("compute shaders have no outputs");
   };

   bool read_prop(std::istream& is) override;
   void do_print_properties(std::ostream& os) const override;

   bool emit_load_num_workgroups(nir_intrinsic_instr* instr);
   bool emit_load_3vec(nir_intrinsic_instr* instr, const std::array<PRegister,3>& src);

   std::array<PRegister,3> m_workgroup_id{nullptr};
   std::array<PRegister,3> m_local_invocation_id{nullptr};
};

}

#endif // COMPUTE_H
