/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use std::ops::Mul;

/** Provides div_ceil()
 *
 * This is a nightly feature so we can't rely on it yet.
 * https://github.com/rust-lang/rust/issues/88581
 */
pub trait DivCeil<Rhs = Self> {
    type Output;

    fn div_ceil(&self, a: Rhs) -> Self::Output;
}

macro_rules! impl_uint_div_ceil {
    ($ty: ty) => {
        impl DivCeil<$ty> for $ty {
            type Output = $ty;

            fn div_ceil(&self, d: $ty) -> $ty {
                (*self + (d - 1)) / d
            }
        }
    };
}

impl_uint_div_ceil!(u8);
impl_uint_div_ceil!(u16);
impl_uint_div_ceil!(u32);
impl_uint_div_ceil!(u64);
impl_uint_div_ceil!(usize);

/** Provides next_multiple_of()
 *
 * This is a nightly feature so we can't rely on it yet.
 * https://github.com/rust-lang/rust/issues/88581
 */
pub trait NextMultipleOf<Rhs = Self> {
    type Output;

    fn next_multiple_of(&self, a: Rhs) -> Self::Output;
}

impl<T: Copy + DivCeil<Output = T> + Mul<Output = T>> NextMultipleOf<T> for T {
    type Output = T;

    fn next_multiple_of(&self, a: T) -> T {
        self.div_ceil(a) * a
    }
}
