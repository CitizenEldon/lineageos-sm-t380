// SPDX-License-Identifier: Apache-2.0
#include "Backlight.h"
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
namespace t380 {
int brightnessForColor(uint32_t color, int maximum) {
    const uint32_t level = (77 * ((color >> 16) & 255)
            + 150 * ((color >> 8) & 255) + 29 * (color & 255)) >> 8;
    return static_cast<int>((static_cast<uint64_t>(level) * maximum + 127) / 255);
}
int readMaximum(const std::string& path, int* maximum) {
    int fd = TEMP_FAILURE_RETRY(open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd < 0) return errno;
    char data[32] = {};
    const ssize_t count = TEMP_FAILURE_RETRY(read(fd, data, sizeof(data) - 1));
    const int savedError = errno;
    close(fd);
    if (count < 0) return savedError;
    if (count == 0 || count == sizeof(data) - 1) return EINVAL;
    char* end;
    errno = 0;
    long value = strtol(data, &end, 10);
    if (errno || end == data || value < 1 || value > 65535) return EINVAL;
    while (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t') ++end;
    if (*end) return EINVAL;
    *maximum = static_cast<int>(value);
    return 0;
}
int writeBrightness(const std::string& path, int brightness) {
    const std::string value = std::to_string(brightness) + "\n";
    int fd = TEMP_FAILURE_RETRY(open(path.c_str(), O_WRONLY | O_CLOEXEC | O_TRUNC));
    if (fd < 0) return errno;
    const ssize_t count = TEMP_FAILURE_RETRY(write(fd, value.data(), value.size()));
    int result = count < 0 ? errno : (count == static_cast<ssize_t>(value.size()) ? 0 : EIO);
    if (close(fd) != 0 && result == 0) result = errno;
    return result;
}
}
