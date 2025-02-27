/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2020, Sergey Bugaev <bugaevc@serenityos.org>
 * Copyright (c) 2021, Liav A. <liavalb@hotmail.co.il>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "VirtualConsole.h"
#include <AK/StdLibExtras.h>
#include <AK/String.h>
#include <Kernel/Arch/x86/CPU.h>
#include <Kernel/Debug.h>
#include <Kernel/Devices/HID/HIDManagement.h>
#include <Kernel/Graphics/GraphicsManagement.h>
#include <Kernel/Heap/kmalloc.h>
#include <Kernel/IO.h>
#include <Kernel/StdLib.h>
#include <Kernel/TTY/ConsoleManagement.h>

namespace Kernel {

ConsoleImpl::ConsoleImpl(VirtualConsole& client)
    : Terminal(client)
{
}

void ConsoleImpl::invalidate_cursor()
{
}
void ConsoleImpl::clear()
{
    m_client.clear();
}
void ConsoleImpl::clear_including_history()
{
}

void ConsoleImpl::set_size(u16 determined_columns, u16 determined_rows)
{
    VERIFY(determined_columns);
    VERIFY(determined_rows);

    if (determined_columns == columns() && determined_rows == rows())
        return;

    m_columns = determined_columns;
    m_rows = determined_rows;

    m_cursor_row = min<size_t>((int)m_cursor_row, rows() - 1);
    m_cursor_column = min<size_t>((int)m_cursor_column, columns() - 1);
    m_saved_cursor_row = min<size_t>((int)m_saved_cursor_row, rows() - 1);
    m_saved_cursor_column = min<size_t>((int)m_saved_cursor_column, columns() - 1);

    m_horizontal_tabs.resize(determined_columns);
    for (unsigned i = 0; i < determined_columns; ++i)
        m_horizontal_tabs[i] = (i % 8) == 0;
    // Rightmost column is always last tab on line.
    m_horizontal_tabs[determined_columns - 1] = 1;
    m_client.terminal_did_resize(m_columns, m_rows);
}
void ConsoleImpl::scroll_up()
{
    // NOTE: We have to invalidate the cursor first.
    m_client.invalidate_cursor(m_cursor_row);
    m_client.scroll_up();
}
void ConsoleImpl::scroll_down()
{
}
void ConsoleImpl::newline()
{
    u16 new_row = m_cursor_row;
    u16 max_row = rows() - 1;
    if (new_row == max_row) {
        // NOTE: We have to invalidate the cursor first.
        m_client.invalidate_cursor(new_row);
        m_client.scroll_up();
    } else {
        ++new_row;
    }
    set_cursor(new_row, 0);
}
void ConsoleImpl::put_character_at(unsigned row, unsigned column, u32 ch)
{
    m_client.put_character_at(row, column, ch, m_current_attribute);
    m_last_code_point = ch;
}
void ConsoleImpl::set_window_title(const String&)
{
}
void ConsoleImpl::ICH(Parameters)
{
    // FIXME: Implement this
}
void ConsoleImpl::IL(Parameters)
{
    // FIXME: Implement this
}
void ConsoleImpl::DCH(Parameters)
{
    // FIXME: Implement this
}
void ConsoleImpl::DL(Parameters)
{
    // FIXME: Implement this
}

void VirtualConsole::set_graphical(bool graphical)
{
    m_graphical = graphical;
}

UNMAP_AFTER_INIT NonnullRefPtr<VirtualConsole> VirtualConsole::create(size_t index)
{
    return adopt_ref(*new VirtualConsole(index));
}

UNMAP_AFTER_INIT NonnullRefPtr<VirtualConsole> VirtualConsole::create_with_preset_log(size_t index, const CircularQueue<char, 16384>& log)
{
    return adopt_ref(*new VirtualConsole(index, log));
}

UNMAP_AFTER_INIT void VirtualConsole::initialize()
{
    m_tty_name = String::formatted("/dev/tty{}", m_index);
    VERIFY(GraphicsManagement::the().console());
    set_size(GraphicsManagement::the().console()->max_column(), GraphicsManagement::the().console()->max_row());
    m_console_impl.set_size(GraphicsManagement::the().console()->max_column(), GraphicsManagement::the().console()->max_row());

    // Allocate twice of the max row * max column * sizeof(Cell) to ensure we can some sort of history mechanism...
    auto size = GraphicsManagement::the().console()->max_column() * GraphicsManagement::the().console()->max_row() * sizeof(Cell) * 2;
    m_cells = MM.allocate_kernel_region(page_round_up(size), "Virtual Console Cells", Region::Access::Read | Region::Access::Write, AllocationStrategy::AllocateNow);

    // Add the lines, so we also ensure they will be flushed now
    for (size_t row = 0; row < rows(); row++) {
        m_lines.append({ true });
    }
    clear();
    VERIFY(m_cells);
}

UNMAP_AFTER_INIT VirtualConsole::VirtualConsole(const unsigned index)
    : TTY(4, index)
    , m_index(index)
    , m_console_impl(*this)
{
    initialize();
}

UNMAP_AFTER_INIT VirtualConsole::VirtualConsole(const unsigned index, const CircularQueue<char, 16384>& log)
    : TTY(4, index)
    , m_index(index)
    , m_console_impl(*this)
{
    initialize();
    for (auto& ch : log) {
        echo(ch);
    }
}

UNMAP_AFTER_INIT VirtualConsole::~VirtualConsole()
{
    VERIFY_NOT_REACHED();
}

enum class ANSIColor : u8 {
    Black = 0,
    Red,
    Green,
    Brown,
    Blue,
    Magenta,
    Cyan,
    LightGray,
    DarkGray,
    BrightRed,
    BrightGreen,
    Yellow,
    BrightBlue,
    BrightMagenta,
    BrightCyan,
    White,
    __Count,
};

static inline Graphics::Console::Color ansi_color_to_standard_vga_color(ANSIColor color)
{
    switch (color) {
    case ANSIColor::Black:
        return Graphics::Console::Color::Black;
    case ANSIColor::Red:
        return Graphics::Console::Color::Red;
    case ANSIColor::Brown:
        return Graphics::Console::Color::Brown;
    case ANSIColor::Blue:
        return Graphics::Console::Color::Blue;
    case ANSIColor::Magenta:
        return Graphics::Console::Color::Magenta;
    case ANSIColor::Green:
        return Graphics::Console::Color::Green;
    case ANSIColor::Cyan:
        return Graphics::Console::Color::Cyan;
    case ANSIColor::LightGray:
        return Graphics::Console::Color::LightGray;
    case ANSIColor::DarkGray:
        return Graphics::Console::Color::DarkGray;
    case ANSIColor::BrightRed:
        return Graphics::Console::Color::BrightRed;
    case ANSIColor::BrightGreen:
        return Graphics::Console::Color::BrightGreen;
    case ANSIColor::Yellow:
        return Graphics::Console::Color::Yellow;
    case ANSIColor::BrightBlue:
        return Graphics::Console::Color::BrightBlue;
    case ANSIColor::BrightMagenta:
        return Graphics::Console::Color::BrightMagenta;
    case ANSIColor::BrightCyan:
        return Graphics::Console::Color::BrightCyan;
    case ANSIColor::White:
        return Graphics::Console::Color::White;
    default:
        VERIFY_NOT_REACHED();
    }
}

static inline Graphics::Console::Color xterm_to_standard_color(u32 color)
{
    for (u8 i = 0; i < (u8)ANSIColor::__Count; i++) {
        if (xterm_colors[i] == color)
            return (Graphics::Console::Color)ansi_color_to_standard_vga_color((ANSIColor)i);
    }
    return Graphics::Console::Color::LightGray;
}

void VirtualConsole::on_key_pressed(KeyEvent event)
{
    // Ignore keyboard in graphical mode.
    if (m_graphical)
        return;

    if (!event.is_press())
        return;

    Processor::deferred_call_queue([this, event]() {
        m_console_impl.handle_key_press(event.key, event.code_point, event.flags);
    });
}

ssize_t VirtualConsole::on_tty_write(const UserOrKernelBuffer& data, ssize_t size)
{
    ScopedSpinLock global_lock(ConsoleManagement::the().tty_write_lock());
    ScopedSpinLock lock(m_lock);
    auto result = data.read_buffered<512>((size_t)size, [&](u8 const* buffer, size_t buffer_bytes) {
        for (size_t i = 0; i < buffer_bytes; ++i)
            m_console_impl.on_input(buffer[i]);
        return buffer_bytes;
    });
    if (m_active)
        flush_dirty_lines();
    if (result.is_error())
        return result.error();
    return (ssize_t)result.value();
}

void VirtualConsole::set_active(bool active)
{
    VERIFY(ConsoleManagement::the().m_lock.is_locked());
    VERIFY(m_active != active);
    m_active = active;

    if (active) {
        HIDManagement::the().set_client(this);

        m_console_impl.m_need_full_flush = true;
        flush_dirty_lines();
    } else {
        HIDManagement::the().set_client(nullptr);
    }
}

void VirtualConsole::emit_char(char ch)
{
    echo(ch);
}

void VirtualConsole::flush_dirty_lines()
{
    VERIFY(GraphicsManagement::is_initialized());
    VERIFY(GraphicsManagement::the().console());
    for (u16 visual_row = 0; visual_row < rows(); ++visual_row) {
        auto& line = m_lines[visual_row];
        if (!line.dirty && !m_console_impl.m_need_full_flush)
            continue;
        for (size_t column = 0; column < columns(); ++column) {
            auto& cell = cell_at(column, visual_row);

            auto foreground_color = xterm_to_standard_color(cell.attribute.effective_foreground_color());
            if (cell.attribute.flags & VT::Attribute::Flags::Bold)
                foreground_color = (Graphics::Console::Color)((u8)foreground_color | 0x08);
            GraphicsManagement::the().console()->write(column,
                visual_row,
                ((u8)cell.ch < 128 ? cell.ch : '?'),
                xterm_to_standard_color(cell.attribute.effective_background_color()),
                foreground_color);
        }
        line.dirty = false;
    }
    GraphicsManagement::the().console()->set_cursor(m_console_impl.cursor_column(), m_console_impl.cursor_row());
    m_console_impl.m_need_full_flush = false;
}

void VirtualConsole::beep()
{
    // TODO
    dbgln("Beep!1");
}

void VirtualConsole::set_window_title(const StringView&)
{
    // Do nothing.
}

void VirtualConsole::set_window_progress(int, int)
{
    // Do nothing.
}

void VirtualConsole::terminal_did_resize(u16 columns, u16 rows)
{
    // FIXME: Allocate more Region(s) or deallocate them if needed...
    dbgln("VC {}: Resized to {} x {}", index(), columns, rows);
}

void VirtualConsole::terminal_history_changed()
{
    // Do nothing, I guess?
}

void VirtualConsole::emit(const u8* data, size_t size)
{
    for (size_t i = 0; i < size; i++)
        TTY::emit(data[i], true);
}

String VirtualConsole::device_name() const
{
    return String::formatted("tty{}", minor());
}

void VirtualConsole::echo(u8 ch)
{
    if (should_echo_input()) {
        auto buffer = UserOrKernelBuffer::for_kernel_buffer(&ch);
        on_tty_write(buffer, 1);
    }
}

VirtualConsole::Cell& VirtualConsole::cell_at(size_t x, size_t y)
{
    auto* ptr = (VirtualConsole::Cell*)(m_cells->vaddr().as_ptr());
    ptr += (y * columns()) + x;
    return *ptr;
}

void VirtualConsole::clear()
{
    auto* cell = (Cell*)m_cells->vaddr().as_ptr();
    for (size_t y = 0; y < rows(); y++) {
        m_lines[y].dirty = true;
        for (size_t x = 0; x < columns(); x++) {
            cell[x].clear();
        }
        cell += columns();
    }
    m_console_impl.set_cursor(0, 0);
}

void VirtualConsole::scroll_up()
{
    memmove(m_cells->vaddr().as_ptr(), m_cells->vaddr().offset(columns() * sizeof(Cell)).as_ptr(), ((rows() - 1) * columns() * sizeof(Cell)));
    clear_line(rows() - 1);
    m_console_impl.m_need_full_flush = true;
}

void VirtualConsole::newline()
{
}

void VirtualConsole::clear_line(size_t y_index)
{
    m_lines[y_index].dirty = true;
    for (size_t x = 0; x < columns(); x++) {
        auto& cell = cell_at(x, y_index);
        cell.clear();
    }
}

void VirtualConsole::put_character_at(unsigned row, unsigned column, u32 code_point, const VT::Attribute& attribute)
{
    VERIFY(row < rows());
    VERIFY(column < columns());
    auto& line = m_lines[row];
    auto& cell = cell_at(column, row);
    cell.attribute.foreground_color = attribute.foreground_color;
    cell.attribute.background_color = attribute.background_color;
    cell.attribute.flags = attribute.flags;
    if (code_point > 128)
        cell.ch = ' ';
    else
        cell.ch = code_point;
    cell.attribute.flags |= VT::Attribute::Flags::Touched;
    line.dirty = true;
}

void VirtualConsole::invalidate_cursor(size_t row)
{
    m_lines[row].dirty = true;
}

}
