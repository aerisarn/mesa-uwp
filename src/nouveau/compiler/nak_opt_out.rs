/*
 * Copyright © 2023 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use crate::nak_ir::*;

use std::collections::HashMap;
use std::slice;

struct OutPass;

impl Shader {
    pub fn opt_out(&mut self) {
        if let ShaderStageInfo::Geometry(_) = self.info.stage {
            for f in &mut self.functions {
                for b in &mut f.blocks {
                    let mut instrs = Vec::new();

                    {
                        let mut drain = b.instrs.drain(..);

                        while let Some(instr) = drain.next() {
                            match instr.op {
                                Op::Out(op) if op.out_type == OutType::Emit => {
                                    let next_op_opt =
                                        drain.next().map(|x| x.op);

                                    match next_op_opt {
                                        Some(Op::Out(next_op))
                                            if next_op.out_type
                                                == OutType::Cut
                                                && op.stream
                                                    == next_op.stream =>
                                        {
                                            instrs.push(Instr::new_boxed(
                                                OpOut {
                                                    dst: next_op.dst.clone(),
                                                    handle: op.handle,
                                                    stream: op.stream,
                                                    out_type:
                                                        OutType::EmitThenCut,
                                                },
                                            ));
                                        }
                                        Some(next_op) => {
                                            instrs.push(Instr::new_boxed(op));
                                            instrs.push(Instr::new_boxed(
                                                next_op,
                                            ));
                                        }
                                        None => {}
                                    }
                                }
                                _ => instrs.push(instr),
                            }
                        }
                    }

                    b.instrs = instrs;
                }
            }
        }
    }
}
