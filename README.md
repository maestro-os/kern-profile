This tool allows to profile the CPU usage of a Rust kernel running in QEMU, using flamegraphs.

It works by sampling the execution of the code at a given rate.
For each sample, the plugin collects the current callstack of the code being executed.

The assumption is made that more time the CPU spends executing a function, the higher is the probability the function shows up in the callstack.

Aggregating all the callstacks together allows to build the flamegraph.

The repository contains the following components:
- a QEMU TCG plugin (written in C) for data acquisition
- an aggregator (written in Rust) to convert the data into a form that can be processed by `flamegraph.pl`



## Requirements

The following programs are required:
- QEMU version 8.2.0 **exactly**
- [Flamegraph](https://github.com/brendangregg/FlameGraph)

**Note**: another version of QEMU cannot be used because the API can change from a version to another



## Build

Build the QEMU plugin using:

```sh
QEMU_SRC=<path to QEMU sources> make
```

Then, build the aggregator:

```sh
cargo build --release
```



## Usage

First, make sure the kernel is compiled with the `-Cforce-frame-pointers=yes` option on `rustc`.

Run QEMU with the plugin by adding the following argument (adapt parameters to your needs):

```sh
-plugin 'kern-profile.so,out=raw-data,delay=10'
```

Arguments:
- `out` is the path to the output file
- `delay` (optional) is the amount of microseconds between each sample

The output file can then be processed by the aggregator:

```sh
target/release/kern-profile raw-data <path to kernel ELF> flamegraph-input
```

Then, you can generate the flamegraph with:

```sh
cat flamegraph-input | flamegraph.pl >flamegraph.svg
```



## Caveats/missing features

The following issues need to be fixed in the future:
- Only one CPU core is supported
- Only x86 in 32 bits is supported
- Only the kernel can be profiled. It is not possible to load/observe several ELF at once (either kernel modules or userspace programs)
- The plugin does not allow to generate [memory flamegraphs](https://www.brendangregg.com/FlameGraphs/memoryflamegraphs.html)
