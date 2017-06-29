/*	bff - Black Frame Filter for FFmpeg
	Copyright (C) 2017 Michael Trenholm-Boyle.
	This software is redistributable under a permissive open source license.
	See the LICENSE file for further information. */
#include "stdafx.h"

#include "bff.h"
extern "C" {
#include <libavutil\avutil.h>
#include <libavcodec\avcodec.h>
#include <libavformat\avformat.h>
#include <libavfilter\avfilter.h>
#include <libavutil\opt.h>
#include <libavutil\avstring.h>
#include <libavutil\imgutils.h>
#include <libswscale\swscale.h>
#include <libswresample\swresample.h>
#include <libavfilter\buffersrc.h>
#include <libavfilter\buffersink.h>
}

static bool is_black_frame(AVFrame *frame);

int wmain(int argc, wchar_t ** argv)
{
	cliopts opts(argc, argv);
	if(opts.check_syntax())
	{
		opts.print_syntax_help();
		return 1;
	}
	int rv = bff(opts);
	return rv;
}

int bff(const cliopts & opts)
{
	int rv;
	av_register_all();
	avfilter_register_all();
	// statistics to display
	uint64_t video_frame_count = 0, audio_frame_count = 0;
	uint64_t video_packet_count = 0, audio_packet_count = 0;
	// open input
	AVFormatContext *p = nullptr;
	rv = avformat_open_input(&p, ansi(opts.input).c_str(), nullptr, nullptr);
	if (rv < 0) {
		char msg[100] = { 0 };
		av_strerror(rv, msg, sizeof(msg) - 1);
		std::cerr << "error:\tcannot open input file: " << ansi(opts.input) << std::endl;
		std::cerr << "\t" << msg << std::endl;
		return rv;
	}
	std::unique_ptr<AVFormatContext, std::function<void(AVFormatContext*)>> informat(p, [](AVFormatContext *p) {
		avformat_close_input(&p);
	});
	{
		rv = avformat_find_stream_info(informat.get(), nullptr);
		if (rv < 0) {
			char msg[100] = { 0 };
			av_strerror(rv, msg, sizeof(msg) - 1);
			std::cerr << "error:\tcannot find stream info" << std::endl;
			std::cerr << "\t" << msg << std::endl;
			return rv;
		}
		AVCodec *q = nullptr;
		rv = av_find_best_stream(informat.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &q, 0);
		if (rv < 0) {
			char msg[100] = { 0 };
			av_strerror(rv, msg, sizeof(msg) - 1);
			std::cerr << "error:\tcannot find video stream in input file" << std::endl;
			std::cerr << "\t" << msg << std::endl;
			return rv;
		}
		int video_stream_index = rv;
		std::unique_ptr<AVCodecContext, std::function<void(AVCodecContext*)>> invcodec(avcodec_alloc_context3(q), [](AVCodecContext *p) {
			avcodec_free_context(&p);
		});
		rv = avcodec_parameters_to_context(invcodec.get(), informat->streams[video_stream_index]->codecpar);
		if (rv < 0) {
			char msg[100] = { 0 };
			av_strerror(rv, msg, sizeof(msg) - 1);
			std::cerr << "error:\tfailed to set input video codec parameters (" << rv << ")" << std::endl;
			std::cerr << "\t" << msg << std::endl;
			return rv;
		}
		rv = av_opt_set_int(invcodec.get(), "refcounted_frames", 1, 0);
		if (rv < 0) {
			char msg[100] = { 0 };
			av_strerror(rv, msg, sizeof(msg) - 1);
			std::cerr << "error:\tfailed to set input video codec option \"refcounted_frames\" (" << rv << ")" << std::endl;
			std::cerr << "\t" << msg << std::endl;
			return rv;
		}
		rv = avcodec_open2(invcodec.get(), q, nullptr);
		if (rv < 0) {
			char msg[100] = { 0 };
			av_strerror(rv, msg, sizeof(msg) - 1);
			std::cerr << "error:\tcannot open input video codec (" << rv << ")" << std::endl;
			std::cerr << "\t" << msg << std::endl;
			return rv;
		}
		q = nullptr;
		rv = av_find_best_stream(informat.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &q, 0);
		bool has_audio = rv >= 0;
		int audio_stream_index = has_audio ? rv : -1;
		std::unique_ptr<AVCodecContext, std::function<void(AVCodecContext*)>> inacodec(has_audio ? avcodec_alloc_context3(q) : nullptr, [](AVCodecContext *p) {
			if (p) {
				avcodec_free_context(&p);
			}
		});
		if (has_audio) {
			rv = avcodec_parameters_to_context(inacodec.get(), informat->streams[audio_stream_index]->codecpar);
			if (rv < 0) {
				char msg[100] = { 0 };
				av_strerror(rv, msg, sizeof(msg) - 1);
				std::cerr << "error:\tfailed to set input audio codec parameters (" << rv << ")" << std::endl;
				std::cerr << "\t" << msg << std::endl;
				return rv;
			}
			rv = avcodec_open2(inacodec.get(), q, nullptr);
			if (rv < 0) {
				char msg[100] = { 0 };
				av_strerror(rv, msg, sizeof(msg) - 1);
				std::cerr << "error:\tcannot open input audio codec (" << rv << ")" << std::endl;
				std::cerr << "\t" << msg << std::endl;
				return rv;
			}
		}
		// open output
		std::string fname = ansi(opts.output);
		struct stat st = { 0 };
		if (stat(fname.c_str(), &st) == 0) {
			std::cerr << "warn:\toutput file " << fname << " already exists and will be deleted" << std::endl;
			if (_unlink(fname.c_str()) != 0) {
				rv = errno;
				char * msg = _strdup(strerror(rv));
				std::cerr << "error:\tfailed to delete output file" << std::endl;
				std::cerr << "\t" << msg << std::endl;
				free(msg);
				return rv;
			}
		}
		std::unique_ptr<AVFormatContext, std::function<void(AVFormatContext*)>> oformat(avformat_alloc_context(), [](AVFormatContext *p) {
			avformat_free_context(p);
		});
		{
			rv = avio_open(&(oformat->pb), fname.c_str(), AVIO_FLAG_READ_WRITE);
			if (rv < 0) {
				char msg[100] = { 0 };
				av_strerror(rv, msg, sizeof(msg) - 1);
				std::cerr << "error:\tfailed to open output file (" << rv << ")" << std::endl;
				std::cerr << "\t" << msg << std::endl;
				return rv;
			}
			oformat->oformat = av_guess_format("mp4", nullptr, nullptr);
			av_strlcpy(oformat->filename, fname.c_str(), sizeof(oformat->filename));
			AVCodec * h264 = avcodec_find_encoder(AV_CODEC_ID_H264);
			std::unique_ptr<AVStream, std::function<void(AVStream*)>> ovstream(avformat_new_stream(oformat.get(), h264), [](AVStream *p) {});
			{
				std::unique_ptr<AVCodecContext, std::function<void(AVCodecContext*)>> ovcodec(avcodec_alloc_context3(h264), [](AVCodecContext *p) {
					avcodec_free_context(&p);
				});
				{
					ovcodec->pix_fmt = AV_PIX_FMT_YUV420P;
					ovcodec->width = invcodec->width;
					ovcodec->height = invcodec->height;
					ovcodec->framerate = informat->streams[video_stream_index]->avg_frame_rate;
					ovcodec->sample_aspect_ratio = invcodec->sample_aspect_ratio;
					ovcodec->time_base = informat->streams[video_stream_index]->time_base;
					std::unique_ptr<AVDictionary*, std::function<void(AVDictionary**)>> vopts((AVDictionary **)calloc(1, sizeof(AVDictionary*)), [](AVDictionary **p) {
						if (*p) {
							av_dict_free(p);
						}
						if (p) {
							free(p);
						}
					});
					av_dict_set(vopts.get(), "profile", "Main", 0);
					av_dict_set(vopts.get(), "level", "4.1", 0);
					av_dict_set(vopts.get(), "preset", "slow", 0);
					av_dict_set(vopts.get(), "crf", "18", 0);
					rv = avcodec_open2(ovcodec.get(), h264, vopts.get());
					if (rv < 0) {
						char msg[100] = { 0 };
						av_strerror(rv, msg, sizeof(msg) - 1);
						std::cerr << "error:\tfailed to open H.264 codec for output (" << rv << ")" << std::endl;
						std::cerr << "\t" << msg << std::endl;
						return rv;
					}
					rv = avcodec_parameters_from_context(ovstream->codecpar, ovcodec.get());
					if (rv < 0) {
						char msg[100] = { 0 };
						av_strerror(rv, msg, sizeof(msg) - 1);
						std::cerr << "error:\tfailed to initialize output video codec parameters (" << rv << ")" << std::endl;
						std::cerr << "\t" << msg << std::endl;
						return rv;
					}
					if (oformat->oformat->flags & AVFMT_GLOBALHEADER) {
						ovcodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
					}
					ovstream->time_base = ovcodec->time_base;
					AVCodec * aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
					std::unique_ptr<AVStream, std::function<void(AVStream*)>> oastream(has_audio ? avformat_new_stream(oformat.get(), aac) : nullptr, [](AVStream *p) {});
					{
						std::unique_ptr<AVCodecContext, std::function<void(AVCodecContext*)>> oacodec(has_audio ? avcodec_alloc_context3(aac) : nullptr, [](AVCodecContext *p) {
							if (p) {
								avcodec_free_context(&p);
							}
						});
						{
							if (has_audio) {
								oacodec->sample_rate = 48000;
								oacodec->channel_layout = AV_CH_LAYOUT_STEREO;
								oacodec->channels = 2;
								oacodec->sample_fmt = AV_SAMPLE_FMT_FLTP;
								oacodec->time_base = inacodec->time_base;
								rv = avcodec_open2(oacodec.get(), aac, nullptr);
								if (rv < 0) {
									char msg[100] = { 0 };
									av_strerror(rv, msg, sizeof(msg) - 1);
									std::cerr << "error:\tfailed to open AAC codec for output (" << rv << ")" << std::endl;
									std::cerr << "\t" << msg << std::endl;
									return rv;
								}
								rv = avcodec_parameters_from_context(oastream->codecpar, oacodec.get());
								if (rv < 0) {
									char msg[100] = { 0 };
									av_strerror(rv, msg, sizeof(msg) - 1);
									std::cerr << "error:\tfailed to initialize output audio codec parameters (" << rv << ")" << std::endl;
									std::cerr << "\t" << msg << std::endl;
									return rv;
								}
								if (oformat->oformat->flags & AVFMT_GLOBALHEADER) {
									oacodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
								}
								oastream->time_base = oacodec->time_base;
							}
							rv = avformat_write_header(oformat.get(), nullptr);
							if (rv < 0) {
								char msg[100] = { 0 };
								av_strerror(rv, msg, sizeof(msg) - 1);
								std::cerr << "error:\tfailed to write output file header (" << rv << ")" << std::endl;
								std::cerr << "\t" << msg << std::endl;
								return rv;
							}
							bool sws_required = (invcodec->pix_fmt != ovcodec->pix_fmt) || (invcodec->width != ovcodec->width) || (invcodec->height != ovcodec->height);
							std::unique_ptr<SwsContext, std::function<void(SwsContext*)>> sws(sws_required ? sws_getCachedContext(nullptr, invcodec->width, invcodec->height, invcodec->pix_fmt, ovcodec->width, ovcodec->height, ovcodec->pix_fmt, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr) : nullptr, [](SwsContext *p) {
								if (p) {
									sws_freeContext(p);
								}
							});
							bool swr_required = has_audio && ((inacodec->sample_fmt != oacodec->sample_fmt) || (inacodec->sample_rate != oacodec->sample_rate) || (inacodec->channels != oacodec->channels) || (inacodec->channel_layout != oacodec->channel_layout));
							std::unique_ptr<SwrContext, std::function<void(SwrContext*)>> swr(swr_required ? swr_alloc_set_opts(nullptr, oacodec->channel_layout, oacodec->sample_fmt, oacodec->sample_rate, inacodec->channel_layout, inacodec->sample_fmt, inacodec->sample_rate, 0, nullptr) : nullptr, [](SwrContext *p) {
								if (p) {
									swr_free(&p);
								}
							});
							// configure filter graph for deinterlacing
							std::unique_ptr<AVFilterGraph, std::function<void(AVFilterGraph*)>> filter_graph(avfilter_graph_alloc(), [](AVFilterGraph *p) {
								avfilter_graph_free(&p);
							});
							AVFilterContext * bufferctx = nullptr;
							AVFilterContext * buffersinkctx = nullptr;
							{
								AVFilter * buffer = avfilter_get_by_name("buffer");
								if (!buffer) {
									std::cerr << "error:\tno buffer filter" << std::endl;
									return 2;
								}
								AVFilter * buffersink = avfilter_get_by_name("buffersink");
								if (!buffersink) {
									std::cerr << "error:\tno buffersink filter" << std::endl;
									return 2;
								}
								const size_t arglen = 32*32;
								char * args = (char *)alloca(arglen);
								memset(args, 0, arglen);
								AVRational time_base = ovcodec->time_base;
								snprintf(args, arglen, "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", ovcodec->width, ovcodec->height, ovcodec->pix_fmt, time_base.num, time_base.den, ovcodec->sample_aspect_ratio.num, ovcodec->sample_aspect_ratio.den);
								rv = avfilter_graph_create_filter(&bufferctx, buffer, "in", args, nullptr, filter_graph.get());
								if (rv < 0) {
									char msg[100] = { 0 };
									av_strerror(rv, msg, sizeof(msg) - 1);
									std::cerr << "error:\tfailed to create in filter (" << rv << ")" << std::endl;
									std::cerr << "\t" << msg << std::endl;
									return rv;
								}
								rv = avfilter_graph_create_filter(&buffersinkctx, buffersink, "out", nullptr, nullptr, filter_graph.get());
								if (rv < 0) {
									char msg[100] = { 0 };
									av_strerror(rv, msg, sizeof(msg) - 1);
									std::cerr << "error:\tfailed to create out filter (" << rv << ")" << std::endl;
									std::cerr << "\t" << msg << std::endl;
									return rv;
								}
								AVFilterInOut * inputs = avfilter_inout_alloc(), *outputs = avfilter_inout_alloc();
								if (!outputs || !inputs) {
									std::cerr << "error:\tfilter inout allocation failure" << std::endl;
									return 2;
								}
								outputs->name = av_strdup("in");
								outputs->filter_ctx = bufferctx;
								outputs->pad_idx = 0;
								outputs->next = nullptr;
								inputs->name = av_strdup("out");
								inputs->filter_ctx = buffersinkctx;
								inputs->pad_idx = 0;
								inputs->next = nullptr;
								rv = avfilter_graph_parse_ptr(filter_graph.get(), "kerndeint", &inputs, &outputs, nullptr);
								avfilter_inout_free(&inputs);
								avfilter_inout_free(&outputs);
								if (rv < 0) {
									char msg[100] = { 0 };
									av_strerror(rv, msg, sizeof(msg) - 1);
									std::cerr << "error:\tfailed to parse filter expr (" << rv << ")" << std::endl;
									std::cerr << "\t" << msg << std::endl;
									return rv;
								}
								rv = avfilter_graph_config(filter_graph.get(), nullptr);
								if (rv < 0) {
									char msg[100] = { 0 };
									av_strerror(rv, msg, sizeof(msg) - 1);
									std::cerr << "error:\tfailed to configure filter graph (" << rv << ")" << std::endl;
									std::cerr << "\t" << msg << std::endl;
									return rv;
								}
							}
							// read
							AVPacket inpacket = { 0 };
							std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame(av_frame_alloc(), [](AVFrame * p) {
								av_frame_free(&p);
							});
							std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> prev_frame(av_frame_alloc(), [](AVFrame * p) {
								av_frame_free(&p);
							});
							std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> deinterlaced_frame(av_frame_alloc(), [](AVFrame * p) {
								av_frame_free(&p);
							});
							if (!frame || !prev_frame || !deinterlaced_frame) {
								std::cerr << "error: cannot allocate required frames" << std::endl;
								return 2;
							}
							std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> sws_frame(sws_required ? av_frame_alloc() : nullptr, [](AVFrame * p) {
								if (p) {
									av_frame_free(&p);
								}
							});
							if (sws_required && !sws_frame) {
								std::cerr << "error: cannot allocate required frames" << std::endl;
								return 2;
							}
							std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> swr_frame(swr_required ? av_frame_alloc() : nullptr, [](AVFrame * p) {
								if (p) {
									av_frame_free(&p);
								}
							});
							if (swr_required && !swr_frame) {
								std::cerr << "error: cannot allocate required frames" << std::endl;
								return 2;
							}
							bool have_prev_frame = false;
							int64_t apts = LLONG_MIN, adts = LLONG_MIN, vpts = LLONG_MIN, vdts = LLONG_MIN;
							while (video_frame_count < 9000) {
								rv = av_read_frame(informat.get(), &inpacket);
								if (rv == AVERROR_EOF) {
									break;
								} else if (rv < 0) {
									char msg[100] = { 0 };
									av_strerror(rv, msg, sizeof(msg) - 1);
									std::cerr << "error:\tfailed to read input (" << rv << ")" << std::endl;
									std::cerr << "\t" << msg << std::endl;
									return rv;
								}
								std::unique_ptr<AVPacket, std::function<void(AVPacket *)>> inpacketref(&inpacket, [](AVPacket *p) {
									av_packet_unref(p);
								});
								if (inpacket.stream_index == video_stream_index) {
									rv = avcodec_send_packet(invcodec.get(), &inpacket);
									if (rv < 0) {
										char msg[100] = { 0 };
										av_strerror(rv, msg, sizeof(msg) - 1);
										std::cerr << "error:\tfailed to decode packet (" << rv << ")" << std::endl;
										std::cerr << "\t" << msg << std::endl;
										return rv;
									}
									while (rv >= 0) {
										rv = avcodec_receive_frame(invcodec.get(), frame.get());
										if (rv == AVERROR(EAGAIN) || rv == AVERROR_EOF) {
											break;
										} else if (rv < 0) {
											char msg[100] = { 0 };
											av_strerror(rv, msg, sizeof(msg) - 1);
											std::cerr << "error:\tfailed to receive video frame (" << rv << ")" << std::endl;
											std::cerr << "\t" << msg << std::endl;
											return rv;
										} else {
											++video_frame_count;
											std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frameref(frame.get(), [](AVFrame *p) {
												av_frame_unref(p);
											});
											if ((video_frame_count % 100) == 0) {
												std::cout << video_frame_count << " frames processed" << std::endl;
												if ((video_frame_count % 1000) == 0) {
													Sleep(1000);
												}
											}
											if (sws_required) {
												sws_frame->format = ovcodec->pix_fmt;
												sws_frame->width = ovcodec->width;
												sws_frame->height = ovcodec->height;
												rv = av_image_alloc(sws_frame->data, sws_frame->linesize, sws_frame->width, sws_frame->height, (AVPixelFormat)sws_frame->format, 32);
												if (rv < 0) {
													char msg[100] = { 0 };
													av_strerror(rv, msg, sizeof(msg) - 1);
													std::cerr << "error:\tfailed to allocate yuv420p frame (" << rv << ")" << std::endl;
													std::cerr << "\t" << msg << std::endl;
													return rv;
												}
												rv = sws_scale(sws.get(), frame->data, frame->linesize, 0, frame->height, sws_frame->data, sws_frame->linesize);
												if (rv < 0) {
													char msg[100] = { 0 };
													av_strerror(rv, msg, sizeof(msg) - 1);
													std::cerr << "error:\tfailed to convert input video frame to yuv420p (" << rv << ")" << std::endl;
													std::cerr << "\t" << msg << std::endl;
													return rv;
												}
												rv = av_frame_copy_props(sws_frame.get(), frame.get());
												if (rv < 0) {
													char msg[100] = { 0 };
													av_strerror(rv, msg, sizeof(msg) - 1);
													std::cerr << "error:\tfailed to frame metadata (" << rv << ")" << std::endl;
													std::cerr << "\t" << msg << std::endl;
													return rv;
												}
											}
											std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> sws_frameref(sws_required ? sws_frame.get() : nullptr, [](AVFrame *p) {
												if (p) {
													av_frame_unref(p);
												}
											});
											AVFrame * curframe = sws_required ? sws_frame.get() : frame.get();
											curframe->pts = curframe->best_effort_timestamp;
											// todo: filter
											rv = av_buffersrc_add_frame_flags(bufferctx, curframe, AV_BUFFERSRC_FLAG_KEEP_REF);
											if (rv < 0) {
												char msg[100] = { 0 };
												av_strerror(rv, msg, sizeof(msg) - 1);
												std::cerr << "error:\tfailed to push frame into filter graph (" << rv << ")" << std::endl;
												std::cerr << "\t" << msg << std::endl;
												return rv;
											}
											while (true) {
												rv = av_buffersink_get_frame(buffersinkctx, deinterlaced_frame.get());
												if ((rv == AVERROR(EAGAIN)) || (rv == AVERROR_EOF)) {
													break;
												} else if (rv < 0) {
													char msg[100] = { 0 };
													av_strerror(rv, msg, sizeof(msg) - 1);
													std::cerr << "error:\tfailed to pull frames into filter graph (" << rv << ")" << std::endl;
													std::cerr << "\t" << msg << std::endl;
													return rv;
												}
												AVFrame * diframe = deinterlaced_frame.get();
												std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> diframeref(diframe, [](AVFrame *p) {
													av_frame_unref(p);
												});
												if (is_black_frame(diframe)) {
													if (have_prev_frame) {
														rv = av_frame_copy(diframe, prev_frame.get());
														if (rv < 0) {
															char msg[100] = { 0 };
															av_strerror(rv, msg, sizeof(msg) - 1);
															std::cerr << "error:\tfailed to copy previous video frame data (" << rv << ")" << std::endl;
															std::cerr << "\t" << msg << std::endl;
															return rv;
														}
													}
												} else {
													if (!have_prev_frame) {
														prev_frame->format = diframe->format;
														prev_frame->width = diframe->width;
														prev_frame->height = diframe->height;
														memcpy(prev_frame->linesize, diframe->linesize, sizeof(prev_frame->linesize));
														rv = av_frame_get_buffer(prev_frame.get(), 0);
														if (rv < 0) {
															char msg[100] = { 0 };
															av_strerror(rv, msg, sizeof(msg) - 1);
															std::cerr << "error:\tfailed to allocate video frame data (" << rv << ")" << std::endl;
															std::cerr << "\t" << msg << std::endl;
															return rv;
														}
														have_prev_frame = true;
													}
													rv = av_frame_copy(prev_frame.get(), diframe);
													if (rv < 0) {
														char msg[100] = { 0 };
														av_strerror(rv, msg, sizeof(msg) - 1);
														std::cerr << "error:\tfailed to copy current video frame data (" << rv << ")" << std::endl;
														std::cerr << "\t" << msg << std::endl;
														return rv;
													}
													rv = av_frame_copy_props(prev_frame.get(), diframe);
													if (rv < 0) {
														char msg[100] = { 0 };
														av_strerror(rv, msg, sizeof(msg) - 1);
														std::cerr << "error:\tfailed to frame metadata (" << rv << ")" << std::endl;
														std::cerr << "\t" << msg << std::endl;
														return rv;
													}
												}
												rv = avcodec_send_frame(ovcodec.get(), diframe);
												if (rv < 0) {
													char msg[100] = { 0 };
													av_strerror(rv, msg, sizeof(msg) - 1);
													std::cerr << "error:\tfailed to encode video frame (" << rv << ")" << std::endl;
													std::cerr << "\t" << msg << std::endl;
													return rv;
												}
												while (true) {
													AVPacket outpacket = { 0 };
													av_init_packet(&outpacket);
													rv = avcodec_receive_packet(ovcodec.get(), &outpacket);
													if (rv >= 0) {
														++video_packet_count;
														std::unique_ptr<AVPacket, std::function<void(AVPacket *)>> outpacketref(&outpacket, [](AVPacket *p) {
															av_packet_unref(p);
														});
														outpacket.stream_index = video_stream_index;
														av_packet_rescale_ts(&outpacket, ovcodec->time_base, ovstream->time_base);
														if (outpacket.pts <= vpts) {
															++vpts;
															outpacket.pts = vpts;
														} else {
															vpts = outpacket.pts;
														}
														if (outpacket.dts <= vdts) {
															++vdts;
															outpacket.dts = vdts;
														} else {
															vdts = outpacket.dts;
														}
														rv = av_interleaved_write_frame(oformat.get(), &outpacket);
														if (rv < 0) {
															char msg[100] = { 0 };
															av_strerror(rv, msg, sizeof(msg) - 1);
															std::cerr << "error:\tfailed to write interleaved video frame (" << rv << ")" << std::endl;
															std::cerr << "\t" << msg << std::endl;
															return rv;
														}
													} else if (rv == AVERROR(EAGAIN)) {
														break;
													} else {
														char msg[100] = { 0 };
														av_strerror(rv, msg, sizeof(msg) - 1);
														std::cerr << "error:\tfailed to encode video frame (" << rv << ")" << std::endl;
														std::cerr << "\t" << msg << std::endl;
														return rv;
													}
												}
											}
										}
									}
								} else if (has_audio && (inpacket.stream_index == audio_stream_index)) {
									rv = avcodec_send_packet(inacodec.get(), &inpacket);
									if (rv < 0) {
										char msg[100] = { 0 };
										av_strerror(rv, msg, sizeof(msg) - 1);
										std::cerr << "error:\tfailed to decode audio packet (" << rv << ")" << std::endl;
										std::cerr << "\t" << msg << std::endl;
										return rv;
									}
									while (rv >= 0) {
										rv = avcodec_receive_frame(inacodec.get(), frame.get());
										if (rv == AVERROR(EAGAIN) || rv == AVERROR_EOF) {
											break;
										} else if (rv < 0) {
											char msg[100] = { 0 };
											av_strerror(rv, msg, sizeof(msg) - 1);
											std::cerr << "error:\tfailed to receive audio frame (" << rv << ")" << std::endl;
											std::cerr << "\t" << msg << std::endl;
											return rv;
										} else {
											++audio_frame_count;
											std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frameref(frame.get(), [](AVFrame *p) {
												av_frame_unref(p);
											});
											if (swr_required) {
												swr_frame->format = oacodec->sample_fmt;
												swr_frame->channels = oacodec->channels;
												swr_frame->channel_layout = oacodec->channel_layout;
												swr_frame->sample_rate = oacodec->sample_rate;
												swr_frame->nb_samples = swr_get_out_samples(swr.get(), frame->nb_samples);
												rv = av_samples_alloc(swr_frame->data, swr_frame->linesize, swr_frame->channels, swr_frame->nb_samples, (AVSampleFormat)swr_frame->format, 32);
												if (rv < 0) {
													char msg[100] = { 0 };
													av_strerror(rv, msg, sizeof(msg) - 1);
													std::cerr << "error:\tfailed to allocate audio resampling frame (" << rv << ")" << std::endl;
													std::cerr << "\t" << msg << std::endl;
													return rv;
												}
												if (!frame->channels || !frame->channel_layout) {
													frame->channels = oacodec->channels;
													frame->channel_layout = oacodec->channel_layout;
												}
												rv = swr_convert_frame(swr.get(), swr_frame.get(), frame.get());
												if (rv < 0) {
													char msg[100] = { 0 };
													av_strerror(rv, msg, sizeof(msg) - 1);
													std::cerr << "error:\tfailed to convert input audio frame to required format (" << rv << ")" << std::endl;
													std::cerr << "\t" << msg << std::endl;
													return rv;
												}
												rv = av_frame_copy_props(swr_frame.get(), frame.get());
												if (rv < 0) {
													char msg[100] = { 0 };
													av_strerror(rv, msg, sizeof(msg) - 1);
													std::cerr << "error:\tfailed to copy audio frame metadata (" << rv << ")" << std::endl;
													std::cerr << "\t" << msg << std::endl;
													return rv;
												}
											}
											std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> swr_frameref(swr_required ? swr_frame.get() : nullptr, [](AVFrame *p) {
												if (p) {
													av_frame_unref(p);
												}
											});
											AVFrame * curframe = swr_required ? swr_frame.get() : frame.get();
											curframe->pts = curframe->best_effort_timestamp;
											rv = avcodec_send_frame(oacodec.get(), curframe);
											if (rv < 0) {
												char msg[100] = { 0 };
												av_strerror(rv, msg, sizeof(msg) - 1);
												std::cerr << "error:\tfailed to encode audio frame (" << rv << ")" << std::endl;
												std::cerr << "\t" << msg << std::endl;
												return rv;
											}
											while (true) {
												AVPacket outpacket = { 0 };
												av_init_packet(&outpacket);
												rv = avcodec_receive_packet(oacodec.get(), &outpacket);
												if (rv >= 0) {
													++audio_packet_count;
													std::unique_ptr<AVPacket, std::function<void(AVPacket *)>> outpacketref(&outpacket, [](AVPacket *p) {
														av_packet_unref(p);
													});
													outpacket.stream_index = audio_stream_index;
													av_packet_rescale_ts(&outpacket, oacodec->time_base, oastream->time_base);
													if (outpacket.pts <= apts) {
														++apts;
														outpacket.pts = apts;
													} else {
														apts = outpacket.pts;
													}
													if (outpacket.dts <= adts) {
														++adts;
														outpacket.dts = adts;
													} else {
														adts = outpacket.dts;
													}
													rv = av_interleaved_write_frame(oformat.get(), &outpacket);
													if (rv < 0) {
														char msg[100] = { 0 };
														av_strerror(rv, msg, sizeof(msg) - 1);
														std::cerr << "error:\tfailed to write interleaved audio frame (" << rv << ")" << std::endl;
														std::cerr << "\t" << msg << std::endl;
														return rv;
													}
												} else if (rv == AVERROR(EAGAIN)) {
													break;
												} else {
													char msg[100] = { 0 };
													av_strerror(rv, msg, sizeof(msg) - 1);
													std::cerr << "error:\tfailed to encode audio frame (" << rv << ")" << std::endl;
													std::cerr << "\t" << msg << std::endl;
													return rv;
												}
											}
										}
									}
								}
							}
							if (ovcodec->codec->capabilities & AV_CODEC_CAP_DELAY) {
								rv = avcodec_send_frame(ovcodec.get(), nullptr);
								if (rv < 0) {
									char msg[100] = { 0 };
									av_strerror(rv, msg, sizeof(msg) - 1);
									std::cerr << "error:\tfailed to flush video frames (" << rv << ")" << std::endl;
									std::cerr << "\t" << msg << std::endl;
									return rv;
								}
								while (true) {
									AVPacket outpacket = { 0 };
									av_init_packet(&outpacket);
									rv = avcodec_receive_packet(ovcodec.get(), &outpacket);
									if (rv >= 0) {
										++video_packet_count;
										outpacket.stream_index = video_stream_index;
										av_packet_rescale_ts(&outpacket, ovcodec->time_base, ovstream->time_base);
										if (outpacket.pts <= vpts) {
											++vpts;
											outpacket.pts = vpts;
										} else {
											vpts = outpacket.pts;
										}
										if (outpacket.dts <= vdts) {
											++vdts;
											outpacket.dts = vdts;
										} else {
											vdts = outpacket.dts;
										}
										rv = av_interleaved_write_frame(oformat.get(), &outpacket);
										if (rv < 0) {
											char msg[100] = { 0 };
											av_strerror(rv, msg, sizeof(msg) - 1);
											std::cerr << "error:\tfailed to write interleaved flushed video frame (" << rv << ")" << std::endl;
											std::cerr << "\t" << msg << std::endl;
											return rv;
										}
									} else if (rv == AVERROR_EOF) {
										break;
									} else {
										char msg[100] = { 0 };
										av_strerror(rv, msg, sizeof(msg) - 1);
										std::cerr << "error:\tfailed to encode flushed video frames (" << rv << ")" << std::endl;
										std::cerr << "\t" << msg << std::endl;
										return rv;
									}
								}
							}
							if (has_audio && (oacodec->codec->capabilities & AV_CODEC_CAP_DELAY)) {
								rv = avcodec_send_frame(oacodec.get(), nullptr);
								if (rv < 0) {
									char msg[100] = { 0 };
									av_strerror(rv, msg, sizeof(msg) - 1);
									std::cerr << "error:\tfailed to flush audio frame (" << rv << ")" << std::endl;
									std::cerr << "\t" << msg << std::endl;
									return rv;
								}
								while (true) {
									AVPacket outpacket = { 0 };
									av_init_packet(&outpacket);
									rv = avcodec_receive_packet(oacodec.get(), &outpacket);
									if (rv >= 0) {
										++audio_packet_count;
										outpacket.stream_index = audio_stream_index;
										av_packet_rescale_ts(&outpacket, oacodec->time_base, oastream->time_base);
										if (outpacket.pts <= apts) {
											++apts;
											outpacket.pts = apts;
										} else {
											apts = outpacket.pts;
										}
										if (outpacket.dts <= adts) {
											++adts;
											outpacket.dts = adts;
										} else {
											adts = outpacket.dts;
										}
										rv = av_interleaved_write_frame(oformat.get(), &outpacket);
										if (rv < 0) {
											char msg[100] = { 0 };
											av_strerror(rv, msg, sizeof(msg) - 1);
											std::cerr << "error:\tfailed to write interleaved flushed audio frame (" << rv << ")" << std::endl;
											std::cerr << "\t" << msg << std::endl;
											return rv;
										}
									} else if (rv == AVERROR_EOF) {
										break;
									} else {
										char msg[100] = { 0 };
										av_strerror(rv, msg, sizeof(msg) - 1);
										std::cerr << "error:\tfailed to encode flushed audio frames (" << rv << ")" << std::endl;
										std::cerr << "\t" << msg << std::endl;
										return rv;
									}
								}
							}
							rv = av_write_trailer(oformat.get());
							if (rv < 0) {
								char msg[100] = { 0 };
								av_strerror(rv, msg, sizeof(msg) - 1);
								std::cerr << "error:\tfailed to write output file trailer (" << rv << ")" << std::endl;
								std::cerr << "\t" << msg << std::endl;
								return rv;
							}
						}
					}
				}
			}
		}
	}
	std::cout << "info:\tprocessed " << video_frame_count << " video and " << audio_frame_count << " audio frames" << std::endl;
	return 0;
}

static bool is_black_frame(AVFrame * frame)
{
	AVBufferRef * luma = av_frame_get_plane_buffer(frame, 0);
	double N = frame->height * frame->width;
	uint8_t m = 255, M = 0;
	double Y = 0;
	for (int y = 0; y < frame->height; ++y) {
		const uint8_t * row = luma->data + y * frame->linesize[0];
		for (int x = 0; x < frame->width; ++x) {
			Y += row[x];
			if (row[x] < m) {
				m = row[x];
			}
			if (row[x] > M) {
				M = row[x];
				if (M > 32) {
					return false;
				}
			}
		}
	}
	Y /= N;
	double V = 0;
	for (int y = 0; y < frame->height; ++y) {
		const uint8_t * row = luma->data + y * frame->linesize[0];
		for (int x = 0; x < frame->width; ++x) {
			V += pow(row[x] - Y, 2);
		}
	}
	V = sqrt(V / N);
	bool black = Y <= 17 && V <= 1;
//	char buf[80] = { 0 };
//	snprintf(buf, sizeof(buf) - 1, "@%06lld: [%d,%d] %0.3f +/ %0.3f %s", frame->pts, m, M, Y, V, black ? "black" : "");
//	std::cout << buf << std::endl;
	return black;
}