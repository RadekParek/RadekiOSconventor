# Third-Party Notices

## Dynarmic

Android and host builds of `compat-runtime-v1` fetch Dynarmic from the
`suyu-emu/dynarmic` repository (the actively maintained continuation of
MerryMage's dynarmic ARM JIT) at commit
`a2ef15df6d833ab7bc66131a90350637f26dd2d4`, and build only its A32 (ARM32)
frontend. Dynarmic is distributed under the 0BSD license:

> Copyright (c) 2016 MerryMage
>
> Permission to use, copy, modify, and/or distribute this software for any
> purpose with or without fee is hereby granted, provided that the above
> copyright notice and this permission notice appear in all copies.
>
> THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
> WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
> MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
> SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
> WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
> OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
> CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

Its bundled externals (fmt, mcl, oaknut, robin-map, xbyak) carry their own
permissive licenses (fmt, mcl, oaknut and robin-map: MIT; xbyak: BSD-3-Clause)
and are compiled statically into `libcompat_runtime_v1.so`. No Dynarmic source
or binary is vendored in this repository.

Dynarmic replaces the previously used Unicorn Engine backend; the ARM32
execution boundary, callout contract and guest-memory contract are unchanged.
