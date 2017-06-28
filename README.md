123456789012345678901234567890123456789012345678901234567890123456789012
# Black Frame Filter (bff)

Black Frame Filter is a native C++ command-line utility for processing
video files that contain spurious segments of black (or nearly-black)
frames. Whenever a black frame is encountered it is replaced with the
previous non-black frame.

This utility uses the developer libraries from FFmpeg/libav available
from [Zeranoe](https://ffmpeg.zeranoe.com/builds/).


# Requirements

*	Microsoft Visual Studio 2017
	*	Native Win32 C++ development options must be installed
*	FFmpeg dynamic libraries and their corresponding import
	libraries
	*	64-bit libraries are required
	*	Place the FFmpeg files in `C:\Developer\SDK\FFmpeg` with
		sub-folders `include`, `lib`, and `bin`


# Building

*	Open bff.sln and build
*	A post-build event script will copy the required dynamic
	libraries to the output folder


# Running

```
bff.exe --input infile --output outfile
```

The `infile` argument must contain a video stream and _may_ contain an
audio stream.

The `outfile` generated is always an MPEG-4 container. The video stream
is always H.264 encoded using *crf*=18 and the `yuv420p` pixel format.
An audio stream, if present, is always AAC encoded stereo at 128 kbps.


# License

Redistributed under a permissive open-source license. See the `LICENSE`
file for further details.


# Todo

- [ ]	Implement the `is_black_frame` function
- [ ]	Add deinterlacing
- [ ]	Refactor to permit greater sharing


# Notes

*	Statically links to [Getopt for Microsoft C](http://www.codeproject.com/KB/cpp/getopt4win.aspx)
	by Ludvik Jerabek which, according to the header file for it,
	contravenes LGPL

Please report all issues and defects through the normal GitHub issue
reporting mechanisms. Pull requests are appreciated!
