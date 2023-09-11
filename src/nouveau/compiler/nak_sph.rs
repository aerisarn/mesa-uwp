// Copyright © 2023 Collabora, Ltd.
// SPDX-License-Identifier: MIT

use std::ops::Range;

use nak_bindings::*;

use crate::bitview::{
    BitMutView, BitMutViewable, BitView, BitViewable, SetBit, SetField,
    SetFieldU64,
};

pub const FERMI_SHADER_HEADER_SIZE: usize = 20;
pub const TURING_SHADER_HEADER_SIZE: usize = 32;
pub const CURRENT_MAX_SHADER_HEADER_SIZE: usize = TURING_SHADER_HEADER_SIZE;

type SubSPHView<'a> = BitMutView<'a, [u32; CURRENT_MAX_SHADER_HEADER_SIZE]>;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShaderType {
    Vertex,
    TessellationControl,
    Tessellation,
    Geometry,
    Fragment,
}

impl From<pipe_shader_type> for ShaderType {
    fn from(value: pipe_shader_type) -> Self {
        match value {
            MESA_SHADER_VERTEX => ShaderType::Vertex,
            MESA_SHADER_TESS_CTRL => ShaderType::TessellationControl,
            MESA_SHADER_TESS_EVAL => ShaderType::Tessellation,
            MESA_SHADER_GEOMETRY => ShaderType::Geometry,
            MESA_SHADER_FRAGMENT => ShaderType::Fragment,
            _ => panic!("Invalid pipe_shader_type {}", value),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OutputTopology {
    PointList,
    LineStrip,
    TriangleStrip,
}

impl From<mesa_prim> for OutputTopology {
    fn from(value: mesa_prim) -> Self {
        match value {
            MESA_PRIM_POINTS => OutputTopology::PointList,
            MESA_PRIM_TRIANGLES | MESA_PRIM_LINE_STRIP => {
                OutputTopology::LineStrip
            }
            MESA_PRIM_TRIANGLE_STRIP => OutputTopology::TriangleStrip,
            _ => panic!("Invalid mesa_prim {}", value),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PixelImap {
    Unused,
    Constant,
    Perspective,
    ScreenLinear,
}

impl From<glsl_interp_mode> for PixelImap {
    fn from(value: glsl_interp_mode) -> Self {
        match value {
            INTERP_MODE_NONE | INTERP_MODE_SMOOTH => PixelImap::Perspective,
            INTERP_MODE_FLAT => PixelImap::Constant,
            INTERP_MODE_NOPERSPECTIVE => PixelImap::ScreenLinear,
            INTERP_MODE_EXPLICIT => PixelImap::Unused,
            _ => panic!("Unsupported INTERP_MODE"),
        }
    }
}

impl From<PixelImap> for u8 {
    fn from(value: PixelImap) -> u8 {
        match value {
            PixelImap::Unused => 0,
            PixelImap::Constant => 1,
            PixelImap::Perspective => 2,
            PixelImap::ScreenLinear => 3,
        }
    }
}

#[derive(Debug)]
pub struct ShaderProgramHeader {
    pub data: [u32; CURRENT_MAX_SHADER_HEADER_SIZE],
    shader_type: ShaderType,
    sm: u8,
}

impl BitViewable for ShaderProgramHeader {
    fn bits(&self) -> usize {
        BitView::new(&self.data).bits()
    }

    fn get_bit_range_u64(&self, range: Range<usize>) -> u64 {
        BitView::new(&self.data).get_bit_range_u64(range)
    }
}

impl BitMutViewable for ShaderProgramHeader {
    fn set_bit_range_u64(&mut self, range: Range<usize>, val: u64) {
        BitMutView::new(&mut self.data).set_bit_range_u64(range, val);
    }
}

impl SetFieldU64 for ShaderProgramHeader {
    fn set_field_u64(&mut self, range: Range<usize>, val: u64) {
        BitMutView::new(&mut self.data).set_field_u64(range, val);
    }
}

impl ShaderProgramHeader {
    pub fn new(shader_type: ShaderType, sm: u8) -> Self {
        let mut res = Self {
            data: [0; CURRENT_MAX_SHADER_HEADER_SIZE],
            shader_type,
            sm,
        };

        let sph_type = if shader_type == ShaderType::Fragment {
            2
        } else {
            1
        };

        let sph_version = 3;
        res.set_sph_type(sph_type, sph_version);
        res.set_shader_type(shader_type);

        res
    }

    #[inline]
    fn common_word0(&mut self) -> SubSPHView<'_> {
        BitMutView::new_subset(&mut self.data, 0..32)
    }

    #[inline]
    fn common_word1(&mut self) -> SubSPHView<'_> {
        BitMutView::new_subset(&mut self.data, 32..64)
    }

    #[inline]
    fn common_word2(&mut self) -> SubSPHView<'_> {
        BitMutView::new_subset(&mut self.data, 64..96)
    }

    #[inline]
    fn common_word3<'a>(&mut self) -> SubSPHView<'_> {
        BitMutView::new_subset(&mut self.data, 96..128)
    }

    #[inline]
    fn common_word4(&mut self) -> SubSPHView<'_> {
        BitMutView::new_subset(&mut self.data, 128..160)
    }

    #[inline]
    fn imap_system_values_ab(&mut self) -> SubSPHView<'_> {
        BitMutView::new_subset(&mut self.data, 160..192)
    }

    #[inline]
    fn imap_g_vtg(&mut self) -> SubSPHView<'_> {
        assert!(self.shader_type != ShaderType::Fragment);

        BitMutView::new_subset(&mut self.data, 192..320)
    }

    #[inline]
    fn imap_g_ps(&mut self) -> SubSPHView<'_> {
        assert!(self.shader_type == ShaderType::Fragment);

        BitMutView::new_subset(&mut self.data, 192..448)
    }

    #[inline]
    fn imap_system_values_c(&mut self) -> SubSPHView<'_> {
        if self.shader_type == ShaderType::Fragment {
            BitMutView::new_subset(&mut self.data, 464..480)
        } else {
            BitMutView::new_subset(&mut self.data, 336..352)
        }
    }

    #[inline]
    fn omap_system_values_ab(&mut self) -> SubSPHView<'_> {
        assert!(self.shader_type != ShaderType::Fragment);
        BitMutView::new_subset(&mut self.data, 400..432)
    }

    #[inline]
    fn omap_g(&mut self) -> SubSPHView<'_> {
        assert!(self.shader_type != ShaderType::Fragment);

        BitMutView::new_subset(&mut self.data, 432..560)
    }

    #[inline]
    fn omap_system_values_c(&mut self) -> SubSPHView<'_> {
        assert!(self.shader_type != ShaderType::Fragment);
        BitMutView::new_subset(&mut self.data, 576..592)
    }

    #[inline]
    fn omap_target(&mut self) -> SubSPHView<'_> {
        assert!(self.shader_type == ShaderType::Fragment);

        BitMutView::new_subset(&mut self.data, 576..608)
    }

    #[inline]
    fn set_sph_type(&mut self, sph_type: u8, sph_version: u8) {
        let mut common_word0 = self.common_word0();

        common_word0.set_field(0..5, sph_type);
        common_word0.set_field(5..10, sph_version);
    }

    #[inline]
    fn set_shader_type(&mut self, shader_type: ShaderType) {
        self.common_word0().set_field(
            10..14,
            match shader_type {
                ShaderType::Vertex => 1_u8,
                ShaderType::TessellationControl => 2_u8,
                ShaderType::Tessellation => 3_u8,
                ShaderType::Geometry => 4_u8,
                ShaderType::Fragment => 5_u8,
            },
        );
    }

    #[inline]
    pub fn set_multiple_render_target_enable(&mut self, mrt_enable: bool) {
        self.common_word0().set_bit(14, mrt_enable);
    }

    #[inline]
    pub fn set_kills_pixels(&mut self, kills_pixels: bool) {
        self.common_word0().set_bit(15, kills_pixels);
    }

    #[inline]
    pub fn set_does_global_store(&mut self, does_global_store: bool) {
        self.common_word0().set_bit(16, does_global_store);
    }

    #[inline]
    pub fn set_sass_version(&mut self, sass_version: u8) {
        self.common_word0().set_field(17..21, sass_version);
    }

    #[inline]
    pub fn set_does_load_or_store(&mut self, does_load_or_store: bool) {
        self.common_word0().set_bit(26, does_load_or_store);
    }

    #[inline]
    pub fn set_does_fp64(&mut self, does_fp64: bool) {
        self.common_word0().set_bit(27, does_fp64);
    }

    #[inline]
    pub fn set_stream_out_mask(&mut self, stream_out_mask: u8) {
        self.common_word0().set_field(28..32, stream_out_mask);
    }

    #[inline]
    pub fn set_shader_local_memory_size(
        &mut self,
        shader_local_memory_size: u64,
    ) {
        assert!(shader_local_memory_size <= 0xffffffffffff);
        assert!(shader_local_memory_size % 0x10 == 0);

        let low = (shader_local_memory_size & 0xffffff) as u32;
        let high = ((shader_local_memory_size >> 32) & 0xffffff) as u32;

        self.common_word1().set_field(0..24, low);
        self.common_word2().set_field(0..24, high);
    }

    #[inline]
    pub fn set_per_patch_attribute_count(
        &mut self,
        per_patch_attribute_count: u8,
    ) {
        assert!(self.shader_type == ShaderType::TessellationControl);

        // Maxwell changed that encoding.
        if self.sm > 35 {
            self.common_word3()
                .set_field(28..32, per_patch_attribute_count & 0xf);
            self.common_word4()
                .set_field(20..24, per_patch_attribute_count >> 4);
        } else {
            self.common_word1()
                .set_field(24..32, per_patch_attribute_count);
        }
    }

    #[inline]
    pub fn set_threads_per_input_primitive(
        &mut self,
        threads_per_input_primitive: u8,
    ) {
        self.common_word2()
            .set_field(24..32, threads_per_input_primitive);
    }

    #[inline]
    pub fn set_shader_local_memory_crs_size(
        &mut self,
        shader_local_memory_crs_size: u32,
    ) {
        assert!(shader_local_memory_crs_size <= 0xffffff);
        self.common_word3()
            .set_field(0..24, shader_local_memory_crs_size);
    }

    #[inline]
    pub fn set_output_topology(&mut self, output_topology: OutputTopology) {
        self.common_word3().set_field(
            24..28,
            match output_topology {
                OutputTopology::PointList => 1_u8,
                OutputTopology::LineStrip => 6_u8,
                OutputTopology::TriangleStrip => 7_u8,
            },
        );
    }

    #[inline]
    pub fn set_max_output_vertex_count(
        &mut self,
        max_output_vertex_count: u16,
    ) {
        assert!(max_output_vertex_count <= 0xfff);
        self.common_word4()
            .set_field(0..12, max_output_vertex_count);
    }

    #[inline]
    pub fn set_store_req_start(&mut self, store_req_start: u8) {
        self.common_word4().set_field(12..20, store_req_start);
    }

    #[inline]
    pub fn set_store_req_end(&mut self, store_req_end: u8) {
        self.common_word4().set_field(24..32, store_req_end);
    }

    pub fn set_imap_system_values_ab(&mut self, val: u32) {
        self.imap_system_values_ab().set_field(0..32, val);
    }

    pub fn set_imap_system_values_c(&mut self, val: u16) {
        self.imap_system_values_c().set_field(0..16, val);
    }

    #[inline]
    pub fn set_imap_vector(&mut self, index: usize, value: u8) {
        assert!(index < 32);

        let (mut imap_g, elem_size) =
            if self.shader_type == ShaderType::Fragment {
                (self.imap_g_ps(), 8)
            } else {
                (self.imap_g_vtg(), 4)
            };

        imap_g.set_field(index * elem_size..(index + 1) * elem_size, value);
    }

    #[inline]
    pub fn set_imap_color(&mut self, value: u16) {
        self.set_field(320..336, value);
    }

    #[inline]
    pub fn set_omap_system_values_ab(&mut self, val: u32) {
        self.omap_system_values_ab().set_field(0..32, val);
    }

    #[inline]
    pub fn set_omap_system_values_c(&mut self, val: u16) {
        self.omap_system_values_c().set_field(0..16, val);
    }

    #[inline]
    pub fn set_omap_vector(&mut self, index: usize, value: u8) {
        assert!(index < 32);

        let (mut omap_g, elem_size) = (self.omap_g(), 4);
        omap_g.set_field(index * elem_size..(index + 1) * elem_size, value);
    }

    #[inline]
    pub fn set_omap_color(&mut self, value: u16) {
        self.set_field(560..576, value);
    }

    #[inline]
    pub fn set_omap_targets(&mut self, value: u32) {
        self.omap_target().set_field(0..32, value)
    }

    #[inline]
    pub fn set_omap_sample_mask(&mut self, sample_mask: bool) {
        assert!(self.shader_type == ShaderType::Fragment);
        self.set_bit(608, sample_mask);
    }

    #[inline]
    pub fn set_omap_depth(&mut self, depth: bool) {
        assert!(self.shader_type == ShaderType::Fragment);
        self.set_bit(609, depth);
    }
}
