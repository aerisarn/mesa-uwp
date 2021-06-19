#include "sfn_shader_cs.h"
#include "sfn_instr_fetch.h"


namespace r600 {

ComputeShader::ComputeShader(UNUSED const r600_shader_key& key):
   Shader("CS")
{

}

bool ComputeShader::do_scan_instruction(UNUSED nir_instr *instr)
{
   return false;
}

int ComputeShader::do_allocate_reserved_registers()
{
   auto& vf = value_factory();

   const int thread_id_sel = 0;
   const int wg_id_sel = 1;

   for (int i = 0; i < 3; ++i) {
      m_local_invocation_id[i] = vf.allocate_pinned_register(thread_id_sel, i);
      m_local_invocation_id[i]->pin_live_range(true);

      m_workgroup_id[i] = vf.allocate_pinned_register(wg_id_sel, i);
      m_workgroup_id[i]->pin_live_range(true);
   }
   return 2;
}

bool ComputeShader::process_stage_intrinsic(nir_intrinsic_instr *instr)
{
   switch (instr->intrinsic) {
   case nir_intrinsic_load_local_invocation_id:
      return emit_load_3vec(instr, m_local_invocation_id);
   case nir_intrinsic_load_workgroup_id:
      return emit_load_3vec(instr, m_workgroup_id);
   case nir_intrinsic_load_num_workgroups:
      return emit_load_num_workgroups(instr);
   default:
      return false;
   }
}

void ComputeShader::do_get_shader_info(r600_shader *sh_info)
{
   sh_info->processor_type = PIPE_SHADER_COMPUTE;
}

bool ComputeShader::read_prop(UNUSED std::istream& is)
{
   return true;
}

void ComputeShader::do_print_properties(UNUSED std::ostream& os) const
{

}

bool ComputeShader::emit_load_num_workgroups(nir_intrinsic_instr* instr)
{
   auto zero = value_factory().temp_register();

   emit_instruction(new AluInstr(op1_mov, zero, value_factory().inline_const(ALU_SRC_0, 0),
                                 AluInstr::last_write));
   auto dest = value_factory().dest_vec4(instr->dest, pin_group);

   auto ir = new LoadFromBuffer(dest, {0,1,2,7}, zero, 16,
                                R600_BUFFER_INFO_CONST_BUFFER,
                                nullptr, fmt_32_32_32_32);

   ir->set_fetch_flag(LoadFromBuffer::srf_mode);
   ir->reset_fetch_flag(LoadFromBuffer::format_comp_signed);
   ir->set_num_format(vtx_nf_int);
   emit_instruction(ir);
   return true;

}

bool ComputeShader::emit_load_3vec(nir_intrinsic_instr* instr, const std::array<PRegister,3>& src)
{
   auto& vf = value_factory();

   for (int i = 0; i < 3; ++i) {
      auto dest = vf.dest(instr->dest, i, pin_none);
      emit_instruction(new AluInstr(op1_mov, dest, src[i], i == 2 ? AluInstr::last_write : AluInstr::write));
   }
   return true;
}

}
