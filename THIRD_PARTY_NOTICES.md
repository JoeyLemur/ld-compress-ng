# Third-Party Notices

This project links against the system `libFLAC` and `libogg` libraries for the
CPU/Ogg FLAC path. The native FLAC encoder is project code, but its FLAC
analysis work is checked against the Xiph.org FLAC reference implementation
under `reference/flac/`.

The OpenCL analysis path includes a reduced, locally modified kernel slice
derived from CUETools.FLACCL. Preserve the LGPL notice in the kernel source and
keep local modification notes when changing that code. A copy of the LGPL 2.1
license text is provided at `LICENSES/LGPL-2.1-or-later.txt`.

## CUETools.FLACCL OpenCL Kernels

Portions of `src/opencl_analysis.cpp` are derived from the CUETools.FLACCL
OpenCL encoder kernels for mono wasted-bits and residual-size analysis.

```text
CUETools.FLACCL: FLAC audio encoder using OpenCL
Copyright (c) 2010-2022 Gregory S. Chudov

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
Lesser General Public License for more details.
```

## Xiph.org FLAC Reference

Portions of the native FLAC analysis and tuning work are informed by the
Xiph.org FLAC reference source. Preserve this notice when porting additional
implementation details.

```text
libFLAC - Free Lossless Audio Codec library
Copyright (C) 2000-2009  Josh Coalson
Copyright (C) 2011-2025  Xiph.Org Foundation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

- Neither the name of the Xiph.org Foundation nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
