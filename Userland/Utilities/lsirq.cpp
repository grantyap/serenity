/*
 * Copyright (c) 2020, Liav A. <liavalb@hotmail.co.il>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibCore/File.h>
#include <stdio.h>
#include <unistd.h>

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
    if (pledge("stdio rpath", nullptr) < 0) {
        perror("pledge");
        return 1;
    }

    if (unveil("/proc/interrupts", "r") < 0) {
        perror("unveil");
        return 1;
    }

    unveil(nullptr, nullptr);

    auto proc_interrupts = Core::File::construct("/proc/interrupts");
    if (!proc_interrupts->open(Core::OpenMode::ReadOnly)) {
        fprintf(stderr, "Error: %s\n", proc_interrupts->error_string());
        return 1;
    }

    if (pledge("stdio", nullptr) < 0) {
        perror("pledge");
        return 1;
    }

    printf("%4s  %-10s\n", " ", "CPU0");
    auto file_contents = proc_interrupts->read_all();
    auto json = JsonValue::from_string(file_contents);
    VERIFY(json.has_value());
    json.value().as_array().for_each([](auto& value) {
        auto handler = value.as_object();
        auto purpose = handler.get("purpose").to_string();
        auto interrupt = handler.get("interrupt_line").to_string();
        auto controller = handler.get("controller").to_string();
        auto call_count = handler.get("call_count").to_string();

        printf("%4s: %-10s %-10s  %-30s\n",
            interrupt.characters(), call_count.characters(), controller.characters(), purpose.characters());
    });

    return 0;
}
