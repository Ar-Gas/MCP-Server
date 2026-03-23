/*
 * Patched replacement for seastar/src/http/common.cc
 *
 * Fix: http_chunked_data_sink_impl::flush() was missing (inherited the no-op
 * data_sink_impl::flush()), causing SSE streaming to buffer indefinitely and
 * never reach the client. This patch adds:
 *
 *   virtual future<> flush() override { return _out.flush(); }
 *
 * By defining all strong symbols from common.cc here, the linker will not
 * extract common.cc.o from libseastar.a, so this patched version takes effect.
 */

#include <cstdlib>
#include <memory>
#include <utility>
#include <numeric>
#include <span>

#include <seastar/http/common.hh>
#include <seastar/core/iostream-impl.hh>

namespace seastar {

namespace httpd {

operation_type str2type(const sstring& type) {
    if (type == "DELETE") {
        return DELETE;
    }
    if (type == "POST") {
        return POST;
    }
    if (type == "PUT") {
        return PUT;
    }
    if (type == "HEAD") {
        return HEAD;
    }
    if (type == "OPTIONS") {
        return OPTIONS;
    }
    if (type == "TRACE") {
        return TRACE;
    }
    if (type == "CONNECT") {
        return CONNECT;
    }
    if (type == "PATCH") {
        return PATCH;
    }
    return GET;
}

sstring type2str(operation_type type) {
    if (type == DELETE) {
        return "DELETE";
    }
    if (type == POST) {
        return "POST";
    }
    if (type == PUT) {
        return "PUT";
    }
    if (type == HEAD) {
        return "HEAD";
    }
    if (type == OPTIONS) {
        return "OPTIONS";
    }
    if (type == TRACE) {
        return "TRACE";
    }
    if (type == CONNECT) {
        return "CONNECT";
    }
    if (type == PATCH) {
        return "PATCH";
    }
    return "GET";
}

}

namespace http {
namespace internal {

static constexpr size_t default_body_sink_buffer_size = 32000;

// Data sinks below are running "on top" of socket output stream and provide
// reliable and handy way of generating request bodies according to selected
// encoding type and content-length.
//
// Respectively, both .close() methods should not close the underlying stream,
// because the socket in question may continue being in use for keep-alive
// connections, and closing it would just break the keep-alive-ness

class http_chunked_data_sink_impl : public data_sink_impl {
    output_stream<char>& _out;

    future<> write_size(size_t s) {
        auto req = format("{:x}\r\n", s);
        return _out.write(req);
    }
public:
    http_chunked_data_sink_impl(output_stream<char>& out) : _out(out) {
    }
#if SEASTAR_API_LEVEL >= 9
    future<> put(std::span<temporary_buffer<char>> data) override {
        return data_sink_impl::fallback_put(data, [this] (temporary_buffer<char>&& buf) {
            return do_put(std::move(buf));
        });
    }
#else
    virtual future<> put(net::packet data) override {
        return data_sink_impl::fallback_put(std::move(data));
    }
    using data_sink_impl::put;
    virtual future<> put(temporary_buffer<char> buf) override {
        return do_put(std::move(buf));
    }
#endif
    // FIX: propagate flush to the underlying connection output stream so that
    // SSE events and other streamed data are actually sent to the client.
    virtual future<> flush() override {
        return _out.flush();
    }
private:
    future<> do_put(temporary_buffer<char> buf) {
        if (buf.size() == 0) {
            // size 0 buffer should be ignored, some server
            // may consider it an end of message
            return make_ready_future<>();
        }
        auto size = buf.size();
        return write_size(size).then([this, buf = std::move(buf)] () mutable {
            return _out.write(buf.get(), buf.size());
        }).then([this] () mutable {
            return _out.write("\r\n", 2);
        });
    }
    virtual future<> close() override {
        return  make_ready_future<>();
    }
};

class http_chunked_data_sink : public data_sink {
public:
    http_chunked_data_sink(output_stream<char>& out)
        : data_sink(std::make_unique<http_chunked_data_sink_impl>(
                out)) {}
};

output_stream<char> make_http_chunked_output_stream(output_stream<char>& out) {
    output_stream_options opts;
    opts.trim_to_size = true;
    return output_stream<char>(http_chunked_data_sink(out), default_body_sink_buffer_size, opts);
}

class http_content_length_data_sink_impl : public data_sink_impl {
    output_stream<char>& _out;
    const size_t _limit;
    size_t& _bytes_written;

public:
    http_content_length_data_sink_impl(output_stream<char>& out, size_t total_len, size_t& bytes_written)
            : _out(out)
            , _limit(total_len)
            , _bytes_written(bytes_written)
    {
        // at the very beginning, 0 bytes were written
        _bytes_written = 0;
    }
#if SEASTAR_API_LEVEL >= 9
    future<> put(std::span<temporary_buffer<char>> data) override {
        size_t size = std::accumulate(data.begin(), data.end(), size_t(0), [] (size_t s, const auto& b) { return s + b.size(); });
        if (size == 0) {
            return make_ready_future<>();
        }
        if (_bytes_written + size > _limit) {
            return make_exception_future<>(std::runtime_error(format("body content length overflow: want {} limit {}", _bytes_written + size, _limit)));
        }
        return _out.write(data).then([this, size] {
            _bytes_written += size;
        });
    }
#else
    virtual future<> put(net::packet data) override {
        auto size = data.len();
        if (size == 0) {
            return make_ready_future<>();
        }
        if (_bytes_written + size > _limit) {
            return make_exception_future<>(std::runtime_error(format("body content length overflow: want {} limit {}", _bytes_written + size, _limit)));
        }
        return _out.write(std::move(data)).then([this, size] {
            _bytes_written += size;
        });
    }
    using data_sink_impl::put;
    virtual future<> put(temporary_buffer<char> buf) override {
        auto size = buf.size();
        if (size == 0) {
            return make_ready_future<>();
        }

        if (_bytes_written + size > _limit) {
            return make_exception_future<>(std::runtime_error(format("body content length overflow: want {} limit {}", _bytes_written + buf.size(), _limit)));
        }

        return _out.write(buf.get(), size).then([this, size] {
            _bytes_written += size;
        });
    }
#endif
    virtual future<> close() override {
        return make_ready_future<>();
    }
};

class http_content_length_data_sink : public data_sink {
public:
    http_content_length_data_sink(output_stream<char>& out, size_t total_len, size_t& bytes_written)
        : data_sink(std::make_unique<http_content_length_data_sink_impl>(out, total_len, bytes_written))
    {
    }
};

output_stream<char> make_http_content_length_output_stream(output_stream<char>& out, size_t total_len, size_t& bytes_written) {
    output_stream_options opts;
    opts.trim_to_size = true;
    return output_stream<char>(http_content_length_data_sink(out, total_len, bytes_written), default_body_sink_buffer_size, opts);
}

}
}

}
