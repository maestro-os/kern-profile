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
use std::io::BufWriter;
use std::io::Read;
use std::io::Write;
use std::io::{BufReader, Bytes};
use std::mem::size_of;
use std::process::{exit, Command, Stdio};

struct Symbol {
    addr: u64,
    size: u64,
    name: String,
}

/// Loads the symbols list, sorted by address.
///
/// If no symbol table is present, the function returns `None`.
fn list_symbols(elf_path: &OsString) -> Result<Option<Vec<Symbol>>> {
    let elf_buf = fs::read(elf_path)?;
    let elf = ElfBytes::<AnyEndian>::minimal_parse(&elf_buf)?;
    let symbol_table = elf.symbol_table()?;
    let Some((symbol_table, string_table)) = symbol_table else {
        return Ok(None);
    };

    let mut syms = symbol_table
        .iter()
        .map(|sym| {
            let name = string_table.get(sym.st_name as _)?;
            Ok::<_, ParseError>(Symbol {
                addr: sym.st_value,
                size: sym.st_size,
                name: format!("{:#}", demangle(name)),
            })
        })
        .collect::<Result<Vec<_>, _>>()?;
    syms.sort_unstable_by(|s1, s2| s1.addr.cmp(&s2.addr).then_with(|| s1.size.cmp(&s2.size)));
    Ok(Some(syms))
}

/// Returns the name of the symbol in which the address is located.
fn find_symbol(symbols: &[Symbol], addr: u64) -> Option<&str> {
    let index = symbols
        .binary_search_by(|sym| {
            if addr < sym.addr {
                Ordering::Greater
            } else if addr >= sym.addr + sym.size {
                Ordering::Less
            } else {
                Ordering::Equal
            }
        })
        .ok()?;
    Some(symbols[index].name.as_str())
}

/// TODO doc
fn stack_iter<'i, 's: 'i, I: Iterator<Item = io::Result<u8>>>(
    iter: &'i mut I,
    symbols: &'s [Symbol],
) -> io::Result<impl Iterator<Item = Option<&'s str>> + 'i> {
    let Some(stack_depth) = iter.next().transpose()? else {
        // TODO
        todo!()
    };
    let stack_depth = stack_depth as usize;
    Ok(iter
        .take(stack_depth * size_of::<u64>())
        .map(|r| r.unwrap()) // TODO handle error
        .array_chunks()
        .map(u64::from_le_bytes)
        .map(|addr| find_symbol(symbols, addr)))
}

/// TODO doc
fn next_u64<I: Iterator<Item = io::Result<u8>>>(iter: &mut I) -> io::Result<Option<u64>> {
    Ok(iter
        .map(|r| r.unwrap()) // TODO handle error
        .array_chunks()
        .map(u64::from_le_bytes)
        .next())
}

type FoldedStacks<'s> = HashMap<Vec<&'s str>, u64>;

/// Count the number of identical stacks.
///
/// The function returns a hashmap with each stack associated with its number of occurrences.
fn fold_stacks_cpu(iter: Bytes<BufReader<File>>, symbols: &[Symbol]) -> io::Result<FoldedStacks> {
    let mut iter = iter.peekable();
    let mut folded_stacks: FoldedStacks = HashMap::new();
    while iter.peek().is_some() {
        let mut frames = stack_iter(&mut iter, symbols)?.peekable();
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
            *folded_stacks.entry(substack).or_insert(0) += 1;
        }
    }
    Ok(folded_stacks)
}

/// Counts **net** allocated memory for each stack.
///
/// For each allocator, the function returns a hashmap with each stack associated with the quantity of allocated memory.
fn fold_stacks_memory(
    iter: Bytes<BufReader<File>>,
    symbols: &[Symbol],
) -> io::Result<HashMap<String, FoldedStacks>> {
    let mut iter = iter.peekable();
    let mut allocators: HashMap<String, HashMap<u64, (Vec<&str>, u64)>> = HashMap::new();
    while let Some(alloc_name_len) = iter.next() {
        let alloc_name_len = alloc_name_len? as usize;
        let name = iter
            .by_ref()
            .take(alloc_name_len)
            .map(|c| c.map(char::from))
            .collect::<io::Result<String>>()?;
        let Some(op) = iter.next().transpose()? else {
            break;
        };
        let Some(ptr) = next_u64(&mut iter)? else {
            break;
        };
        let Some(size) = next_u64(&mut iter)? else {
            break;
        };
        let frames = stack_iter(&mut iter, symbols)?
            .map(|s| s.unwrap_or("???"))
            .collect();
        // Update
        let entry = allocators.entry(name).or_insert(HashMap::new());
        let alloc = entry.entry(ptr).or_insert((frames, 0));
        match op {
            // Allocate or reallocate
            0 | 1 => alloc.1 = size,
            // Free
            2 => alloc.1 = 0,
            opcode => panic!("Invalid opcode `{opcode}`"),
        }
    }
    // Fold stacks
    Ok(allocators
        .into_iter()
        .map(|(allocator, allocations)| {
            let mut stacks = HashMap::new();
            for (_, (stack, size)) in allocations {
                *stacks.entry(stack).or_insert(0) += size;
            }
            (allocator, stacks)
        })
        .collect())
}

fn main() -> io::Result<()> {
    let mut args_iter = env::args_os().peekable();
    // Skip program name
    args_iter.next();
    let alloc = args_iter.next_if(|p| p == "--alloc").is_some();
    let args: Vec<OsString> = args_iter.collect();
    let [input_path, elf_path] = &args[..] else {
        eprintln!("usage: kern-profile [--alloc] <profile file> <elf file>");
        eprintln!();
        eprintln!("options:");
        eprintln!("\t--alloc: if set, the provided profile file contains memory allocator tracing. If not, it contains CPU tracing");
        eprintln!("\t<profile file>: path to the file containing samples recorded from execution");
        eprintln!("\t<elf file>: path to the observed kernel");
        eprintln!();
        eprintln!("On success, the command writes one or several Flamegraph(s) at `cpu.svg` for CPU tracing, or at `mem-<allocator>.svg` for memory tracing.");
        exit(1);
    };

    // Read ELF symbols
    let symbols = match list_symbols(elf_path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("Could not read ELF: {e}");
            exit(1);
        }
    };
    let Some(symbols) = symbols else {
        eprintln!("ELF does not have a symbol table!");
        exit(1);
    };

    // Read profile data
    let input = File::open(input_path)?;
    let reader = BufReader::new(input);
    let iter = reader.bytes();
    let graphs = if !alloc {
        let folded_stacks = fold_stacks_cpu(iter, &symbols)?;
        vec![("cpu.svg".into(), folded_stacks)]
    } else {
        let folded_stacks = fold_stacks_memory(iter, &symbols)?;
        folded_stacks
            .into_iter()
            .map(|(name, stacks)| (format!("mem-{name}.svg"), stacks))
            .collect()
    };

    // Produce flamegraphs
    for (output, stacks) in graphs {
        // Run flamegraph
        let mut cmd = Command::new("FlameGraph/flamegraph.pl");
        if alloc {
            cmd.args(&["--colors", "mem"]);
        }
        cmd.stdin(Stdio::piped());
        // Redirect output to file
        let file = File::create(output)?;
        cmd.stdout(file);
        // Run
        let child = cmd.spawn()?;
        // Serialize output
        let mut writer = BufWriter::new(child.stdin.unwrap());
        for (frames, count) in stacks {
            let buff = frames.into_iter().rev().intersperse(";");
            for b in buff {
                write!(writer, "{b}")?;
            }
            writeln!(writer, " {count}")?;
        }
    }

    Ok(())
}
