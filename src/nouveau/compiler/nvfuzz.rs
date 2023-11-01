/*
 * Copyright © 2022 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

mod bitview;

use crate::bitview::*;

use std::fs;
use std::io::Write;
use std::ops::Range;
use std::path::PathBuf;
use std::process::Command;

const TMP_FILE: &str = "/tmp/nvfuzz";
const SM: &str = "SM75";

fn find_cuda() -> std::io::Result<PathBuf> {
    let paths = fs::read_dir("/usr/local")?;

    for path in paths {
        let mut path = path?.path();
        let Some(fname) = path.file_name() else {
            continue;
        };

        let Some(fname) = fname.to_str() else {
            continue;
        };

        if !fname.starts_with("cuda-") {
            continue;
        }

        path.push("bin");
        path.push("nvdisasm");
        if path.exists() {
            return Ok(path)
        }
    }

    Err(std::io::Error::new(
        std::io::ErrorKind::NotFound,
        "Failed to find nvdisasm",
    ))
}

//fn write_tmpfile(data: &[u32]) -> std::io::Result<()> {
//    let mut file = std::fs::File::create(TMP_FILE)?;
//    for dw in data {
//        file.write(dw.to_le_bytes())?;
//    }
//}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let range: Vec<&str> = args[1].split("..").collect();
    let range: Range<usize> = Range {
        start: range[0].parse().unwrap(),
        end: range[1].parse().unwrap(),
    };

    let mut instr: [u32; 4] = [
        u32::from_str_radix(&args[2], 16).unwrap(),
        u32::from_str_radix(&args[3], 16).unwrap(),
        u32::from_str_radix(&args[4], 16).unwrap(),
        u32::from_str_radix(&args[5], 16).unwrap(),
    ];

    let cuda_path = find_cuda().expect("Failed to find CUDA");

    for bits in 0..(1_u64 << range.len()) {
        BitMutView::new(&mut instr).set_field(range.clone(), bits);

        print!("With {:#x} in {}..{}:", bits, range.start, range.end);
        for dw in instr {
            print!(" {:#x}", dw);
        }
        print!("\n");

        let mut data = Vec::new();
        for dw in instr {
            data.extend(dw.to_le_bytes());
        }
        std::fs::write(TMP_FILE, data).expect("Failed to write file");

        let out = Command::new(cuda_path.as_path())
            .arg("-b")
            .arg(SM)
            .arg(TMP_FILE)
            .output()
            .expect("failed to execute process");
        std::io::stderr().write_all(&out.stderr).expect("IO error");
        std::io::stdout().write_all(&out.stdout).expect("IO error");
    }
}
