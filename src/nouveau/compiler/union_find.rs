/*
 * Copyright © 2023 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

use std::cell::UnsafeCell;
use std::collections::HashMap;
use std::hash::Hash;
use std::ptr::NonNull;

#[derive(Clone, Copy)]
struct Node<K> {
    rank: u32,
    parent: K,
}

pub struct HashSetForest<K> {
    nodes: HashMap<K, UnsafeCell<Node<K>>>,
}

impl<K: Copy + Eq + Hash> HashSetForest<K> {
    pub fn new() -> Self {
        HashSetForest {
            nodes: HashMap::new(),
        }
    }

    fn repr_recur(&self, k: K, n: &UnsafeCell<Node<K>>) -> K {
        let p = unsafe { *n.get() }.parent;
        if p == k {
            k
        } else {
            self.repr_recur(p, self.nodes.get(&p).unwrap())
        }
    }

    pub fn repr(&self, k: K) -> K {
        if let Some(n) = self.nodes.get(&k) {
            self.repr_recur(k, &n)
        } else {
            k
        }
    }

    pub fn equiv(&self, x: K, y: K) -> bool {
        self.repr(x) == self.repr(y)
    }

    unsafe fn get_set_recur(
        &mut self,
        k: K,
        n: *mut Node<K>,
    ) -> (K, *mut Node<K>) {
        let pk = (*n).parent;
        if pk == k {
            (k, n)
        } else {
            let pn = self.nodes.get(&pk).unwrap().get();
            let (pk, pn) = self.get_set_recur(pk, pn);

            // The recurion returning means we didn't have any cycles in the
            // tree (we'd better not have) and this is the only reference to
            // our node so it's safe to mutate.
            (*n).parent = pk;

            (pk, pn)
        }
    }

    fn get_set(&mut self, k: K) -> (K, *mut Node<K>) {
        let n = self
            .nodes
            .entry(k)
            .or_insert_with(|| UnsafeCell::new(Node { rank: 0, parent: k }))
            .get();
        unsafe { self.get_set_recur(k, n) }
    }

    fn union(&mut self, x: K, y: K) -> bool {
        if x == y {
            return false;
        }

        let (xk, xn) = self.get_set(x);
        let (yk, yn) = self.get_set(y);
        if xk == yk {
            return false;
        }

        // These are different nodes now so it's safe to get mut references
        assert!(xn != yn);
        let xn = unsafe { &mut *xn };
        let yn = unsafe { &mut *yn };

        if xn.rank > yn.rank {
            yn.parent = xk;
        } else {
            xn.parent = yk;
            if xn.rank == yn.rank {
                yn.rank += 1;
            }
        }
        true
    }
}
