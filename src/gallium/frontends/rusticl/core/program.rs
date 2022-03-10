extern crate mesa_rust;
extern crate rusticl_opencl_gen;

use crate::api::icd::*;
use crate::core::context::*;
use crate::core::device::*;
use crate::impl_cl_type_trait;

use self::mesa_rust::compiler::clc::*;
use self::rusticl_opencl_gen::*;

use std::collections::HashMap;
use std::collections::HashSet;
use std::ffi::CString;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::MutexGuard;

#[repr(C)]
pub struct Program {
    pub base: CLObjectBase<CL_INVALID_PROGRAM>,
    pub context: Arc<Context>,
    pub devs: Vec<Arc<Device>>,
    pub src: CString,
    build: Mutex<ProgramBuild>,
}

impl_cl_type_trait!(cl_program, Program, CL_INVALID_PROGRAM);

struct ProgramBuild {
    builds: HashMap<Arc<Device>, ProgramDevBuild>,
    kernels: Vec<String>,
}

struct ProgramDevBuild {
    spirv: Option<spirv::SPIRVBin>,
    status: cl_build_status,
    options: String,
    log: String,
}

fn prepare_options(options: &str) -> Vec<CString> {
    options
        .split_whitespace()
        .map(|a| match a {
            "-cl-denorms-are-zero" => "-fdenormal-fp-math=positive-zero",
            _ => a,
        })
        .map(CString::new)
        .map(Result::unwrap)
        .collect()
}

impl Program {
    pub fn new(context: &Arc<Context>, devs: &[Arc<Device>], src: CString) -> Arc<Program> {
        let builds = devs
            .iter()
            .map(|d| {
                (
                    d.clone(),
                    ProgramDevBuild {
                        spirv: None,
                        status: CL_BUILD_NONE,
                        log: String::from(""),
                        options: String::from(""),
                    },
                )
            })
            .collect();

        Arc::new(Self {
            base: CLObjectBase::new(),
            context: context.clone(),
            devs: devs.to_vec(),
            src: src,
            build: Mutex::new(ProgramBuild {
                builds: builds,
                kernels: Vec::new(),
            }),
        })
    }

    fn build_info(&self) -> MutexGuard<ProgramBuild> {
        self.build.lock().unwrap()
    }

    fn dev_build_info<'a>(
        l: &'a mut MutexGuard<ProgramBuild>,
        dev: &Arc<Device>,
    ) -> &'a mut ProgramDevBuild {
        l.builds.get_mut(dev).unwrap()
    }

    pub fn status(&self, dev: &Arc<Device>) -> cl_build_status {
        Self::dev_build_info(&mut self.build_info(), dev).status
    }

    pub fn log(&self, dev: &Arc<Device>) -> String {
        Self::dev_build_info(&mut self.build_info(), dev)
            .log
            .clone()
    }

    pub fn options(&self, dev: &Arc<Device>) -> String {
        Self::dev_build_info(&mut self.build_info(), dev)
            .options
            .clone()
    }

    pub fn args(&self, dev: &Arc<Device>, kernel: &str) -> Vec<spirv::SPIRVKernelArg> {
        Self::dev_build_info(&mut self.build_info(), dev)
            .spirv
            .as_ref()
            .unwrap()
            .args(kernel)
    }

    pub fn kernels(&self) -> Vec<String> {
        self.build_info().kernels.clone()
    }

    pub fn build(&self, dev: &Arc<Device>, options: String) -> bool {
        let mut info = self.build_info();
        let d = Self::dev_build_info(&mut info, dev);

        let args = prepare_options(&options);
        let (spirv, log) =
            spirv::SPIRVBin::from_clc(&self.src, &args, &Vec::new(), dev.cl_features());

        d.log = log;
        d.options = options;
        if spirv.is_none() {
            d.status = CL_BUILD_ERROR;
            return false;
        }

        let spirvs = vec![spirv.as_ref().unwrap()];
        let (spirv, log) = spirv::SPIRVBin::link(&spirvs, false);

        d.log.push_str(&log);
        d.spirv = spirv;

        d.status = match &d.spirv {
            None => CL_BUILD_ERROR,
            Some(_) => CL_BUILD_SUCCESS as cl_build_status,
        };

        if d.spirv.is_some() {
            let mut kernels = d.spirv.as_ref().unwrap().kernels();
            info.kernels.append(&mut kernels);
            true
        } else {
            false
        }
    }

    pub fn compile(
        &self,
        dev: &Arc<Device>,
        options: String,
        headers: &[spirv::CLCHeader],
    ) -> bool {
        let mut info = self.build_info();
        let d = Self::dev_build_info(&mut info, dev);
        let args = prepare_options(&options);

        let (spirv, log) = spirv::SPIRVBin::from_clc(&self.src, &args, headers, dev.cl_features());

        d.spirv = spirv;
        d.log = log;
        d.options = options;

        if d.spirv.is_some() {
            d.status = CL_BUILD_SUCCESS as cl_build_status;
            true
        } else {
            d.status = CL_BUILD_ERROR;
            false
        }
    }

    pub fn link(
        context: Arc<Context>,
        devs: &[Arc<Device>],
        progs: &[Arc<Program>],
    ) -> Arc<Program> {
        let devs: Vec<Arc<Device>> = devs.iter().map(|d| (*d).clone()).collect();
        let mut builds = HashMap::new();
        let mut kernels = HashSet::new();
        let mut locks: Vec<_> = progs.iter().map(|p| p.build_info()).collect();

        for d in &devs {
            let bins: Vec<_> = locks
                .iter_mut()
                .map(|l| Self::dev_build_info(l, d).spirv.as_ref().unwrap())
                .collect();

            let (spirv, log) = spirv::SPIRVBin::link(&bins, false);
            let status = if let Some(spirv) = &spirv {
                for k in spirv.kernels() {
                    kernels.insert(k);
                }
                CL_BUILD_SUCCESS as cl_build_status
            } else {
                CL_BUILD_ERROR
            };

            builds.insert(
                d.clone(),
                ProgramDevBuild {
                    spirv: spirv,
                    status: status,
                    log: log,
                    options: String::from(""),
                },
            );
        }

        Arc::new(Self {
            base: CLObjectBase::new(),
            context: context,
            devs: devs,
            src: CString::new("").unwrap(),
            build: Mutex::new(ProgramBuild {
                builds: builds,
                kernels: kernels.into_iter().collect(),
            }),
        })
    }
}
