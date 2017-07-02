/*	bff - Black Frame Filter for FFmpeg
	Copyright (C) 2017 Michael Trenholm-Boyle.
	This software is redistributable under a permissive open source license.
	See the LICENSE file for further information. */
#include "stdafx.h"

#include "bff.h"

#include <cstdarg>
#include <vector>

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

std::string ffmpeg_error::format_message(int er, const char * fn, const char * arg)
{
	char em[100] = { 0 }, bm[200] = { 0 };
	av_strerror(er, em, sizeof(em) - 1);
	snprintf(bm, sizeof(bm) - 1, "%s(%s) failed; return = %d: %s", fn, arg, er, em);
	return bm;
}


int wmain(int argc, wchar_t ** argv)
{
	cliopts opts(argc, argv);
	if(opts.check_syntax())
	{
		opts.print_syntax_help();
		return 1;
	}
	int rv = -1;
	try {
		rv = bff(opts);
	} catch (const ffmpeg_error & e) {
		std::cerr << e.what() << std::endl;
		rv = e.error_code();
	}
	return rv;
}

int bff(const cliopts & opts)
{
	int rv;
	av_register_all();
	avfilter_register_all();
	// statistics to display
	uint64_t video_frame_count = 0, audio_frame_count = 0, black_frame_count = 0;
	uint64_t video_packet_count = 0, audio_packet_count = 0;
	// open input
	AVFormatContext *p = nullptr;
	rv = avformat_open_input(&p, ansi(opts.input).c_str(), nullptr, nullptr);
	if (rv < 0) {
		throw ffmpeg_error(rv, "avformat_open_input", ansi(opts.input).c_str());
	}
	std::unique_ptr<AVFormatContext, std::function<void(AVFormatContext*)>> informat(p, [](AVFormatContext *p) {
		avformat_close_input(&p);
	});
	{
		rv = avformat_find_stream_info(informat.get(), nullptr);
		if (rv < 0) {
			throw ffmpeg_error(rv, "avformat_find_stream_info", "");
		}
		AVCodec *q = nullptr;
		rv = av_find_best_stream(informat.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &q, 0);
		if (rv < 0) {
			throw ffmpeg_error(rv, "av_find_best_stream", "AVMEDIA_TYPE_VIDEO");
		}
		int video_stream_index = rv;
		std::unique_ptr<AVCodecContext, std::function<void(AVCodecContext*)>> invcodec(avcodec_alloc_context3(q), [](AVCodecContext *p) {
			avcodec_free_context(&p);
		});
		rv = avcodec_parameters_to_context(invcodec.get(), informat->streams[video_stream_index]->codecpar);
		if (rv < 0) {
			throw ffmpeg_error(rv, "avcodec_parameters_to_context", "video");
		}
		rv = av_opt_set_int(invcodec.get(), "refcounted_frames", 1, 0);
		if (rv < 0) {
			throw ffmpeg_error(rv, "av_opt_set_int", "refcounted_frames/video");
		}
		rv = avcodec_open2(invcodec.get(), q, nullptr);
		if (rv < 0) {
			throw ffmpeg_error(rv, "avcodec_open2", "video");
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
				throw ffmpeg_error(rv, "avcodec_parameters_to_context", "audio");
			}
			rv = avcodec_open2(inacodec.get(), q, nullptr);
			if (rv < 0) {
				throw ffmpeg_error(rv, "avcodec_open2", "audio");
			}
		}
		// open output
		std::string fname = ansi(opts.output);
		struct stat st = { 0 };
		if (stat(fname.c_str(), &st) == 0) {
			std::cerr << "warn:\toutput file " << fname << " already exists and will be deleted" << std::endl;
			if (_unlink(fname.c_str()) != 0) {
				throw std::runtime_error(_strdup(strerror(errno)));
			}
		}
		std::unique_ptr<AVFormatContext, std::function<void(AVFormatContext*)>> oformat(avformat_alloc_context(), [](AVFormatContext *p) {
			avformat_free_context(p);
		});
		{
			rv = avio_open(&(oformat->pb), fname.c_str(), AVIO_FLAG_READ_WRITE);
			if (rv < 0) {
				throw ffmpeg_error(rv, "avio_open", fname.c_str());
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
						throw ffmpeg_error(rv, "avcodec_open2", "h264");
					}
					rv = avcodec_parameters_from_context(ovstream->codecpar, ovcodec.get());
					if (rv < 0) {
						throw ffmpeg_error(rv, "avcodec_parameters_from_context", "video");
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
									throw ffmpeg_error(rv, "avcodec_open2", "aac");
								}
								rv = avcodec_parameters_from_context(oastream->codecpar, oacodec.get());
								if (rv < 0) {
									throw ffmpeg_error(rv, "avcodec_parameters_from_context", "audio");
								}
								if (oformat->oformat->flags & AVFMT_GLOBALHEADER) {
									oacodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
								}
								oastream->time_base = oacodec->time_base;
							}
							rv = avformat_write_header(oformat.get(), nullptr);
							if (rv < 0) {
								throw ffmpeg_error(rv, "avformat_write_header", "");
							}
							bool sws_required = (invcodec->pix_fmt != ovcodec->pix_fmt) || (invcodec->width != ovcodec->width) || (invcodec->height != ovcodec->height);
							bool swr_required = has_audio && ((inacodec->sample_fmt != oacodec->sample_fmt) || (inacodec->sample_rate != oacodec->sample_rate) || (inacodec->channels != oacodec->channels) || (inacodec->channel_layout != oacodec->channel_layout));
							// configure filter graph for deinterlacing
							std::unique_ptr<AVFilterGraph, std::function<void(AVFilterGraph*)>> filter_graph(avfilter_graph_alloc(), [](AVFilterGraph *p) {
								avfilter_graph_free(&p);
							});
							AVFilterContext * bufferctx = nullptr;
							AVFilterContext * buffersinkctx = nullptr;
							{
								AVFilter * buffer = avfilter_get_by_name("buffer");
								if (!buffer) {
									throw ffmpeg_error(AVERROR_UNKNOWN, "avfilter_get_by_name", "buffer");
								}
								AVFilter * buffersink = avfilter_get_by_name("buffersink");
								if (!buffersink) {
									throw ffmpeg_error(AVERROR_UNKNOWN, "avfilter_get_by_name", "buffersink");
								}
								const size_t arglen = 32*32;
								char * args = (char *)alloca(arglen);
								memset(args, 0, arglen);
								AVRational time_base = ovcodec->time_base;
								snprintf(args, arglen, "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", ovcodec->width, ovcodec->height, ovcodec->pix_fmt, time_base.num, time_base.den, ovcodec->sample_aspect_ratio.num, ovcodec->sample_aspect_ratio.den);
								rv = avfilter_graph_create_filter(&bufferctx, buffer, "in", args, nullptr, filter_graph.get());
								if (rv < 0) {
									throw ffmpeg_error(rv, "avfilter_graph_create_filter", args);
								}
								rv = avfilter_graph_create_filter(&buffersinkctx, buffersink, "out", nullptr, nullptr, filter_graph.get());
								if (rv < 0) {
									throw ffmpeg_error(rv, "avfilter_graph_create_filter", "out");
								}
								AVFilterInOut * inputs = avfilter_inout_alloc(), *outputs = avfilter_inout_alloc();
								if (!outputs || !inputs) {
									throw ffmpeg_error(AVERROR_UNKNOWN, "avfilter_inout_alloc", "");
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
									throw ffmpeg_error(rv, "avfilter_graph_parse_ptr", "kerndeint");
								}
								rv = avfilter_graph_config(filter_graph.get(), nullptr);
								if (rv < 0) {
									throw ffmpeg_error(rv, "avfilter_graph_parse_ptr", "");
								}
							}
							// read
							std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> prev_frame(av_frame_alloc(), [](AVFrame * p) {
								av_frame_free(&p);
							});
							if (!prev_frame) {
								throw ffmpeg_error(AVERROR_UNKNOWN, "av_frame_alloc", "prev_frame");
							}
							bool have_prev_frame = false;
							int64_t apts = LLONG_MIN, adts = LLONG_MIN, vpts = LLONG_MIN, vdts = LLONG_MIN;
							while (true) {
								std::unique_ptr<AVPacket, std::function<void(AVPacket *)>> inpacket(av_packet_alloc(), [](AVPacket *p) {
									av_packet_free(&p);
								});
								rv = av_read_frame(informat.get(), inpacket.get());
								if (rv == AVERROR_EOF) {
									break;
								} else if (rv < 0) {
									throw ffmpeg_error(rv, "av_read_frame", "input");
								}
								if (inpacket->stream_index == video_stream_index) {
									rv = avcodec_send_packet(invcodec.get(), inpacket.get());
									if (rv < 0) {
										throw ffmpeg_error(rv, "avcodec_send_packet", "input");
									}
									while (rv >= 0) {
										std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame(av_frame_alloc(), [](AVFrame * p) {
											av_frame_free(&p);
										});
										rv = avcodec_receive_frame(invcodec.get(), frame.get());
										if (rv == AVERROR(EAGAIN) || rv == AVERROR_EOF) {
											break;
										} else if (rv < 0) {
											throw ffmpeg_error(rv, "avcodec_receive_frame", "input video");
										} else {
											++video_frame_count;
											if ((video_frame_count % 100) == 0) {
												std::cout << video_frame_count << " frames processed, " << black_frame_count << " black frame(s) encountered" << std::endl;
											}
											std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> sws_frame(sws_required ? av_frame_alloc() : nullptr, [](AVFrame * p) {
												if (p) {
													av_freep(p->data);
													av_frame_free(&p);
												}
											});
											if (sws_required && !sws_frame) {
												throw ffmpeg_error(AVERROR_UNKNOWN, "av_frame_alloc", "sws");
											}
											if (sws_required) {
												std::unique_ptr<SwsContext, std::function<void(SwsContext*)>> sws(sws_required ? sws_getContext(invcodec->width, invcodec->height, invcodec->pix_fmt, ovcodec->width, ovcodec->height, ovcodec->pix_fmt, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr) : nullptr, [](SwsContext *p) {
													if (p) {
														sws_freeContext(p);
													}
												});
												sws_frame->format = ovcodec->pix_fmt;
												sws_frame->width = ovcodec->width;
												sws_frame->height = ovcodec->height;
												rv = av_image_alloc(sws_frame->data, sws_frame->linesize, sws_frame->width, sws_frame->height, (AVPixelFormat)sws_frame->format, 32);
												if (rv < 0) {
													throw ffmpeg_error(rv, "av_image_alloc", "sws");
												}
												rv = sws_scale(sws.get(), frame->data, frame->linesize, 0, frame->height, sws_frame->data, sws_frame->linesize);
												if (rv < 0) {
													throw ffmpeg_error(rv, "sws_scale", "");
												}
												rv = av_frame_copy_props(sws_frame.get(), frame.get());
												if (rv < 0) {
													throw ffmpeg_error(rv, "av_frame_copy_props", "sws");
												}
											}
											AVFrame * curframe = sws_required ? sws_frame.get() : frame.get();
											curframe->pts = curframe->best_effort_timestamp;
											rv = av_buffersrc_add_frame_flags(bufferctx, curframe, AV_BUFFERSRC_FLAG_KEEP_REF);
											if (rv < 0) {
												throw ffmpeg_error(rv, "av_buffersrc_add_frame_flags", "");
											}
											while (true) {
												std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> deinterlaced_frame(av_frame_alloc(), [](AVFrame * p) {
													av_frame_free(&p);
												});
												rv = av_buffersink_get_frame(buffersinkctx, deinterlaced_frame.get());
												if ((rv == AVERROR(EAGAIN)) || (rv == AVERROR_EOF)) {
													break;
												} else if (rv < 0) {
													throw ffmpeg_error(rv, "av_buffersink_get_frame", "");
												}
												if (is_black_frame(deinterlaced_frame.get())) {
													if (have_prev_frame) {
														++black_frame_count;
														rv = av_frame_copy(deinterlaced_frame.get(), prev_frame.get());
														if (rv < 0) {
															throw ffmpeg_error(rv, "av_frame_copy", "deinterlaced");
														}
													}
												} else {
													if (!have_prev_frame) {
														prev_frame->format = deinterlaced_frame->format;
														prev_frame->width = deinterlaced_frame->width;
														prev_frame->height = deinterlaced_frame->height;
														memcpy(prev_frame->linesize, deinterlaced_frame->linesize, sizeof(prev_frame->linesize));
														rv = av_frame_get_buffer(prev_frame.get(), 0);
														if (rv < 0) {
															throw ffmpeg_error(rv, "av_frame_get_buffer", "deinterlaced");
														}
														have_prev_frame = true;
													}
													rv = av_frame_copy(prev_frame.get(), deinterlaced_frame.get());
													if (rv < 0) {
														throw ffmpeg_error(rv, "av_frame_copy", "deinterlaced");
													}
													rv = av_frame_copy_props(prev_frame.get(), deinterlaced_frame.get());
													if (rv < 0) {
														throw ffmpeg_error(rv, "av_frame_copy_props", "deinterlaced");
													}
												}
												rv = avcodec_send_frame(ovcodec.get(), deinterlaced_frame.get());
												if (rv < 0) {
													throw ffmpeg_error(rv, "avcodec_send_frame", "output video");
												}
												while (true) {
													std::unique_ptr<AVPacket, std::function<void(AVPacket *)>> outpacket(av_packet_alloc(), [](AVPacket *p) {
														av_packet_free(&p);
													});
													rv = avcodec_receive_packet(ovcodec.get(), outpacket.get());
													if (rv >= 0) {
														++video_packet_count;
														outpacket->stream_index = ovstream->index;
														av_packet_rescale_ts(outpacket.get(), ovcodec->time_base, ovstream->time_base);
														if (outpacket->pts <= vpts) {
															++vpts;
															outpacket->pts = vpts;
														} else {
															vpts = outpacket->pts;
														}
														if (outpacket->dts <= vdts) {
															++vdts;
															outpacket->dts = vdts;
														} else {
															vdts = outpacket->dts;
														}
														rv = av_interleaved_write_frame(oformat.get(), outpacket.get());
														if (rv < 0) {
															throw ffmpeg_error(rv, "av_interleaved_write_frame", "video");
														}
													} else if (rv == AVERROR(EAGAIN)) {
														break;
													} else {
														throw ffmpeg_error(rv, "avcodec_receive_packet", "output video");
													}
												}
											}
										}
									}
								} else if (has_audio && (inpacket->stream_index == audio_stream_index)) {
									rv = avcodec_send_packet(inacodec.get(), inpacket.get());
									if (rv < 0) {
										throw ffmpeg_error(rv, "avcodec_send_packet", "input audio");
									}
									while (rv >= 0) {
										std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame(av_frame_alloc(), [](AVFrame * p) {
											av_frame_free(&p);
										});
										rv = avcodec_receive_frame(inacodec.get(), frame.get());
										if (rv == AVERROR(EAGAIN) || rv == AVERROR_EOF) {
											break;
										} else if (rv < 0) {
											throw ffmpeg_error(rv, "avcodec_receive_frame", "input audio");
										} else {
											++audio_frame_count;
											std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> swr_frame(swr_required ? av_frame_alloc() : nullptr, [](AVFrame * p) {
												if (p) {
													av_freep(p->data);
													av_frame_free(&p);
												}
											});
											if (swr_required && !swr_frame) {
												throw ffmpeg_error(AVERROR_UNKNOWN, "av_frame_alloc", "swr");
											}
											if (swr_required) {
												std::unique_ptr<SwrContext, std::function<void(SwrContext*)>> swr(swr_required ? swr_alloc_set_opts(nullptr, oacodec->channel_layout, oacodec->sample_fmt, oacodec->sample_rate, inacodec->channel_layout, inacodec->sample_fmt, inacodec->sample_rate, 0, nullptr) : nullptr, [](SwrContext *p) {
													if (p) {
														swr_free(&p);
													}
												});
												swr_frame->format = oacodec->sample_fmt;
												swr_frame->channels = oacodec->channels;
												swr_frame->channel_layout = oacodec->channel_layout;
												swr_frame->sample_rate = oacodec->sample_rate;
												swr_frame->nb_samples = swr_get_out_samples(swr.get(), frame->nb_samples);
												rv = av_samples_alloc(swr_frame->data, swr_frame->linesize, swr_frame->channels, swr_frame->nb_samples, (AVSampleFormat)swr_frame->format, 32);
												if (rv < 0) {
													throw ffmpeg_error(rv, "av_samples_alloc", "swr");
												}
												if (!frame->channels || !frame->channel_layout) {
													frame->channels = oacodec->channels;
													frame->channel_layout = oacodec->channel_layout;
												}
												rv = swr_convert_frame(swr.get(), swr_frame.get(), frame.get());
												if (rv < 0) {
													throw ffmpeg_error(rv, "swr_convert_frame", "");
												}
												rv = av_frame_copy_props(swr_frame.get(), frame.get());
												if (rv < 0) {
													throw ffmpeg_error(rv, "av_frame_copy_props", "swr");
												}
											}
											AVFrame * curframe = swr_required ? swr_frame.get() : frame.get();
											curframe->pts = curframe->best_effort_timestamp;
											rv = avcodec_send_frame(oacodec.get(), curframe);
											if (rv < 0) {
												throw ffmpeg_error(rv, "avcodec_send_frame", "audio");
											}
											while (true) {
												std::unique_ptr<AVPacket, std::function<void(AVPacket *)>> outpacket(av_packet_alloc(), [](AVPacket *p) {
													av_packet_free(&p);
												});
												rv = avcodec_receive_packet(oacodec.get(), outpacket.get());
												if (rv >= 0) {
													++audio_packet_count;
													outpacket->stream_index = oastream->index;
													av_packet_rescale_ts(outpacket.get(), oacodec->time_base, oastream->time_base);
													if (outpacket->pts <= apts) {
														++apts;
														outpacket->pts = apts;
													} else {
														apts = outpacket->pts;
													}
													if (outpacket->dts <= adts) {
														++adts;
														outpacket->dts = adts;
													} else {
														adts = outpacket->dts;
													}
													rv = av_interleaved_write_frame(oformat.get(), outpacket.get());
													if (rv < 0) {
														throw ffmpeg_error(rv, "av_interleaved_write_frame", "audio");
													}
												} else if (rv == AVERROR(EAGAIN)) {
													break;
												} else {
													throw ffmpeg_error(rv, "avcodec_receive_packet", "audio");
												}
											}
										}
									}
								}
							}
							if (ovcodec->codec->capabilities & AV_CODEC_CAP_DELAY) {
								rv = avcodec_send_frame(ovcodec.get(), nullptr);
								if (rv < 0) {
									throw ffmpeg_error(rv, "avcodec_send_frame", "flush video");
								}
								while (true) {
									std::unique_ptr<AVPacket, std::function<void(AVPacket *)>> outpacket(av_packet_alloc(), [](AVPacket *p) {
										av_packet_free(&p);
									});
									rv = avcodec_receive_packet(ovcodec.get(), outpacket.get());
									if (rv >= 0) {
										++video_packet_count;
										outpacket->stream_index = ovstream->index;
										av_packet_rescale_ts(outpacket.get(), ovcodec->time_base, ovstream->time_base);
										if (outpacket->pts <= vpts) {
											++vpts;
											outpacket->pts = vpts;
										} else {
											vpts = outpacket->pts;
										}
										if (outpacket->dts <= vdts) {
											++vdts;
											outpacket->dts = vdts;
										} else {
											vdts = outpacket->dts;
										}
										rv = av_interleaved_write_frame(oformat.get(), outpacket.get());
										if (rv < 0) {
											throw ffmpeg_error(rv, "av_interleaved_write_frame", "flush video");
										}
									} else if (rv == AVERROR(EAGAIN) || rv == AVERROR_EOF) {
										break;
									} else {
										throw ffmpeg_error(rv, "avcodec_receive_packet", "flush video");
									}
								}
							}
							if (has_audio && (oacodec->codec->capabilities & AV_CODEC_CAP_DELAY)) {
								rv = avcodec_send_frame(oacodec.get(), nullptr);
								if (rv < 0) {
									throw ffmpeg_error(rv, "avcodec_send_frame", "flush audio");
								}
								while (true) {
									std::unique_ptr<AVPacket, std::function<void(AVPacket *)>> outpacket(av_packet_alloc(), [](AVPacket *p) {
										av_packet_free(&p);
									});
									rv = avcodec_receive_packet(oacodec.get(), outpacket.get());
									if (rv >= 0) {
										++audio_packet_count;
										outpacket->stream_index = oastream->index;
										av_packet_rescale_ts(outpacket.get(), oacodec->time_base, oastream->time_base);
										if (outpacket->pts <= apts) {
											++apts;
											outpacket->pts = apts;
										} else {
											apts = outpacket->pts;
										}
										if (outpacket->dts <= adts) {
											++adts;
											outpacket->dts = adts;
										} else {
											adts = outpacket->dts;
										}
										rv = av_interleaved_write_frame(oformat.get(), outpacket.get());
										if (rv < 0) {
											throw ffmpeg_error(rv, "av_interleaved_write_frame", "flush audio");
										}
									} else if (rv == AVERROR(EAGAIN) || rv == AVERROR_EOF) {
										break;
									} else {
										throw ffmpeg_error(rv, "avcodec_receive_packet", "flush audio");
									}
								}
							}
							rv = av_write_trailer(oformat.get());
							if (rv < 0) {
								throw ffmpeg_error(rv, "av_write_trailer", "");
							}
						}
					}
				}
			}
		}
	}
	std::cout << "info:\tprocessed " << video_frame_count << " video and " << audio_frame_count << " audio frames" << std::endl;
	std::cout << "info:\tsubstituted " << black_frame_count << " black frames" << std::endl;
	return 0;
}

static void luma_histogram(const uint8_t * Y, int width, int height, int linesize, int lim, int * count, ...)
{
	va_list ap;
	va_start(ap, count);
	const size_t NUM_LIMS = 256;
	int * lims = (int *)alloca(sizeof(int) * NUM_LIMS);
	int ** counts = (int **)alloca(sizeof(int *) * NUM_LIMS);
	lims[0] = lim;
	counts[0] = count;
	*count = 0;
	size_t num = 1;
	while (num < NUM_LIMS) {
		int lim = va_arg(ap, int);
		if (lim <= 0) {
			break;
		} else {
			int * count = va_arg(ap, int *);
			*count = 0;
			lims[num] = lim;
			counts[num] = count;
			++num;
		}
	}
	for (int y = 0; y < height; ++y) {
		const uint8_t * row = Y + y * linesize;
		for (int x = 0; x < width; ++x) {
			int v = row[x];
			for (size_t i = 0; i < num; ++i) {
				if (lims[i] >= v) {
					++*(counts[i]);
					break;
				}
			}
		}
	}
}

static void luma_statistics(const uint8_t * Y, int width, int height, int linesize, uint8_t * range_min, uint8_t * range_max, double * mean, double * stdev)
{
	double N = width * height;
	double S = 0;
	uint8_t m = 255, M = 0;
	for (int y = 0; y < height; ++y) {
		const uint8_t * row = Y + y * linesize;
		for (int x = 0; x < width; ++x) {
			S += row[x];
			if (row[x] < m) {
				m = row[x];
			}
			if (row[x] > M) {
				M = row[x];
			}
		}
	}
	S /= N;
	double V = 0;
	for (int y = 0; y < height; ++y) {
		const uint8_t * row = Y + y * linesize;
		for (int x = 0; x < width; ++x) {
			V += pow(row[x] - S, 2);
		}
	}
	V = sqrt(V / N);
	if (range_min) {
		*range_min = m;
	}
	if (range_max) {
		*range_max = M;
	}
	if (mean) {
		*mean = S;
	}
	if (stdev) {
		*stdev = V;
	}
}

static bool is_statistically_black_frame(AVFrame * frame, double mean_threshold = 17, double stdev_threshold = 1)
{
	AVBufferRef * luma = av_frame_get_plane_buffer(frame, 0);
	double mean = 0, stdev = 0;
	luma_statistics(luma->data, frame->width, frame->height, frame->linesize[0], nullptr, nullptr, &mean, &stdev);
	return ((mean <= mean_threshold) && (stdev <= stdev_threshold));
}

static bool is_proportionally_black_frame(AVFrame * frame, uint8_t y_max = 17, double proportion_threshold = 0.86)
{
	AVBufferRef * luma = av_frame_get_plane_buffer(frame, 0);
	int count = 0;
	luma_histogram(luma->data, frame->width, frame->height, frame->linesize[0], (int)y_max, &count, 0);
	double proportion = count / (double)(frame->width * frame->height);
	return (proportion >= proportion_threshold);
}

static bool is_black_frame(AVFrame * frame)
{
	AVBufferRef * luma = av_frame_get_plane_buffer(frame, 0);
//	return is_statistically_black_frame(frame);
	return is_proportionally_black_frame(frame);
}