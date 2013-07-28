#ifndef STREAM_TRANSCODER_HPP
#define STREAM_TRANSCODER_HPP

#include <istream>
#include <memory>
#include <string>


struct st_contexts;


class stream_transcoder
{
public:
    stream_transcoder(std::istream& auin,
                      const char* codec_name="flac");
    stream_transcoder(const stream_transcoder&);
    ~stream_transcoder();

    bool is_ok() const;

    std::istream& out(unsigned int stream_id);

    std::streamsize read(const unsigned int stream_id,
        char* buffer, const std::string::size_type buffer_size);

    std::string sample_rate(unsigned int stream_id) const;
    unsigned int nstreams() const;

private:
    bool m_ok;
    const char* m_codec_name;

    const std::streamsize m_buffer_size;
    std::auto_ptr<st_contexts> m_contexts;

    bool init_input(std::istream& auin);
    bool init_encoder();

    int load_frame(unsigned int);
    bool resample(unsigned int);
    int make_frame(bool force=false);
};

#endif // STREAM_TRANSCODER_HPP
