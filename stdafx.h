/*	bff - Black Frame Filter for FFmpeg
	Copyright (C) 2017 Michael Trenholm-Boyle.
	This software is redistributable under a permissive open source license.
	See the LICENSE file for further information. */
#pragma once

#include "targetver.h"
#include <Windows.h>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <tchar.h>
#include <memory>
#include <functional>

extern "C" {
#include <libavutil\avutil.h>
#include <libavcodec\avcodec.h>
#include <libavformat\avformat.h>
#include <libavfilter\avfilter.h>
}
