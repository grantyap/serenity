/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibCore/IODevice.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace Core {

IODevice::IODevice(Object* parent)
    : Object(parent)
{
}

IODevice::~IODevice()
{
}

const char* IODevice::error_string() const
{
    return strerror(m_error);
}

int IODevice::read(u8* buffer, int length)
{
    auto read_buffer = read(length);
    memcpy(buffer, read_buffer.data(), length);
    return read_buffer.size();
}

ByteBuffer IODevice::read(size_t max_size)
{
    if (m_fd < 0)
        return {};
    if (!max_size)
        return {};
    auto buffer = ByteBuffer::create_uninitialized(max_size);
    auto* buffer_ptr = (char*)buffer.data();
    size_t remaining_buffer_space = buffer.size();
    size_t taken_from_buffered = 0;
    if (!m_buffered_data.is_empty()) {
        taken_from_buffered = min(remaining_buffer_space, m_buffered_data.size());
        memcpy(buffer_ptr, m_buffered_data.data(), taken_from_buffered);
        Vector<u8> new_buffered_data;
        new_buffered_data.append(m_buffered_data.data() + taken_from_buffered, m_buffered_data.size() - taken_from_buffered);
        m_buffered_data = move(new_buffered_data);
        remaining_buffer_space -= taken_from_buffered;
        buffer_ptr += taken_from_buffered;
    }
    if (!remaining_buffer_space)
        return buffer;
    int nread = ::read(m_fd, buffer_ptr, remaining_buffer_space);
    if (nread < 0) {
        if (taken_from_buffered) {
            buffer.trim(taken_from_buffered);
            return buffer;
        }
        set_error(errno);
        return {};
    }
    if (nread == 0) {
        set_eof(true);
        if (taken_from_buffered) {
            buffer.trim(taken_from_buffered);
            return buffer;
        }
        return {};
    }
    buffer.trim(taken_from_buffered + nread);
    return buffer;
}

bool IODevice::can_read_from_fd() const
{
    // FIXME: Can we somehow remove this once Core::Socket is implemented using non-blocking sockets?
    fd_set rfds {};
    FD_ZERO(&rfds);
    FD_SET(m_fd, &rfds);
    struct timeval timeout {
        0, 0
    };

    for (;;) {
        if (select(m_fd + 1, &rfds, nullptr, nullptr, &timeout) < 0) {
            if (errno == EINTR)
                continue;
            perror("IODevice::can_read_from_fd: select");
            return false;
        }
        break;
    }
    return FD_ISSET(m_fd, &rfds);
}

bool IODevice::can_read_line() const
{
    if (m_eof && !m_buffered_data.is_empty())
        return true;
    if (m_buffered_data.contains_slow('\n'))
        return true;
    if (!can_read_from_fd())
        return false;
    populate_read_buffer();
    if (m_eof && !m_buffered_data.is_empty())
        return true;
    return m_buffered_data.contains_slow('\n');
}

bool IODevice::can_read() const
{
    return !m_buffered_data.is_empty() || can_read_from_fd();
}

ByteBuffer IODevice::read_all()
{
    off_t file_size = 0;
    struct stat st;
    int rc = fstat(fd(), &st);
    if (rc == 0)
        file_size = st.st_size;

    Vector<u8> data;
    data.ensure_capacity(file_size);

    if (!m_buffered_data.is_empty()) {
        data.append(m_buffered_data.data(), m_buffered_data.size());
        m_buffered_data.clear();
    }

    while (true) {
        char read_buffer[4096];
        int nread = ::read(m_fd, read_buffer, sizeof(read_buffer));
        if (nread < 0) {
            set_error(errno);
            break;
        }
        if (nread == 0) {
            set_eof(true);
            break;
        }
        data.append((const u8*)read_buffer, nread);
    }
    return ByteBuffer::copy(data.data(), data.size());
}

String IODevice::read_line(size_t max_size)
{
    if (m_fd < 0)
        return {};
    if (!max_size)
        return {};
    if (!can_read_line())
        return {};
    if (m_eof) {
        if (m_buffered_data.size() > max_size) {
            dbgln("IODevice::read_line: At EOF but there's more than max_size({}) buffered", max_size);
            return {};
        }
        auto line = String((const char*)m_buffered_data.data(), m_buffered_data.size(), Chomp);
        m_buffered_data.clear();
        return line;
    }
    auto line = ByteBuffer::create_uninitialized(max_size + 1);
    size_t line_index = 0;
    while (line_index < max_size) {
        u8 ch = m_buffered_data[line_index];
        line[line_index++] = ch;
        if (ch == '\n') {
            Vector<u8> new_buffered_data;
            new_buffered_data.append(m_buffered_data.data() + line_index, m_buffered_data.size() - line_index);
            m_buffered_data = move(new_buffered_data);
            line.trim(line_index);
            return String::copy(line, Chomp);
        }
    }
    return {};
}

bool IODevice::populate_read_buffer() const
{
    if (m_fd < 0)
        return false;
    u8 buffer[1024];
    int nread = ::read(m_fd, buffer, sizeof(buffer));
    if (nread < 0) {
        set_error(errno);
        return false;
    }
    if (nread == 0) {
        set_eof(true);
        return false;
    }
    m_buffered_data.append(buffer, nread);
    return true;
}

bool IODevice::close()
{
    if (fd() < 0 || m_mode == OpenMode::NotOpen)
        return false;
    int rc = ::close(fd());
    if (rc < 0) {
        set_error(errno);
        return false;
    }
    set_fd(-1);
    set_mode(OpenMode::NotOpen);
    return true;
}

bool IODevice::seek(i64 offset, SeekMode mode, off_t* pos)
{
    int m = SEEK_SET;
    switch (mode) {
    case SeekMode::SetPosition:
        m = SEEK_SET;
        break;
    case SeekMode::FromCurrentPosition:
        m = SEEK_CUR;
        break;
    case SeekMode::FromEndPosition:
        m = SEEK_END;
        break;
    }
    off_t rc = lseek(m_fd, offset, m);
    if (rc < 0) {
        set_error(errno);
        if (pos)
            *pos = -1;
        return false;
    }
    m_buffered_data.clear();
    m_eof = false;
    if (pos)
        *pos = rc;
    return true;
}

bool IODevice::truncate(off_t size)
{
    int rc = ftruncate(m_fd, size);
    if (rc < 0) {
        set_error(errno);
        return false;
    }
    return true;
}

bool IODevice::write(const u8* data, int size)
{
    int rc = ::write(m_fd, data, size);
    if (rc < 0) {
        set_error(errno);
        perror("IODevice::write: write");
        return false;
    }
    return rc == size;
}

void IODevice::set_fd(int fd)
{
    if (m_fd == fd)
        return;

    m_fd = fd;
    did_update_fd(fd);
}

bool IODevice::write(const StringView& v)
{
    return write((const u8*)v.characters_without_null_termination(), v.length());
}

LineIterator::LineIterator(IODevice& device, bool is_end)
    : m_device(device)
    , m_is_end(is_end)
{
    ++*this;
}

bool LineIterator::at_end() const
{
    return m_device->eof();
}

LineIterator& LineIterator::operator++()
{
    m_buffer = m_device->read_line();
    return *this;
}
}
