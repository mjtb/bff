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
							// read
							AVPacket inpacket = { 0 };
							std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame(av_frame_alloc(), [](AVFrame * p) {
								av_frame_free(&p);
							});
							std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> prev_frame(av_frame_alloc(), [](AVFrame * p) {
								av_frame_free(&p);
							});
							if (!frame || !prev_frame) {
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
							while (true) {
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
								std::unique_ptr<AVPacket, std::function<void(AVPacket *)>> packetref(&inpacket, [](AVPacket *p) {
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
											AVFrame * curframe = sws_required ? sws_frame.get() : frame.get();
											std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frameref(curframe, [](AVFrame *p) {
												av_frame_unref(p);
											});
											if (is_black_frame(curframe)) {
												if (have_prev_frame) {
													rv = av_frame_copy(curframe, prev_frame.get());
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
													prev_frame->format = curframe->format;
													prev_frame->width = curframe->width;
													prev_frame->height = curframe->height;
													memcpy(prev_frame->linesize, curframe->linesize, sizeof(prev_frame->linesize));
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
												rv = av_frame_copy(prev_frame.get(), curframe);
												if (rv < 0) {
													char msg[100] = { 0 };
													av_strerror(rv, msg, sizeof(msg) - 1);
													std::cerr << "error:\tfailed to copy current video frame data (" << rv << ")" << std::endl;
													std::cerr << "\t" << msg << std::endl;
													return rv;
												}
												rv = av_frame_copy_props(prev_frame.get(), curframe);
												if (rv < 0) {
													char msg[100] = { 0 };
													av_strerror(rv, msg, sizeof(msg) - 1);
													std::cerr << "error:\tfailed to frame metadata (" << rv << ")" << std::endl;
													std::cerr << "\t" << msg << std::endl;
													return rv;
												}
											}
											curframe->pts = curframe->best_effort_timestamp;
											rv = avcodec_send_frame(ovcodec.get(), curframe);
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
											AVFrame * curframe = swr_required ? swr_frame.get() : frame.get();
											curframe->pts = curframe->best_effort_timestamp;
											std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frameref(curframe, [](AVFrame *p) {
												av_frame_unref(p);
											});
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
	return false;
}