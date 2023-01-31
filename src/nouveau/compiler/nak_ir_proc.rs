/*
 * Copyright © 2023 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

extern crate proc_macro;
extern crate proc_macro2;
#[macro_use]
extern crate quote;
extern crate syn;

use proc_macro::TokenStream;
use proc_macro2::{Span, TokenStream as TokenStream2};
use syn::*;

fn count_type(ty: &Type, search_type: &str) -> TokenStream2 {
    match ty {
        syn::Type::Array(a) => {
            let elems = count_type(a.elem.as_ref(), search_type);
            if !elems.is_empty() {
                let len = &a.len;
                quote! {((#elems) * (#len))}
            } else {
                TokenStream2::new()
            }
        }
        syn::Type::Path(p) => {
            if p.qself.is_none() && p.path.is_ident(search_type) {
                quote! {1_usize}
            } else {
                TokenStream2::new()
            }
        }
        _ => TokenStream2::new(),
    }
}

fn derive_as_slice(
    input: TokenStream,
    trait_name: &str,
    func_prefix: &str,
    search_type: &str,
) -> TokenStream {
    let DeriveInput {
        attrs, ident, data, ..
    } = parse_macro_input!(input);

    let trait_name = Ident::new(trait_name, Span::call_site());
    let elem_type = Ident::new(search_type, Span::call_site());
    let as_slice =
        Ident::new(&format!("{}_as_slice", func_prefix), Span::call_site());
    let as_mut_slice =
        Ident::new(&format!("{}_as_mut_slice", func_prefix), Span::call_site());

    match data {
        Data::Struct(s) => {
            let mut has_repr_c = false;
            for attr in attrs {
                match attr.meta {
                    Meta::List(ml) => {
                        if ml.path.is_ident("repr")
                            && format!("{}", ml.tokens) == "C"
                        {
                            has_repr_c = true;
                        }
                    }
                    _ => (),
                }
            }
            assert!(has_repr_c, "Struct must be declared #[repr(C)]");

            let mut first = None;
            let mut count = quote! {0_usize};
            let mut found_last = false;

            if let Fields::Named(named) = s.fields {
                for f in named.named {
                    let ty_count = count_type(&f.ty, search_type);
                    if !ty_count.is_empty() {
                        assert!(
                            !found_last,
                            "All fields of type {} must be consecutive",
                            search_type
                        );
                        first.get_or_insert(f.ident);
                        count.extend(quote! {+ #ty_count});
                    } else {
                        if !first.is_none() {
                            found_last = true;
                        }
                    }
                }
            } else {
                panic!("Fields are not named");
            }

            if let Some(name) = first {
                quote! {
                    impl #trait_name for #ident {
                        fn #as_slice(&self) -> &[#elem_type] {
                            unsafe {
                                let first = &self.#name as *const #elem_type;
                                std::slice::from_raw_parts(first, #count)
                            }
                        }

                        fn #as_mut_slice(&mut self) -> &mut [#elem_type] {
                            unsafe {
                                let first = &mut self.#name as *mut #elem_type;
                                std::slice::from_raw_parts_mut(first, #count)
                            }
                        }
                    }
                }
            } else {
                quote! {
                    impl #trait_name for #ident {
                        fn #as_slice(&self) -> &[#elem_type] {
                            &[]
                        }

                        fn #as_mut_slice(&mut self) -> &mut [#elem_type] {
                            &mut []
                        }
                    }
                }
            }
            .into()
        }
        Data::Enum(e) => {
            let mut as_slice_cases = TokenStream2::new();
            let mut as_mut_slice_cases = TokenStream2::new();
            for v in e.variants {
                let case = v.ident;
                as_slice_cases.extend(quote! {
                    #ident::#case(x) => x.#as_slice(),
                });
                as_mut_slice_cases.extend(quote! {
                    #ident::#case(x) => x.#as_mut_slice(),
                });
            }
            quote! {
                impl #trait_name for #ident {
                    fn #as_slice(&self) -> &[#elem_type] {
                        match self {
                            #as_slice_cases
                        }
                    }

                    fn #as_mut_slice(&mut self) -> &mut [#elem_type] {
                        match self {
                            #as_mut_slice_cases
                        }
                    }
                }
            }
            .into()
        }
        _ => panic!("Not a struct type"),
    }
}

#[proc_macro_derive(SrcsAsSlice)]
pub fn derive_srcs_as_slice(input: TokenStream) -> TokenStream {
    derive_as_slice(input, "SrcsAsSlice", "srcs", "Src")
}

#[proc_macro_derive(DstsAsSlice)]
pub fn derive_dsts_as_slice(input: TokenStream) -> TokenStream {
    derive_as_slice(input, "DstsAsSlice", "dsts", "Dst")
}

#[proc_macro_derive(SrcModsAsSlice)]
pub fn derive_src_mods_as_slice(input: TokenStream) -> TokenStream {
    derive_as_slice(input, "SrcModsAsSlice", "src_mods", "SrcMod")
}

#[proc_macro_derive(Display)]
pub fn enum_derive_display(input: TokenStream) -> TokenStream {
    let DeriveInput { ident, data, .. } = parse_macro_input!(input);

    if let Data::Enum(e) = data {
        let mut cases = TokenStream2::new();
        for v in e.variants {
            let case = v.ident;
            cases.extend(quote! {
                #ident::#case(x) => x.fmt(f),
            });
        }
        quote! {
            impl fmt::Display for #ident {
                fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                    match self {
                        #cases
                    }
                }
            }
        }
        .into()
    } else {
        panic!("Not an enum type");
    }
}
