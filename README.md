# Black Frame Filter (bff)

This is a native C++ command-line utility that implements an libavfilter (FFmpeg) filter.
The filter examines each video frame. If a frame is black (or, more precisely, nearly black)
the frame will be replaced with the most recent non-black frame.


# Requirements

*	Microsoft Visual Studio 2017
	*	Native Win32 C++ development options must be installed
*	FFmpeg dynamic libraries and their corresponding import libraries
	*	64-bit libraries are required
	*	Place the FFmpeg files in `C:\Developer\SDK\FFmpeg` with sub-folders `include`, `lib`, and `bin`


# Building

*	Open bff.sln and build
*	A post-build event script will copy the required dynamic libraries to the output folder


# Running

```
bff.exe --input infile --output outfile
```

The `infile` argument must contain a video stream and _may_ contain an audio stream.

The `outfile` generated is always an MPEG-4 container. The video stream is always H.264
encoded using *crf*=18 and the `yuv420p` pixel format. An audio stream, if present, is
always AAC encoded stereo at 128 kbps.


# License

Redistributed under a permissive open-source license. See the LICENSE file for further
details.


# Todo

[ ]	Implement the `is_black_frame` function
[ ] Add deinterlacing
[ ] Refactor to permit greater sharing


# Notes

*	Statically links to [Getopt for Microsoft C](http://www.codeproject.com/KB/cpp/getopt4win.aspx)
	by Ludvik Jerabek which, according to the header file for it, contravenes LGPL

