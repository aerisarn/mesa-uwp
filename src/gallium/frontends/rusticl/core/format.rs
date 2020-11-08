extern crate mesa_rust_gen;
extern crate rusticl_opencl_gen;

use crate::api::util::*;

use self::mesa_rust_gen::pipe_format;
use self::rusticl_opencl_gen::*;

pub struct RusticlImageFormat {
    pub cl_image_format: cl_image_format,
    pub req_for_full_read_or_write: bool,
    pub req_for_embeded_read_or_write: bool,
    pub req_for_full_read_and_write: bool,
    pub pipe: pipe_format,
}

pub const fn rusticl_image_format(
    cl_image_format: cl_image_format,
    req_for_full_read_or_write: bool,
    req_for_embeded_read_or_write: bool,
    req_for_full_read_and_write: bool,
    pipe: pipe_format,
) -> RusticlImageFormat {
    RusticlImageFormat {
        cl_image_format: cl_image_format,
        req_for_full_read_or_write: req_for_full_read_or_write,
        req_for_embeded_read_or_write: req_for_embeded_read_or_write,
        req_for_full_read_and_write: req_for_full_read_and_write,
        pipe: pipe,
    }
}

pub const FORMATS: &[RusticlImageFormat] = &[
    rusticl_image_format(
        cl_image_format(CL_R, CL_HALF_FLOAT),
        false,
        false,
        true,
        pipe_format::PIPE_FORMAT_R16_FLOAT,
    ),
    rusticl_image_format(
        cl_image_format(CL_R, CL_FLOAT),
        false,
        false,
        true,
        pipe_format::PIPE_FORMAT_R32_FLOAT,
    ),
    rusticl_image_format(
        cl_image_format(CL_R, CL_SIGNED_INT8),
        false,
        false,
        true,
        pipe_format::PIPE_FORMAT_R8_SINT,
    ),
    rusticl_image_format(
        cl_image_format(CL_R, CL_SIGNED_INT16),
        false,
        false,
        true,
        pipe_format::PIPE_FORMAT_R16_SINT,
    ),
    rusticl_image_format(
        cl_image_format(CL_R, CL_SIGNED_INT32),
        false,
        false,
        true,
        pipe_format::PIPE_FORMAT_R32_SINT,
    ),
    rusticl_image_format(
        cl_image_format(CL_R, CL_UNORM_INT8),
        false,
        false,
        true,
        pipe_format::PIPE_FORMAT_R8_UNORM,
    ),
    rusticl_image_format(
        cl_image_format(CL_R, CL_UNORM_INT16),
        false,
        false,
        false,
        pipe_format::PIPE_FORMAT_R16_UNORM,
    ),
    rusticl_image_format(
        cl_image_format(CL_R, CL_UNSIGNED_INT8),
        false,
        false,
        true,
        pipe_format::PIPE_FORMAT_R8_UINT,
    ),
    rusticl_image_format(
        cl_image_format(CL_R, CL_UNSIGNED_INT16),
        false,
        false,
        true,
        pipe_format::PIPE_FORMAT_R16_UINT,
    ),
    rusticl_image_format(
        cl_image_format(CL_R, CL_UNSIGNED_INT32),
        false,
        false,
        true,
        pipe_format::PIPE_FORMAT_R32_UINT,
    ),
    rusticl_image_format(
        cl_image_format(CL_RGBA, CL_HALF_FLOAT),
        true,
        true,
        true,
        pipe_format::PIPE_FORMAT_R16G16B16A16_FLOAT,
    ),
    rusticl_image_format(
        cl_image_format(CL_RGBA, CL_FLOAT),
        true,
        true,
        true,
        pipe_format::PIPE_FORMAT_R32G32B32A32_FLOAT,
    ),
    rusticl_image_format(
        cl_image_format(CL_RGBA, CL_SIGNED_INT8),
        true,
        true,
        true,
        pipe_format::PIPE_FORMAT_R8G8B8A8_SINT,
    ),
    rusticl_image_format(
        cl_image_format(CL_RGBA, CL_SIGNED_INT16),
        true,
        true,
        true,
        pipe_format::PIPE_FORMAT_R16G16B16A16_SINT,
    ),
    rusticl_image_format(
        cl_image_format(CL_RGBA, CL_SIGNED_INT32),
        true,
        true,
        true,
        pipe_format::PIPE_FORMAT_R32G32B32A32_SINT,
    ),
    rusticl_image_format(
        cl_image_format(CL_RGBA, CL_UNORM_INT8),
        true,
        true,
        true,
        pipe_format::PIPE_FORMAT_R8G8B8A8_UNORM,
    ),
    rusticl_image_format(
        cl_image_format(CL_RGBA, CL_UNORM_INT16),
        true,
        true,
        false,
        pipe_format::PIPE_FORMAT_R16G16B16A16_UNORM,
    ),
    rusticl_image_format(
        cl_image_format(CL_RGBA, CL_UNSIGNED_INT8),
        true,
        true,
        true,
        pipe_format::PIPE_FORMAT_R8G8B8A8_UINT,
    ),
    rusticl_image_format(
        cl_image_format(CL_RGBA, CL_UNSIGNED_INT16),
        true,
        true,
        true,
        pipe_format::PIPE_FORMAT_R16G16B16A16_UINT,
    ),
    rusticl_image_format(
        cl_image_format(CL_RGBA, CL_UNSIGNED_INT32),
        true,
        true,
        true,
        pipe_format::PIPE_FORMAT_R32G32B32A32_UINT,
    ),
    rusticl_image_format(
        cl_image_format(CL_BGRA, CL_UNORM_INT8),
        true,
        false,
        false,
        pipe_format::PIPE_FORMAT_B8G8R8A8_UNORM,
    ),
];
