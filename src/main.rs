#![feature(iter_array_chunks)]
#![feature(iter_intersperse)]

use elf::endian::AnyEndian;
use elf::ElfBytes;
use elf::ParseError;
use rustc_demangle::demangle;
use std::cmp::Ordering;
use std::collections::HashMap;
use std::env;
use std::ffi::OsString;
use std::fs;
use std::fs::File;
use std::io;
use std::io::BufReader;
use std::io::BufWriter;
use std::io::Read;
use std::io::Write;
use std::process::exit;

/// Loads the symbols list, sorted by address.
///
/// If no symbol table is present, the function returns `None`.
fn list_symbols(elf_buf: &[u8]) -> Result<Option<Vec<(u64, u64, String)>>, ParseError> {
    let elf = ElfBytes::<AnyEndian>::minimal_parse(&elf_buf)?;
    let symbol_table = elf.symbol_table()?;
    let Some((symbol_table, string_table)) = symbol_table else {
        return Ok(None);
    };

    let mut syms = symbol_table
        .iter()
        .map(|sym| {
            let addr = sym.st_value;
            let size = sym.st_size;
            let name = string_table.get(sym.st_name as _)?;
            let name = format!("{:#}", demangle(name));
            Ok::<(u64, u64, String), ParseError>((addr, size, name))
        })
        .collect::<Result<Vec<_>, _>>()?;
    syms.sort_unstable_by(|s1, s2| s1.0.cmp(&s2.0).then_with(|| s1.1.cmp(&s2.1)));
    Ok(Some(syms))
}

/// Returns the name of the symbol in which the address is located.
fn find_symbol<'s>(symbols: &'s [(u64, u64, String)], addr: u64) -> Option<&'s str> {
    let index = symbols
        .binary_search_by(|(start, size, _)| {
            if addr < *start {
                Ordering::Greater
            } else if addr >= *start + *size {
                Ordering::Less
            } else {
                Ordering::Equal
            }
        })
        .ok()?;
    Some(symbols[index].2.as_str())
}

fn main() -> io::Result<()> {
    let args: Vec<OsString> = env::args_os().collect();
    let [_, input_path, elf_path, output_path] = &args[..] else {
        eprintln!("usage: kern-profile <profile file> <elf file> <output file>");
        exit(1);
    };

    // Read profile data
    let input = File::open(input_path)?;
    let reader = BufReader::new(input);
    let mut iter = reader.bytes();

    // Read elf
    let elf_buf = fs::read(elf_path)?;
    // TODO handle error
    let Some(symbols) = list_symbols(&elf_buf).unwrap() else {
        eprintln!("ELF does not have a symbol table!");
        exit(1);
    };

    let mut folded_stacks: HashMap<Vec<&str>, usize> = HashMap::new();
    while let Some(stack_depth) = iter.next() {
        let stack_depth = stack_depth? as usize;
        let frames: Vec<_> = iter
            .by_ref()
            .take(stack_depth * 8)
            .map(|r| r.unwrap()) // TODO handle error
            .array_chunks()
            .map(u64::from_ne_bytes)
            .map(|addr| find_symbol(&symbols, addr).unwrap_or("???"))
            .collect();
        // Increment counter
        if let Some(c) = folded_stacks.get_mut(&frames) {
            *c += 1;
        } else {
            folded_stacks.insert(frames, 1);
        }
    }

    // Serialize
    let out = File::create(output_path)?;
    let mut writer = BufWriter::new(out);
    for (frames, count) in folded_stacks {
        let buff = frames
            .iter()
            .map(|f| f.as_bytes())
            .intersperse(&[b';'])
            .flatten();
        // TODO optimize (write buffers instead of byte-by-byte)
        for b in buff {
            writer.write_all(&[*b])?;
        }
        write!(writer, " {count}\n")?;
    }

    Ok(())
}
