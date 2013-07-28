#include <vector>
#include <ostream>
#include <cstdarg>

#include <boost/iostreams/categories.hpp>
#include <boost/iostreams/stream_buffer.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/shared_ptr.hpp>

#include <glog/logging.h>

#if defined __cplusplus
# define __STDC_CONSTANT_MACROS
#endif

extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>

#if LIBAVCODEC_VERSION_MAJOR < 54
#   define AV_CODEC_ID_NONE CODEC_ID_NONE
#   define AVAudioResampleContext ReSampleContext
#   define avcodec_free_frame av_freep
#else
#   include <libavresample/avresample.h>
#endif

}

#include "stream_transcoder.hpp"

#include <iostream>


template <class T>
std::auto_ptr<T> make_auto_ptr(T* pointer)
{
    return std::auto_ptr<T>(pointer);
}


/// Emulate printf to const char*
class stringf
{
public:
    stringf(const char* format, std::va_list arg_list)
    {
        const std::size_t start_len = 256;

        this->m_chars = new char[start_len];

        const size_t needed = vsnprintf(this->m_chars, start_len,
                                        format, arg_list) + 1;
        if (needed <= start_len)
        {
            return;
        }

        // need more space...
        delete[] this->m_chars;

        this->m_chars = new char[needed];
        vsnprintf(this->m_chars, needed, format, arg_list);
    }

    ~stringf()
    {
        delete[] this->m_chars;
    }

    const char* get() const
    {
        return this->m_chars;
    }

private:
    char* m_chars;
};


/// libav read hook
int read_callback(void* stream, uint8_t* buf, int buf_size)
{
    std::istream*
                pauin = reinterpret_cast<std::istream*>(stream);
    pauin->read(reinterpret_cast<char*>(buf), buf_size);

    VLOG_IF(5, !bool(pauin)) << "Read failed. EOF?";
    VLOG(6) << "Read bytes: " << pauin->gcount()
            << " of " << buf_size;

    return pauin->gcount();
}


/// libav write hook
int write_callback(void* stream, uint8_t* buf, int buf_size)
{
    std::string* m_buffer =
                        reinterpret_cast<std::string*>(stream);
    try
    {
        m_buffer->insert(m_buffer->size(),
            reinterpret_cast<std::string::const_pointer>(buf),
            buf_size);

        VLOG(6) << "Write bytes: " << buf_size;
    }
    catch(...) // don't want c-code to handle any exceptions
    {
        LOG(ERROR) << "Exception while writing in transcoder";
        return 0;
    }

    return buf_size;
}


/// libav logging hook
void logging_callback(void* ptr, int level, const char* fmt,
                      std::va_list arg_list)
{
    using boost::lexical_cast;

    AVClass* avc = ptr ? (*reinterpret_cast<AVClass**>(ptr)) : 0;

    std::string prefix;

    if (avc)
    {
        if (avc->parent_log_context_offset)
        {
            AVClass** parent = *reinterpret_cast<AVClass ***> (ptr) +
                avc->parent_log_context_offset;
            if (parent && *parent)
            {
                prefix += '[';
                prefix += (*parent)->class_name;
                prefix += ' ';
                prefix += (*parent)->item_name(parent);
                prefix += " @ ";
                prefix += lexical_cast<std::string>(parent);
                prefix += "] ";
            }
        }
        prefix += '[';
        prefix += avc->class_name;
        prefix += ' ';
        prefix += avc->item_name(ptr);
        prefix += " @ ";
        prefix += lexical_cast<std::string>(ptr);
        prefix += "] ";
    }

    switch(level)
    {
    case AV_LOG_PANIC:
    case AV_LOG_FATAL:
    case AV_LOG_ERROR:
        LOG(ERROR) << prefix << stringf(fmt, arg_list).get();
        break;
    case AV_LOG_WARNING:
        LOG(WARNING) << prefix << stringf(fmt, arg_list).get();
        break;
    case AV_LOG_INFO:
        VLOG(3) << prefix << stringf(fmt, arg_list).get();
        break;
    case AV_LOG_VERBOSE:
        VLOG(4) << prefix << stringf(fmt, arg_list).get();
        break;
    case AV_LOG_DEBUG:
        VLOG(5) << prefix << stringf(fmt, arg_list).get();
        break;
    default:
        LOG(ERROR) << prefix
                   << "*" << stringf(fmt, arg_list).get();
    }
}


/// check that a given sample format is supported by the encoder
int check_sample_fmt(AVCodec* codec, AVSampleFormat sample_fmt)
{
    for (const AVSampleFormat*
         p = codec->sample_fmts; *p != AV_SAMPLE_FMT_NONE; p++)
    {
        if (*p == sample_fmt) return 1;
    }
    return 0;
}


/// RAII for libav AVPacket
class av_packet
{
public:
    explicit av_packet(AVFormatContext* context = NULL):
        m_offset(0)
    {
        av_init_packet(&m_packet);
        m_packet.data = NULL;
        m_packet.size = 0;
        if (context) reset(context);
    }

    ~av_packet()
    {
        if (this->m_packet.data)
        {
            av_free_packet(&this->m_packet);
        }
    }

    void reset(AVFormatContext* context)
    {
        if (this->m_packet.data)
        {
            av_free_packet(&this->m_packet);
        }
        int rval = av_read_frame(context, &this->m_packet);
        if (rval < 0)
        {
            this->m_packet.data = NULL;
        }
    }

    AVPacket& get()
    {
        return this->m_packet;
    }

    unsigned int size() const
    {
        return this->m_packet.size;
    }

    uint8_t* data()
    {
        return this->m_packet.data;
    }

    std::size_t offset() const
    {
        return this->m_offset;
    }

    bool offset_reset()
    {
        this->m_offset = 0;
        return true;
    }

    std::size_t offset_add(const std::size_t offset)
    {
        this->m_offset += offset;
        return this->m_offset;
    }

private:
    AVPacket m_packet;
    std::size_t m_offset;

    friend class stream_transcoder;
};


/// boost::iostreams device for resulting stream
class st_device
{
public:
    typedef char                            char_type;
    typedef boost::iostreams::source_tag    category;

    st_device(stream_transcoder* parent, unsigned int stream_id):
        m_parent(parent), m_stream_id(stream_id)
    {}

    std::streamsize
    read(char* buffer, std::streamsize buffer_size)
    {
        return m_parent->read(this->m_stream_id,
                            buffer, buffer_size);
    }

private:
    stream_transcoder* m_parent;
    unsigned int m_stream_id;
};

typedef boost::iostreams::stream_buffer<st_device> stream_buffer;


/// RAII for libav contexts and other resources
struct st_contexts
{
    unsigned char* ibuffer;
    unsigned char* obuffer;

    AVIOContext* input;
    AVIOContext* output;

    AVFormatContext* iformat;
    AVFormatContext* oformat;

    AVFrame* iframe;
    AVFrame* oframe;

    int oframe_linesize;

    typedef std::vector<AVCodecContext*> avcc_vector;
    avcc_vector codec_contexts;

    typedef std::vector<AVAudioResampleContext*> avar_vector;
    avar_vector resample_contexts;

    std::vector<unsigned int> codec_ids;

    std::basic_string<uint8_t> r_samples;
    av_packet packet;

    std::vector<st_device> result_stream_devices;
    std::vector<boost::shared_ptr<stream_buffer> >
                                        result_stream_buffers;
    std::vector<boost::shared_ptr<std::istream> >
                                        result_streams;

    std::string result;
    std::string::size_type result_offset;
    const std::string::size_type max_result_offset;
    bool eof;

    st_contexts():
        ibuffer(NULL), obuffer(NULL),
        input(NULL), output(NULL),
        iformat(NULL), oformat(NULL),
        iframe(NULL), oframe(NULL),
        result_offset(0),
        max_result_offset(FF_MIN_BUFFER_SIZE),
        eof(false)
    {}

    ~st_contexts()
    {
        if (this->iframe)
        {
            VLOG(5) << "Destroying input frame";
            avcodec_free_frame(&this->iframe);
        }
        if (this->oframe)
        {
            VLOG(5) << "Destroying output frame";
            avcodec_free_frame(&this->oframe);
        }
#       if LIBAVCODEC_VERSION_MAJOR > 54
        for (avar_vector::iterator
             i = this->resample_contexts.begin();
             i != this->resample_contexts.end(); i++)
        {
            avresample_free(*i);
        }
#       endif
        if (this->iformat)
        {
            VLOG(5) << "Destroying input format context";
            avformat_free_context(this->iformat);
        }
        if (this->oformat)
        {
            VLOG(5) << "Destroying output format context";
            avformat_free_context(this->oformat);
        }

        if (this->input)
        {
            VLOG(5) << "Cleanning input I/O context";
            av_freep(this->input);
        }
        else if (this->ibuffer)
        {
            VLOG(5) << "Freeing input buffer";
            av_freep(this->ibuffer);
        }
        if (this->output)
        {
            VLOG(5) << "Cleanning output I/O context";
            av_freep(this->output);
        }
        else if (this->obuffer)
        {
            VLOG(5) << "Freeing output buffer";
            av_freep(this->obuffer);
        }
    }
};


stream_transcoder::stream_transcoder(std::istream& auin,
    const char* codec_name):
    m_codec_name(codec_name),
    m_buffer_size(FF_MIN_BUFFER_SIZE)
{
    av_log_set_callback(logging_callback);

    m_ok = init_input(auin);
    if (!m_ok) return;

    for (unsigned int
                 i = 0; i < m_contexts->iformat->nb_streams; ++i)
    {
        AVStream* stream = m_contexts->iformat->streams[i];
        AVCodecContext* codec = stream->codec;

        const char*
                s = av_get_sample_fmt_name(codec->sample_fmt);
        VLOG(3) << "Found: [" << i << "] "
            << " channels: " << codec->channels
            << " sample rate: " << codec->sample_rate
            << " sample format: " << (s ? s : "unknown");

        AVCodec* decoder = avcodec_find_decoder(codec->codec_id);

        if (decoder)
        {
            VLOG(4) << "Decoder: " << decoder->name;

            if (codec->codec_type == AVMEDIA_TYPE_AUDIO)
            {
                m_contexts->codec_contexts.push_back(codec);
                m_contexts->codec_ids.push_back(i);
                m_contexts->result_stream_devices.push_back(
                            st_device(this, i));

                // in c++11 we can use emplace_back on
                // vector<stream_buffer>
                boost::shared_ptr<stream_buffer>
                    rsb(new stream_buffer);
                m_contexts->result_stream_buffers.push_back(rsb);
                rsb->open(
                    m_contexts->result_stream_devices.back());

                boost::shared_ptr<std::istream>
                    rs(new std::istream(rsb.get()));
                m_contexts->result_streams.push_back(rs);

                if (avcodec_open2(codec, decoder, NULL) < 0)
                {
                    LOG(WARNING) << "Cannot open [" << i << "]";
                    m_contexts->codec_contexts.pop_back();
                    m_contexts->codec_ids.pop_back();
                    m_contexts->result_stream_devices.pop_back();
                    m_contexts->result_stream_buffers.pop_back();
                    m_contexts->result_streams.pop_back();
                }
                else
                {
#                   if LIBAVCODEC_VERSION_MAJOR >= 54
                    AVAudioResampleContext* avr =
                            avresample_alloc_context();

                    m_contexts->resample_contexts.push_back(
                                avr);
#                   endif
                }
            }
        }
        else LOG(WARNING) << " (unknown decoder)";
    }
    m_ok = init_encoder();
}

stream_transcoder::~stream_transcoder()
{}

bool stream_transcoder::is_ok() const
{
    return this->m_ok;
}

std::istream& stream_transcoder::out(unsigned int stream_id)
{
    return *this->m_contexts->result_streams[stream_id];
}

std::streamsize
stream_transcoder::read(const unsigned int stream_id,
        char* buffer, const std::string::size_type buffer_size)
{
    if (this->m_contexts->eof) return 0;

    std::string& m_buffer = this->m_contexts->result;
    std::string::size_type& offset =
            this->m_contexts->result_offset;

    int load_result = 0;
    int make_result = 0;

    while(buffer_size > m_buffer.size() - offset)
    {
        // prepare data

        // but maybe we can't
        if (!this->is_ok()) break;

        VLOG(6) << "Decoding frame";
        load_result = this->load_frame(stream_id);

        // have something to resample?
        if (load_result < 0) break;

        VLOG(6) << "Resampling frame";
        if (!this->resample(stream_id)) break;

        VLOG(6) << "Encoding frame";
        make_result = this->make_frame();
    }

    // problems / finished?
    if (load_result < 0 || make_result < 0)
    {
        VLOG(6) << "Encoding last frames";
        if (make_result >= 0) this->make_frame(true);

        VLOG(5) << "Writting trailer";
        av_write_trailer(m_contexts->oformat);
        this->m_contexts->eof = true;
    }

    std::string::size_type to_copy =
                    m_buffer.copy(buffer, buffer_size, offset);

    offset += to_copy;

    if (offset >= this->m_contexts->max_result_offset)
    {
        VLOG(6) << "Shrinking intermediate buffer";
        m_buffer.erase(0, offset);
        offset = 0;
    }

    return to_copy;
}


std::string
stream_transcoder::sample_rate(unsigned int stream_id) const
{
    unsigned int id = this->m_contexts->codec_ids[stream_id];
    int sample_rate =
            this->m_contexts->codec_contexts[id]->sample_rate;
    return boost::lexical_cast<std::string>(sample_rate);
}


unsigned int stream_transcoder::nstreams() const
{
    return this->m_contexts->codec_ids.size();
}


bool stream_transcoder::init_input(std::istream& auin)
{
    m_contexts = make_auto_ptr(new st_contexts);

    VLOG(4) << "Registering codecs";
    av_register_all();

    VLOG(5) << "Init imput buffer";
    m_contexts->ibuffer =
        reinterpret_cast<unsigned char*>(av_malloc(
                                         m_buffer_size));
    if (m_contexts->ibuffer == NULL)
    {
        LOG(ERROR) << "Out of memory";
        return false;
    }

    VLOG(5) << "Init input I/O context";
    m_contexts->input = avio_alloc_context(m_contexts->ibuffer,
        m_buffer_size, 0, reinterpret_cast<void*>(&auin),
        read_callback, NULL, NULL);
    if (m_contexts->input == NULL)
    {
        LOG(ERROR)
                << "avio_alloc_context failed. Out of memory?";
        return false;
    }

    VLOG(5) << "Init input format context";
    m_contexts->iformat = avformat_alloc_context();
    if (m_contexts->iformat == NULL)
    {
        LOG(ERROR) << "Cannot init input format context";
        return false;
    }

    VLOG(5) << "Open input format context";
    m_contexts->iformat->pb = m_contexts->input;
    if (avformat_open_input(&m_contexts->iformat, "-stream-",
                                                NULL, NULL) < 0)
    {
        LOG(ERROR) << "Cannot open input format context";
        return false;
    }

    VLOG(5) << "Analizing stream";
    if (avformat_find_stream_info(m_contexts->iformat, NULL) < 0)
    {
        LOG(ERROR) << "Cannot find stream information";
        return false;
    }
    return true;
}

bool stream_transcoder::init_encoder()
{
    if (m_contexts->codec_contexts.size() == 0) return false;

    VLOG(5) << "Init output buffer";
    m_contexts->obuffer =
        reinterpret_cast<unsigned char*>(av_malloc(
                                             m_buffer_size));
    if (m_contexts->obuffer == NULL)
    {
        LOG(ERROR) << "Out of memory";
        return false;
    }

    VLOG(5) << "Init output I/O context";
    m_contexts->output = avio_alloc_context(m_contexts->obuffer,
        m_buffer_size, 1,
        reinterpret_cast<void*>(&this->m_contexts->result),
        NULL, write_callback, NULL);
    if (m_contexts->output == NULL)
    {
        LOG(ERROR) << "Cannot allocate output I/O context";
        return false;
    }

    VLOG(5) << "Guessing output format";
    AVOutputFormat* of = av_guess_format(
                this->m_codec_name, NULL, NULL);
    if (of == NULL)
    {
        LOG(ERROR) << "Cannot guess output format";
        return false;
    }

    VLOG(5) << "Init output format";
    m_contexts->oformat = avformat_alloc_context();
    if (m_contexts->oformat == NULL)
    {
        LOG(ERROR) << "Cannot init output format";
        return false;
    }

    m_contexts->oformat->oformat = of;
    m_contexts->oformat->pb = m_contexts->output;

    if (of->audio_codec == AV_CODEC_ID_NONE)
    {
        LOG(ERROR) << "Output audio codec is not defined";
        return false;
    }

    VLOG(5) << "Finding encoder";
    AVCodec* encoder = avcodec_find_encoder(of->audio_codec);
    if (encoder == NULL)
    {
        LOG(ERROR) << "Encoder not found";
        return false;
    }

    VLOG(5) << "Preparing output stream";
    AVStream* stream = avformat_new_stream(m_contexts->oformat,
        encoder);
    if (stream == NULL)
    {
        LOG(ERROR) << "Cannot make new output stream";
        return false;
    }

    AVCodecContext* in = m_contexts->codec_contexts[0];
    AVCodecContext* out = stream->codec;

    VLOG(5) << "Checking sample format";
    if (!check_sample_fmt(encoder, in->sample_fmt))
    {
        LOG(ERROR) << "Wrong sample format";
        return false;
    }

    out->codec_id = encoder->id;
    out->codec_type = AVMEDIA_TYPE_AUDIO;

    out->sample_fmt = in->sample_fmt;
    out->sample_rate = in->sample_rate;
    out->channels = in->channels;
    //out->bit_rate = 64000;

    if(of->flags & AVFMT_GLOBALHEADER)
    {
        out->flags |= CODEC_FLAG_GLOBAL_HEADER;
    }

    VLOG(5) << "Openning encoder";
    if (avcodec_open2(out, encoder, NULL) < 0)
    {
        LOG(ERROR) << "Cannot open encoder";
        return false;
    }

    VLOG(5) << "Writing header";
    avformat_write_header(m_contexts->oformat, NULL);

    return true;
}


int stream_transcoder::load_frame(unsigned int index)
{
    av_packet* packet = &this->m_contexts->packet;
    AVCodecContext* codec =
            this->m_contexts->codec_contexts[index];
    unsigned int stream_index =
            this->m_contexts->codec_ids[index];
    int nframe_available = 0;
    int len = 0;
    std::basic_string<uint8_t> keep;

    if (this->m_contexts->iframe == NULL)
    {
        VLOG(5) << "Init input frame";
        this->m_contexts->iframe = avcodec_alloc_frame();
        if (this->m_contexts->iframe == NULL)
        {
            LOG(ERROR) << "Cannot init input frame";
            return -2;
        }
    }

    while(nframe_available == 0)
    {
        AVPacket packet_offset = packet->get();

        // Read
        if (packet->offset() >= packet->size())
        {
            do
            {
                packet->reset(this->m_contexts->iformat);
                if (packet->get().stream_index != stream_index)
                {
                    VLOG(6) << "Skipping packet";
                    continue;
                }
                if (packet->data() == NULL)
                {
                    LOG(WARNING) << "Empty packet";
                    return -3;
                }
            }
            while(0);

            keep.insert(keep.size(),
                packet->data(), packet->size());
            packet->offset_reset();
        }
        else
        {
            keep.insert(
                keep.size(),
                packet->data() + packet->offset(),
                packet->size() - packet->offset()
            );
        }


        packet_offset.data = (uint8_t*)keep.data();
        packet_offset.size = keep.size();

        VLOG(6) << "Decoding audio frame";
        len = avcodec_decode_audio4(codec,
            this->m_contexts->iframe,
            &nframe_available, &packet_offset
        );
        keep.clear();

        if (len < 0)
        {
            LOG(WARNING) << "Decoding audio error: " << len
                         << ". skipping";

            packet->offset_add(packet->size());
            len = 0;
            nframe_available = 0;
            return -1;
        }
        if ((nframe_available == 0) && (len > 0))
        {
            VLOG(6) << "Frame not ready";
            keep.insert(keep.size(), packet_offset.data, len);
        }
        VLOG(6) << "Read decoded bytes " << len
                << " of " << packet->size() - packet->offset()
                << " frame: " << nframe_available;
        packet->offset_add(len);
    }

    return nframe_available;
}

bool stream_transcoder::resample(unsigned int index) // mock
{
    int linesize = av_samples_get_buffer_size(NULL,
        this->m_contexts->codec_contexts[index]->channels,
        this->m_contexts->iframe->nb_samples,
        this->m_contexts->codec_contexts[index]->sample_fmt,
    1);

    if (linesize < 0) return false;

    this->m_contexts->r_samples.insert(
            this->m_contexts->r_samples.size(),
            this->m_contexts->iframe->data[0],
            linesize);
    VLOG(6) << "Resampling mock: " << linesize << " bytes";

    return true;
}

int stream_transcoder::make_frame(bool force)
{
    std::basic_string<uint8_t>& buffer =
                                    this->m_contexts->r_samples;

    AVCodecContext* out =
                    this->m_contexts->oformat->streams[0]->codec;

    if (this->m_contexts->oframe == NULL)
    {
        VLOG(5) << "Init output frame";
        this->m_contexts->oframe = avcodec_alloc_frame();
        if (this->m_contexts->oframe == NULL)
        {
            LOG(ERROR) << "Cannot init output frame."
                          "Out of memory?";
            return -2;
        }

        this->m_contexts->oframe->nb_samples = out->frame_size;

        this->m_contexts->oframe->format = out->sample_fmt;
#       if LIBAVCODEC_VERSION_MAJOR >= 54
        this->m_contexts->oframe->channel_layout
                = out->channel_layout;
#       endif

        this->m_contexts->oframe_linesize = 0;
        if (av_samples_get_buffer_size(
                            &this->m_contexts->oframe_linesize,
                            out->channels, out->frame_size,
                            out->sample_fmt, 1) < 0)
        {
            LOG(ERROR) << "Cannot estimate buffer size";
            return -4;
        }
    }

    unsigned int linesize = this->m_contexts->oframe_linesize;


    if (buffer.size() == 0) return 0;

    if ((linesize > buffer.size()) && !force)
    {
        VLOG(6) << "Not enougth data to fill output frame";
        return 1;
    }

    std::basic_string<uint8_t>::size_type offset = 0;
    do
    {
        VLOG(6) << "Filling audio frame";
        if (avcodec_fill_audio_frame(this->m_contexts->oframe,
                    out->channels, out->sample_fmt,
                    buffer.data() + offset,
                    linesize, 1
                )< 0)
        {
            buffer.clear();
            return -1;
        }
        offset += linesize;

        av_packet packet;
        int got_output = 0;
        VLOG(6) << "Encoding audio frame";
        if (avcodec_encode_audio2(out, &packet.get(),
                    this->m_contexts->oframe, &got_output) < 0)
        {
            LOG(ERROR) << "Cannot encode audio frame";
            return -5;
        }

        VLOG(6) << "Writing frame";
        if (got_output &&
            av_write_frame(this->m_contexts->oformat,
                           &packet.get()) < 0)
        {
            LOG(ERROR) << "Cannot write frame";
            return -6;
        }
        if (force) break;
    }
    while(buffer.size() - offset > linesize);

    buffer.erase(0, offset);

    if (force) // flush
    {
        av_packet packet;
        int got_output = 0;
        VLOG(6) << "Encoding last audio frame";
        if (avcodec_encode_audio2(out, &packet.get(),
                    NULL, &got_output) < 0)
        {
            LOG(ERROR) << "Cannot encode audio frame";
            return 0;
        }
        VLOG_IF(6, got_output) << "Writting last frame";
        if (got_output &&
            av_write_frame(this->m_contexts->oformat,
                           &packet.get()) < 0)
        {
            LOG_IF(ERROR, got_output) << "Cannot write frame";
            return -6;
        }
    }

    return 0;
}

