This tool allows profiling the CPU usage of a Rust kernel running in QEMU, using flamegraphs. More specifically, this has been written for [Maestro](https://github.com/llenotre/maestro).

The repository contains the following components:
- a QEMU TCG plugin (written in C) for data acquisition (CPU profiling only)
- an aggregator (written in Rust) to convert the data into a form that can be processed by `flamegraph.pl`

The aggregator tool outputs SVG one or several SVG file(s) with the desired FlameGraph(s).



## Build

This section describes building the aggregator tool. CPU profiling also requires building the QEMU plugin.

First, make sure you FlameGraph is present:

```shell
git submodule init
```

Then, compile the aggregator:

```sh
cargo +nightly build --release
```



## CPU Profiling

It works by sampling the execution of the code at a given rate.
For each sample, the plugin collects the current callstack of the code being executed.

The assumption is made that more time the CPU spends executing a function, the higher is the probability the function shows up in the callstack.

Aggregating all the callstacks together allows building the flamegraph.



### Requirements

QEMU version 8.2.0 **exactly** is required (for CPU profiling only).

**Note**: another version of QEMU cannot be used because the API is not guaranteed to be consistent from one version to another

If compiling QEMU yourself, the option `--enable-modules` must be passed to `./configure`.



### Build

First, go in the `plugin/` directory.

Build the QEMU plugin using:

```sh
QEMU_SRC=<path-to-QEMU-sources> make
```



### Usage

First, make sure:
- The kernel is compiled with the option `-Cforce-frame-pointers=yes` on `rustc`
- Kernel symbols are present (not stripped)

Run QEMU with the plugin by adding the following argument (adapt parameters to your needs):

```sh
-plugin 'kern-profile.so,out=raw-data,delay=10'
```

Arguments:
- `out` is the path to the output file
- `delay` (optional) is the amount of microseconds between each sample

The output file can then be processed by the aggregator:

```sh
kern-profile raw-data <path-to-kernel-ELF>
```



## Memory profiling

For memory profiling, the tool uses the data output by the `memtrace` feature of the [Maestro kernel](https://github.com/llenotre/maestro).

After collecting memtrace sample, you just have to run the aggregator tool:

```sh
kern-profile --alloc <path-to-memtrace-data> <path-to-kernel-ELF>
```



## Caveats/missing features

The following issues need to be fixed in the future:
- Only one CPU core is supported
- Only x86 in 32 bits is supported
- Only the kernel can be profiled. It is not possible to load/observe several ELF at once (either kernel modules or userspace programs)
