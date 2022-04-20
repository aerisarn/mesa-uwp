use std::ops::Rem;

pub fn gcd<T>(mut a: T, mut b: T) -> T
where
    T: Copy + Default + PartialEq,
    T: Rem<Output = T>,
{
    let mut c = a % b;
    while c != T::default() {
        a = b;
        b = c;
        c = a % b;
    }

    b
}
