/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */
#![allow(unstable_name_collisions)]

use crate::util::DivCeil;

use std::ops::{
    BitAnd, BitAndAssign, BitOr, BitOrAssign, BitXor, BitXorAssign, Not,
};

#[derive(Clone)]
pub struct BitSet {
    words: Vec<u32>,
}

impl BitSet {
    pub fn new() -> BitSet {
        BitSet { words: Vec::new() }
    }

    fn reserve_words(&mut self, words: usize) {
        if self.words.len() < words {
            self.words.resize(words, 0);
        }
    }

    pub fn reserve(&mut self, bits: usize) {
        self.reserve_words(bits.div_ceil(32));
    }

    pub fn clear(&mut self) {
        for w in self.words.iter_mut() {
            *w = 0;
        }
    }

    pub fn get(&self, idx: usize) -> bool {
        let w = idx / 32;
        let b = idx % 32;
        if w < self.words.len() {
            self.words[w] & (1_u32 << b) != 0
        } else {
            false
        }
    }

    pub fn next_set(&self, start: usize) -> Option<usize> {
        if start >= self.words.len() * 32 {
            return None;
        }

        let mut w = start / 32;
        let mut mask = u32::MAX << (start % 32);
        while w < self.words.len() {
            let b = (self.words[w] & mask).trailing_zeros();
            if b < 32 {
                return Some(w * 32 + usize::try_from(b).unwrap());
            }
            mask = 0;
            w += 1;
        }
        None
    }

    pub fn next_unset(&self, start: usize) -> usize {
        if start >= self.words.len() * 32 {
            return start;
        }

        let mut w = start / 32;
        let mut mask = !(u32::MAX << (start % 32));
        while w < self.words.len() {
            let b = (self.words[w] | mask).trailing_ones();
            if b < 32 {
                return w * 32 + usize::try_from(b).unwrap();
            }
            mask = 0;
            w += 1;
        }
        self.words.len() * 32
    }

    pub fn insert(&mut self, idx: usize) -> bool {
        let w = idx / 32;
        let b = idx % 32;
        self.reserve_words(w + 1);
        let exists = self.words[w] & (1_u32 << b) != 0;
        self.words[w] |= 1_u32 << b;
        !exists
    }

    pub fn remove(&mut self, idx: usize) -> bool {
        let w = idx / 32;
        let b = idx % 32;
        self.reserve_words(w + 1);
        let exists = self.words[w] & (1_u32 << b) != 0;
        self.words[w] &= !(1_u32 << b);
        exists
    }

    pub fn words(&self) -> &[u32] {
        &self.words
    }

    pub fn words_mut(&mut self) -> &mut [u32] {
        &mut self.words
    }

    pub fn union_with(&mut self, other: &BitSet) -> bool {
        let mut added_bits = false;
        self.reserve_words(other.words.len());
        for w in 0..other.words.len() {
            let uw = self.words[w] | other.words[w];
            if uw != self.words[w] {
                added_bits = true;
                self.words[w] = uw;
            }
        }
        added_bits
    }
}

impl BitAndAssign for BitSet {
    fn bitand_assign(&mut self, rhs: BitSet) {
        self.reserve_words(rhs.words.len());
        for w in 0..rhs.words.len() {
            self.words[w] &= rhs.words[w];
        }
    }
}

impl BitAnd<BitSet> for BitSet {
    type Output = BitSet;

    fn bitand(self, rhs: BitSet) -> BitSet {
        let mut res = self;
        res.bitand_assign(rhs);
        res
    }
}

impl BitOrAssign for BitSet {
    fn bitor_assign(&mut self, rhs: BitSet) {
        self.reserve_words(rhs.words.len());
        for w in 0..rhs.words.len() {
            self.words[w] |= rhs.words[w];
        }
    }
}

impl BitOr<BitSet> for BitSet {
    type Output = BitSet;

    fn bitor(self, rhs: BitSet) -> BitSet {
        let mut res = self;
        res.bitor_assign(rhs);
        res
    }
}

impl BitXorAssign for BitSet {
    fn bitxor_assign(&mut self, rhs: BitSet) {
        self.reserve_words(rhs.words.len());
        for w in 0..rhs.words.len() {
            self.words[w] ^= rhs.words[w];
        }
    }
}

impl BitXor<BitSet> for BitSet {
    type Output = BitSet;

    fn bitxor(self, rhs: BitSet) -> BitSet {
        let mut res = self;
        res.bitxor_assign(rhs);
        res
    }
}

impl Not for BitSet {
    type Output = BitSet;

    fn not(self) -> BitSet {
        let mut res = self;
        for w in 0..res.words.len() {
            res.words[w] = !res.words[w];
        }
        res
    }
}
