// BitScope H264 / MPEG4 motion-vector bridge. Input consists of complete access
// units tagged with their original coded-frame index, not output ordinals.
// libavcodec carries AVPacket.pts to AVFrame.pts across B-frame reordering.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/motion_vector.h>
}
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

static void check(int result, const char *operation)
{
    if (result < 0) {
        char message[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(result, message, sizeof(message));
        throw std::runtime_error(std::string(operation) + ": " + message);
    }
}

static uint32_t read32(std::istream &input)
{
    unsigned char bytes[4];
    if (!input.read(reinterpret_cast<char *>(bytes), 4)) throw std::runtime_error("truncated input record");
    return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
#else
int main(int argc, char **argv)
#endif
{
    if (argc != 3 && argc != 4) { std::fprintf(stderr, "usage: bitscope-avc-motion input.bsmv frame-index [h264|mpeg4]\n"); return 2; }
    AVCodecContext *decoder = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    int status = 1;
    try {
        const int target = std::stoi(argv[2]);
        const std::string codec = argc == 4 ? std::filesystem::path(argv[3]).string() : "h264";
        if (codec != "h264" && codec != "mpeg4") throw std::runtime_error("unsupported motion codec");
#ifdef _WIN32
        std::ifstream input(std::filesystem::path(argv[1]), std::ios::binary);
#else
        std::ifstream input(std::filesystem::u8path(argv[1]), std::ios::binary);
#endif
        char magic[6];
        if (!input.read(magic, 6) || std::memcmp(magic, "BSMV1\n", 6)) throw std::runtime_error("invalid input header");
        decoder = avcodec_alloc_context3(avcodec_find_decoder(codec == "mpeg4" ? AV_CODEC_ID_MPEG4 : AV_CODEC_ID_H264));
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (!decoder || !packet || !frame) throw std::runtime_error("allocation failed");
        decoder->thread_count = 1;
        decoder->apply_cropping = 0; // Motion coordinates refer to the coded picture.
        decoder->export_side_data |= AV_CODEC_EXPORT_DATA_MVS;
        av_log_set_level(AV_LOG_ERROR);
        check(avcodec_open2(decoder, nullptr, nullptr), "open motion decoder");
        bool found = false;
        auto receive = [&] {
            while (true) {
                const int result = avcodec_receive_frame(decoder, frame);
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
                check(result, "receive frame");
                if (frame->pts == target) {
#ifndef BITSCOPE_PATCHED_MPEG4_MVS
                    if (codec == "mpeg4" && frame->pict_type == AV_PICTURE_TYPE_B)
                        throw std::runtime_error("MPEG4 B-VOP requires the patched BitScope motion helper; unpatched FFmpeg may export uninitialized zero vectors");
#endif
                    if (found) throw std::runtime_error("duplicate target frame identity");
                    if (frame->decode_error_flags) throw std::runtime_error("target contains decoder errors or missing references");
                    if (frame->flags & AV_FRAME_FLAG_INTERLACED)
                        throw std::runtime_error("interlaced/MBAFF motion coordinate mapping is not supported yet");
                    if (frame->crop_left || frame->crop_top)
                        throw std::runtime_error("nonzero left/top crop motion coordinate mapping is not supported yet");
                    const auto *side = av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS);
                    const size_t count = side ? side->size / sizeof(AVMotionVector) : 0;
                    std::printf("BSMV1 %d %d %d %zu\n", target, frame->width, frame->height, count);
                    const auto *vectors = side ? reinterpret_cast<const AVMotionVector *>(side->data) : nullptr;
                    for (size_t i = 0; i < count; ++i) {
                        const auto &v = vectors[i];
                        // H264 ff_print_debug_info2 uses source=-1 for L0,
                        // source=+1 for L1. It is NOT a resolved reference POC.
                        std::printf("%d %d %d %d %d %d %d %u\n", v.source,
                            v.dst_x, v.dst_y, v.w, v.h, v.motion_x, v.motion_y, v.motion_scale);
                    }
                    found = true;
                }
                av_frame_unref(frame);
            }
        };
        while (input.peek() != std::char_traits<char>::eof()) {
            const auto identity = read32(input), size = read32(input);
            if (size == 0 || size > 128 * 1024 * 1024) throw std::runtime_error("invalid access unit size");
            check(av_new_packet(packet, int(size)), "allocate packet");
            if (!input.read(reinterpret_cast<char *>(packet->data), size)) throw std::runtime_error("truncated access unit");
            packet->pts = identity;
            packet->dts = AV_NOPTS_VALUE;
            check(avcodec_send_packet(decoder, packet), "send packet");
            av_packet_unref(packet);
            receive();
        }
        check(avcodec_send_packet(decoder, nullptr), "flush decoder");
        receive();
        if (!found) throw std::runtime_error("target frame was not returned by decoder");
        status = 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "BitScope motion: %s\n", error.what());
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    return status;
}
