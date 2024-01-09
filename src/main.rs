#![feature(iter_array_chunks)]
#![feature(iter_intersperse)]

use anyhow::Result;
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
fn list_symbols(elf_path: &OsString) -> Result<Option<Vec<(u64, u64, String)>>> {
    let elf_buf = fs::read(elf_path)?;
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
fn find_symbol(symbols: &[(u64, u64, String)], addr: u64) -> Option<&str> {
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

/// Count the number of identical stacks.
///
/// The function returns a hashmap with each stack associated with its number of occurences.
fn fold_stacks<'s>(
    input_path: &OsString,
    symbols: &'s [(u64, u64, String)],
) -> io::Result<HashMap<Vec<&'s str>, u64>> {
    // Read profile data
    let input = File::open(input_path)?;
    let reader = BufReader::new(input);
    let mut iter = reader.bytes();

    let mut folded_stacks: HashMap<Vec<&str>, u64> = HashMap::new();
    while let Some(stack_depth) = iter.next() {
        // Read and convert to symbols
        let stack_depth = stack_depth? as usize;
        let mut frames: Vec<_> = iter
            .by_ref()
            .take(stack_depth * 8)
            .map(|r| r.unwrap()) // TODO handle error
            .array_chunks()
            .map(u64::from_ne_bytes)
            .map(|addr| find_symbol(symbols, addr))
            .collect();
        frames.reverse();
        let mut frames = frames.into_iter().peekable();

        // Subdivide stack into substacks (interruptions handling)
        while frames.peek().is_some() {
            let substack: Vec<_> = frames
                .by_ref()
                .take_while(Option::is_some)
                .map(|f| f.unwrap())
                .collect();
            if substack.is_empty() {
                continue;
            }

            // Increment counter
            if let Some(c) = folded_stacks.get_mut(&substack) {
                *c += 1;
            } else {
                folded_stacks.insert(substack, 1);
            }
        }
    }
    Ok(folded_stacks)
}

fn main() -> io::Result<()> {
    let args: Vec<OsString> = env::args_os().collect();
    let [_, input_path, elf_path, output_path] = &args[..] else {
        eprintln!("usage: kern-profile <profile file> <elf file> <output file>");
        exit(1);
    };

    // TODO handle error
    let Some(symbols) = list_symbols(elf_path).unwrap() else {
        eprintln!("ELF does not have a symbol table!");
        exit(1);
    };

    let folded_stacks = fold_stacks(input_path, &symbols)?;

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
        writeln!(writer, " {count}")?;
    }

    Ok(())
}
